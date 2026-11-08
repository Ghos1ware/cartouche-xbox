// Dawn's platform for the app: its worker threads (pipelines compiled in parallel) and a cache on disk of
// what it compiles (shaders, pipelines), in LocalState\dawn-cache: a game loaded twice compiles nothing the
// second time. Built with GpuBridge.cpp (standard C++20, no /ZW, no precompiled header).
//
// webgpu_dawn.dll does not export dawn::platform's classes, so their few base definitions are here, as in
// Dawn's own src/dawn/platform/DawnPlatform.cpp (same headers, same compiler: the same layout).
#define WGPU_SHARED_LIBRARY
#include "DawnPlatform.h"
#include <dawn/platform/DawnPlatform.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dawn::platform {
	CachingInterface::CachingInterface() = default;
	CachingInterface::~CachingInterface() = default;
	Platform::Platform() = default;
	Platform::~Platform() = default;
	const unsigned char* Platform::GetTraceCategoryEnabledFlag(TraceCategory) {
		static unsigned char disabled = 0;
		return &disabled;
	}
	double Platform::MonotonicallyIncreasingTime() {
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	uint64_t Platform::AddTraceEvent(char, const unsigned char*, const char*, uint64_t, double, int, const char**, const unsigned char*, const uint64_t*, unsigned char) { return 0; }
	void Platform::HistogramCustomCounts(const char*, int, int, int, int) {}
	void Platform::HistogramCustomCountsHPC(const char*, int, int, int, int) {}
	void Platform::HistogramEnumeration(const char*, int, int) {}
	void Platform::HistogramSparse(const char*, int) {}
	void Platform::HistogramBoolean(const char*, bool) {}
	CachingInterface* Platform::GetCachingInterface() { return nullptr; }
	std::unique_ptr<WorkerTaskPool> Platform::CreateWorkerTaskPool() { return nullptr; }
	bool Platform::IsFeatureEnabled(Features feature) {
		switch (feature) {
		case Features::kWebGPUUseDXC: return false;
		default: return true;
		}
	}
	void Platform::ReportProgress() {}
	std::unique_ptr<JobHandle> WorkerTaskPool::PostWorkerJob(PostWorkerJobCallback, void*) { return nullptr; }
}

namespace moonlight_xbox_dx {
	namespace {
		using namespace dawn::platform;

		// ---------------------------------------------------------------- worker threads
		class Event final : public WaitableEvent {
		public:
			void Signal() {
				std::lock_guard<std::mutex> hold(lock_);
				done_ = true;
				cv_.notify_all();
			}
			void Wait() override {
				std::unique_lock<std::mutex> hold(lock_);
				cv_.wait(hold, [this] { return done_; });
			}
			bool IsComplete() override {
				std::lock_guard<std::mutex> hold(lock_);
				return done_;
			}

		private:
			std::mutex lock_;
			std::condition_variable cv_;
			bool done_ = false;
		};

		class Pool;
		class Job final : public JobHandle {
		public:
			explicit Job(std::shared_ptr<std::atomic<int>> state, std::shared_ptr<Event> finished) : state_(std::move(state)), finished_(std::move(finished)) {}
			void Cancel() override { state_->store(1); }
			void Join() override { finished_->Wait(); }

		private:
			std::shared_ptr<std::atomic<int>> state_; // 0 running, 1 cancel asked
			std::shared_ptr<Event> finished_;
		};

		class Pool final : public WorkerTaskPool {
		public:
			explicit Pool(unsigned count) {
				for (unsigned i = 0; i < count; i++) threads_.emplace_back([this] { Run(); });
			}
			~Pool() override {
				{
					std::lock_guard<std::mutex> hold(lock_);
					stopping_ = true;
				}
				cv_.notify_all();
				for (auto& t : threads_) t.join();
			}
			std::unique_ptr<WaitableEvent> PostWorkerTask(PostWorkerTaskCallback callback, void* userdata) override {
				auto event = std::make_shared<Event>();
				Post([callback, userdata, event] {
					callback(userdata);
					event->Signal();
				});
				// The caller's handle on the shared event.
				struct Handle final : WaitableEvent {
					std::shared_ptr<Event> e;
					void Wait() override { e->Wait(); }
					bool IsComplete() override { return e->IsComplete(); }
				};
				auto handle = std::make_unique<Handle>();
				handle->e = event;
				return handle;
			}
			std::unique_ptr<JobHandle> PostWorkerJob(PostWorkerJobCallback callback, void* userdata) override {
				auto state = std::make_shared<std::atomic<int>>(0);
				auto finished = std::make_shared<Event>();
				Post([this, callback, userdata, state, finished] { Step(callback, userdata, state, finished); });
				return std::make_unique<Job>(state, finished);
			}

		private:
			void Step(PostWorkerJobCallback callback, void* userdata, std::shared_ptr<std::atomic<int>> state, std::shared_ptr<Event> finished) {
				if (state->load() == 1 || callback(userdata) != JobStatus::Continue) {
					finished->Signal();
					return;
				}
				Post([this, callback, userdata, state, finished] { Step(callback, userdata, state, finished); });
			}
			void Post(std::function<void()> task) {
				{
					std::lock_guard<std::mutex> hold(lock_);
					tasks_.push_back(std::move(task));
				}
				cv_.notify_one();
			}
			void Run() {
				while (true) {
					std::function<void()> task;
					{
						std::unique_lock<std::mutex> hold(lock_);
						cv_.wait(hold, [this] { return stopping_ || !tasks_.empty(); });
						if (tasks_.empty()) return;
						task = std::move(tasks_.front());
						tasks_.pop_front();
					}
					task();
				}
			}
			std::vector<std::thread> threads_;
			std::deque<std::function<void()>> tasks_;
			std::mutex lock_;
			std::condition_variable cv_;
			bool stopping_ = false;
		};

		// ---------------------------------------------------------------- the cache on disk
		// One file per key, named by its hash; the key itself at its head (a collision reads as a miss).
		class DiskCache final : public CachingInterface {
		public:
			explicit DiskCache(std::wstring folder) : folder_(std::move(folder)) { CreateDirectoryW(folder_.c_str(), nullptr); }
			size_t FindKey(std::span<const std::byte> key) override {
				std::vector<std::byte> data;
				return Read(key, data) ? data.size() : 0;
			}
			size_t LoadData(std::span<const std::byte> key, std::span<std::byte> dest) override {
				std::vector<std::byte> data;
				if (!Read(key, data)) return 0;
				if (dest.data() && dest.size() >= data.size()) memcpy(dest.data(), data.data(), data.size());
				hits_++;
				return data.size();
			}
			void StoreData(std::span<const std::byte> key, std::span<const std::byte> src) override {
				std::lock_guard<std::mutex> hold(lock_);
				auto path = PathOf(key);
				auto partial = path + L".part";
				{
					std::ofstream out(partial, std::ios::binary | std::ios::trunc);
					uint32_t keyLength = static_cast<uint32_t>(key.size());
					out.write(reinterpret_cast<const char*>(&keyLength), 4);
					out.write(reinterpret_cast<const char*>(key.data()), key.size());
					out.write(reinterpret_cast<const char*>(src.data()), src.size());
					if (!out) return;
				}
				MoveFileExW(partial.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
				stores_++;
			}
			std::atomic<uint64_t> hits_{ 0 }, stores_{ 0 };

		private:
			std::wstring PathOf(std::span<const std::byte> key) const {
				uint64_t h = 1469598103934665603ull;
				for (auto b : key) h = (h ^ static_cast<uint8_t>(b)) * 1099511628211ull;
				wchar_t name[40];
				swprintf_s(name, L"\\%016llx.bin", static_cast<unsigned long long>(h));
				return folder_ + name;
			}
			bool Read(std::span<const std::byte> key, std::vector<std::byte>& data) {
				std::lock_guard<std::mutex> hold(lock_);
				std::ifstream in(PathOf(key), std::ios::binary);
				if (!in) return false;
				uint32_t keyLength = 0;
				in.read(reinterpret_cast<char*>(&keyLength), 4);
				if (!in || keyLength != key.size()) return false;
				std::vector<std::byte> stored(keyLength);
				in.read(reinterpret_cast<char*>(stored.data()), keyLength);
				if (!in || (keyLength && memcmp(stored.data(), key.data(), keyLength) != 0)) return false;
				in.seekg(0, std::ios::end);
				auto end = static_cast<size_t>(in.tellg());
				size_t size = end - 4 - keyLength;
				data.resize(size);
				in.seekg(4 + keyLength);
				in.read(reinterpret_cast<char*>(data.data()), size);
				return static_cast<bool>(in) || size == 0;
			}
			std::wstring folder_;
			std::mutex lock_;
		};

		class CartouchePlatform final : public Platform {
		public:
			explicit CartouchePlatform(std::wstring cacheFolder) {
				if (!cacheFolder.empty()) cache_ = std::make_unique<DiskCache>(std::move(cacheFolder));
			}
			CachingInterface* GetCachingInterface() override { return cache_.get(); }
			std::unique_ptr<WorkerTaskPool> CreateWorkerTaskPool() override {
				unsigned n = std::thread::hardware_concurrency();
				return std::make_unique<Pool>(n > 2 ? n - 1 : 2);
			}
			std::unique_ptr<DiskCache> cache_;
		};
	}

	dawn::platform::Platform* MakeDawnPlatform(const std::wstring& cacheFolder) { return new CartouchePlatform(cacheFolder); }

	std::string DawnCacheStats(dawn::platform::Platform* platform) {
		auto* p = static_cast<CartouchePlatform*>(platform);
		if (!p || !p->cache_) return "no cache";
		return "cache hits " + std::to_string(p->cache_->hits_.load()) + ", stored " + std::to_string(p->cache_->stores_.load());
	}
}
