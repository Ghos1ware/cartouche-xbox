#define WGPU_SHARED_LIBRARY
#include "GpuBridge.h"
#include <dawn/webgpu_cpp.h>
#include <dawn/native/DawnNative.h>
#include "DawnPlatform.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <cstring>
#include <string_view>
#include <vector>

using json = nlohmann::json;

namespace moonlight_xbox_dx {
	namespace {
		// ---------------------------------------------------------------- reading commands
		struct Reader {
			const uint8_t* p;
			const uint8_t* end;
			bool ok = true;
			bool has(size_t n) {
				if (static_cast<size_t>(end - p) < n) ok = false;
				return ok;
			}
			template <typename T> T get() {
				T v{};
				if (has(sizeof(T))) {
					memcpy(&v, p, sizeof(T));
					p += sizeof(T);
				}
				return v;
			}
			uint16_t u16() { return get<uint16_t>(); }
			uint32_t u32() { return get<uint32_t>(); }
			int32_t i32() { return get<int32_t>(); }
			float f32() { return get<float>(); }
			double f64() { return get<double>(); }
			std::pair<const uint8_t*, uint32_t> bytes() {
				uint32_t n = u32();
				if (!has(n)) return { nullptr, 0 };
				const uint8_t* at = p;
				p += n;
				return { at, n };
			}
			json js() {
				auto [at, n] = bytes();
				if (!at) return json::object();
				return json::parse(at, at + n, nullptr, false);
			}
		};

		wgpu::StringView sv(const std::string& s) { return wgpu::StringView{ s.data(), s.size() }; }
		std::string str(const json& j, const char* key, const char* fallback = "") {
			auto it = j.find(key);
			return it != j.end() && it->is_string() ? it->get<std::string>() : std::string(fallback);
		}
		template <typename T> T num(const json& j, const char* key, T fallback) {
			auto it = j.find(key);
			return it != j.end() && it->is_number() ? it->get<T>() : fallback;
		}
		bool flag(const json& j, const char* key, bool fallback = false) {
			auto it = j.find(key);
			return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
		}
		uint32_t idIn(const json& j, const char* key) {
			auto it = j.find(key);
			return it != j.end() && it->is_number() ? it->get<uint32_t>() : 0;
		}

		// ---------------------------------------------------------------- WebGPU's names to Dawn's enums
		struct FormatName {
			const char* name;
			wgpu::TextureFormat format;
		};
		const FormatName kFormats[] = {
			{ "r8unorm", wgpu::TextureFormat::R8Unorm }, { "r8snorm", wgpu::TextureFormat::R8Snorm }, { "r8uint", wgpu::TextureFormat::R8Uint }, { "r8sint", wgpu::TextureFormat::R8Sint },
			{ "r16uint", wgpu::TextureFormat::R16Uint }, { "r16sint", wgpu::TextureFormat::R16Sint }, { "r16float", wgpu::TextureFormat::R16Float }, { "r16unorm", wgpu::TextureFormat::R16Unorm }, { "r16snorm", wgpu::TextureFormat::R16Snorm },
			{ "rg8unorm", wgpu::TextureFormat::RG8Unorm }, { "rg8snorm", wgpu::TextureFormat::RG8Snorm }, { "rg8uint", wgpu::TextureFormat::RG8Uint }, { "rg8sint", wgpu::TextureFormat::RG8Sint },
			{ "r32uint", wgpu::TextureFormat::R32Uint }, { "r32sint", wgpu::TextureFormat::R32Sint }, { "r32float", wgpu::TextureFormat::R32Float },
			{ "rg16uint", wgpu::TextureFormat::RG16Uint }, { "rg16sint", wgpu::TextureFormat::RG16Sint }, { "rg16float", wgpu::TextureFormat::RG16Float }, { "rg16unorm", wgpu::TextureFormat::RG16Unorm }, { "rg16snorm", wgpu::TextureFormat::RG16Snorm },
			{ "rgba8unorm", wgpu::TextureFormat::RGBA8Unorm }, { "rgba8unorm-srgb", wgpu::TextureFormat::RGBA8UnormSrgb }, { "rgba8snorm", wgpu::TextureFormat::RGBA8Snorm }, { "rgba8uint", wgpu::TextureFormat::RGBA8Uint }, { "rgba8sint", wgpu::TextureFormat::RGBA8Sint },
			{ "bgra8unorm", wgpu::TextureFormat::BGRA8Unorm }, { "bgra8unorm-srgb", wgpu::TextureFormat::BGRA8UnormSrgb },
			{ "rgb9e5ufloat", wgpu::TextureFormat::RGB9E5Ufloat }, { "rgb10a2uint", wgpu::TextureFormat::RGB10A2Uint }, { "rgb10a2unorm", wgpu::TextureFormat::RGB10A2Unorm }, { "rg11b10ufloat", wgpu::TextureFormat::RG11B10Ufloat },
			{ "rg32uint", wgpu::TextureFormat::RG32Uint }, { "rg32sint", wgpu::TextureFormat::RG32Sint }, { "rg32float", wgpu::TextureFormat::RG32Float },
			{ "rgba16uint", wgpu::TextureFormat::RGBA16Uint }, { "rgba16sint", wgpu::TextureFormat::RGBA16Sint }, { "rgba16float", wgpu::TextureFormat::RGBA16Float }, { "rgba16unorm", wgpu::TextureFormat::RGBA16Unorm }, { "rgba16snorm", wgpu::TextureFormat::RGBA16Snorm },
			{ "rgba32uint", wgpu::TextureFormat::RGBA32Uint }, { "rgba32sint", wgpu::TextureFormat::RGBA32Sint }, { "rgba32float", wgpu::TextureFormat::RGBA32Float },
			{ "stencil8", wgpu::TextureFormat::Stencil8 }, { "depth16unorm", wgpu::TextureFormat::Depth16Unorm }, { "depth24plus", wgpu::TextureFormat::Depth24Plus }, { "depth24plus-stencil8", wgpu::TextureFormat::Depth24PlusStencil8 }, { "depth32float", wgpu::TextureFormat::Depth32Float }, { "depth32float-stencil8", wgpu::TextureFormat::Depth32FloatStencil8 },
			{ "bc1-rgba-unorm", wgpu::TextureFormat::BC1RGBAUnorm }, { "bc1-rgba-unorm-srgb", wgpu::TextureFormat::BC1RGBAUnormSrgb }, { "bc2-rgba-unorm", wgpu::TextureFormat::BC2RGBAUnorm }, { "bc2-rgba-unorm-srgb", wgpu::TextureFormat::BC2RGBAUnormSrgb },
			{ "bc3-rgba-unorm", wgpu::TextureFormat::BC3RGBAUnorm }, { "bc3-rgba-unorm-srgb", wgpu::TextureFormat::BC3RGBAUnormSrgb }, { "bc4-r-unorm", wgpu::TextureFormat::BC4RUnorm }, { "bc4-r-snorm", wgpu::TextureFormat::BC4RSnorm },
			{ "bc5-rg-unorm", wgpu::TextureFormat::BC5RGUnorm }, { "bc5-rg-snorm", wgpu::TextureFormat::BC5RGSnorm }, { "bc6h-rgb-ufloat", wgpu::TextureFormat::BC6HRGBUfloat }, { "bc6h-rgb-float", wgpu::TextureFormat::BC6HRGBFloat },
			{ "bc7-rgba-unorm", wgpu::TextureFormat::BC7RGBAUnorm }, { "bc7-rgba-unorm-srgb", wgpu::TextureFormat::BC7RGBAUnormSrgb },
		};
		wgpu::TextureFormat formatOf(const std::string& name) {
			for (const auto& f : kFormats)
				if (name == f.name) return f.format;
			return wgpu::TextureFormat::Undefined;
		}

		template <typename E> struct Name {
			const char* name;
			E value;
		};
		template <typename E, size_t N> E pick(const std::string& name, const Name<E> (&table)[N], E fallback) {
			for (const auto& n : table)
				if (name == n.name) return n.value;
			return fallback;
		}
		const Name<wgpu::TextureDimension> kDimensions[] = { { "1d", wgpu::TextureDimension::e1D }, { "2d", wgpu::TextureDimension::e2D }, { "3d", wgpu::TextureDimension::e3D } };
		const Name<wgpu::TextureViewDimension> kViewDimensions[] = { { "1d", wgpu::TextureViewDimension::e1D }, { "2d", wgpu::TextureViewDimension::e2D }, { "2d-array", wgpu::TextureViewDimension::e2DArray }, { "cube", wgpu::TextureViewDimension::Cube }, { "cube-array", wgpu::TextureViewDimension::CubeArray }, { "3d", wgpu::TextureViewDimension::e3D } };
		const Name<wgpu::TextureAspect> kAspects[] = { { "all", wgpu::TextureAspect::All }, { "stencil-only", wgpu::TextureAspect::StencilOnly }, { "depth-only", wgpu::TextureAspect::DepthOnly } };
		const Name<wgpu::AddressMode> kAddress[] = { { "clamp-to-edge", wgpu::AddressMode::ClampToEdge }, { "repeat", wgpu::AddressMode::Repeat }, { "mirror-repeat", wgpu::AddressMode::MirrorRepeat } };
		const Name<wgpu::FilterMode> kFilters[] = { { "nearest", wgpu::FilterMode::Nearest }, { "linear", wgpu::FilterMode::Linear } };
		const Name<wgpu::MipmapFilterMode> kMipFilters[] = { { "nearest", wgpu::MipmapFilterMode::Nearest }, { "linear", wgpu::MipmapFilterMode::Linear } };
		const Name<wgpu::CompareFunction> kCompare[] = { { "never", wgpu::CompareFunction::Never }, { "less", wgpu::CompareFunction::Less }, { "equal", wgpu::CompareFunction::Equal }, { "less-equal", wgpu::CompareFunction::LessEqual }, { "greater", wgpu::CompareFunction::Greater }, { "not-equal", wgpu::CompareFunction::NotEqual }, { "greater-equal", wgpu::CompareFunction::GreaterEqual }, { "always", wgpu::CompareFunction::Always } };
		const Name<wgpu::BufferBindingType> kBufferBindings[] = { { "uniform", wgpu::BufferBindingType::Uniform }, { "storage", wgpu::BufferBindingType::Storage }, { "read-only-storage", wgpu::BufferBindingType::ReadOnlyStorage } };
		const Name<wgpu::SamplerBindingType> kSamplerBindings[] = { { "filtering", wgpu::SamplerBindingType::Filtering }, { "non-filtering", wgpu::SamplerBindingType::NonFiltering }, { "comparison", wgpu::SamplerBindingType::Comparison } };
		const Name<wgpu::TextureSampleType> kSampleTypes[] = { { "float", wgpu::TextureSampleType::Float }, { "unfilterable-float", wgpu::TextureSampleType::UnfilterableFloat }, { "depth", wgpu::TextureSampleType::Depth }, { "sint", wgpu::TextureSampleType::Sint }, { "uint", wgpu::TextureSampleType::Uint } };
		const Name<wgpu::StorageTextureAccess> kStorageAccess[] = { { "write-only", wgpu::StorageTextureAccess::WriteOnly }, { "read-only", wgpu::StorageTextureAccess::ReadOnly }, { "read-write", wgpu::StorageTextureAccess::ReadWrite } };
		const Name<wgpu::VertexStepMode> kStepModes[] = { { "vertex", wgpu::VertexStepMode::Vertex }, { "instance", wgpu::VertexStepMode::Instance } };
		const Name<wgpu::PrimitiveTopology> kTopologies[] = { { "point-list", wgpu::PrimitiveTopology::PointList }, { "line-list", wgpu::PrimitiveTopology::LineList }, { "line-strip", wgpu::PrimitiveTopology::LineStrip }, { "triangle-list", wgpu::PrimitiveTopology::TriangleList }, { "triangle-strip", wgpu::PrimitiveTopology::TriangleStrip } };
		const Name<wgpu::IndexFormat> kIndexFormats[] = { { "uint16", wgpu::IndexFormat::Uint16 }, { "uint32", wgpu::IndexFormat::Uint32 } };
		const Name<wgpu::FrontFace> kFrontFaces[] = { { "ccw", wgpu::FrontFace::CCW }, { "cw", wgpu::FrontFace::CW } };
		const Name<wgpu::CullMode> kCullModes[] = { { "none", wgpu::CullMode::None }, { "front", wgpu::CullMode::Front }, { "back", wgpu::CullMode::Back } };
		const Name<wgpu::BlendOperation> kBlendOps[] = { { "add", wgpu::BlendOperation::Add }, { "subtract", wgpu::BlendOperation::Subtract }, { "reverse-subtract", wgpu::BlendOperation::ReverseSubtract }, { "min", wgpu::BlendOperation::Min }, { "max", wgpu::BlendOperation::Max } };
		const Name<wgpu::BlendFactor> kBlendFactors[] = {
			{ "zero", wgpu::BlendFactor::Zero }, { "one", wgpu::BlendFactor::One }, { "src", wgpu::BlendFactor::Src }, { "one-minus-src", wgpu::BlendFactor::OneMinusSrc }, { "src-alpha", wgpu::BlendFactor::SrcAlpha }, { "one-minus-src-alpha", wgpu::BlendFactor::OneMinusSrcAlpha },
			{ "dst", wgpu::BlendFactor::Dst }, { "one-minus-dst", wgpu::BlendFactor::OneMinusDst }, { "dst-alpha", wgpu::BlendFactor::DstAlpha }, { "one-minus-dst-alpha", wgpu::BlendFactor::OneMinusDstAlpha }, { "src-alpha-saturated", wgpu::BlendFactor::SrcAlphaSaturated },
			{ "constant", wgpu::BlendFactor::Constant }, { "one-minus-constant", wgpu::BlendFactor::OneMinusConstant }, { "src1", wgpu::BlendFactor::Src1 }, { "one-minus-src1", wgpu::BlendFactor::OneMinusSrc1 }, { "src1-alpha", wgpu::BlendFactor::Src1Alpha }, { "one-minus-src1-alpha", wgpu::BlendFactor::OneMinusSrc1Alpha },
		};
		const Name<wgpu::StencilOperation> kStencilOps[] = { { "keep", wgpu::StencilOperation::Keep }, { "zero", wgpu::StencilOperation::Zero }, { "replace", wgpu::StencilOperation::Replace }, { "invert", wgpu::StencilOperation::Invert }, { "increment-clamp", wgpu::StencilOperation::IncrementClamp }, { "decrement-clamp", wgpu::StencilOperation::DecrementClamp }, { "increment-wrap", wgpu::StencilOperation::IncrementWrap }, { "decrement-wrap", wgpu::StencilOperation::DecrementWrap } };
		const Name<wgpu::LoadOp> kLoadOps[] = { { "load", wgpu::LoadOp::Load }, { "clear", wgpu::LoadOp::Clear } };
		const Name<wgpu::StoreOp> kStoreOps[] = { { "store", wgpu::StoreOp::Store }, { "discard", wgpu::StoreOp::Discard } };
		const Name<wgpu::QueryType> kQueryTypes[] = { { "occlusion", wgpu::QueryType::Occlusion }, { "timestamp", wgpu::QueryType::Timestamp } };
		const Name<wgpu::VertexFormat> kVertexFormats[] = {
			{ "uint8", wgpu::VertexFormat::Uint8 }, { "uint8x2", wgpu::VertexFormat::Uint8x2 }, { "uint8x4", wgpu::VertexFormat::Uint8x4 }, { "sint8", wgpu::VertexFormat::Sint8 }, { "sint8x2", wgpu::VertexFormat::Sint8x2 }, { "sint8x4", wgpu::VertexFormat::Sint8x4 },
			{ "unorm8", wgpu::VertexFormat::Unorm8 }, { "unorm8x2", wgpu::VertexFormat::Unorm8x2 }, { "unorm8x4", wgpu::VertexFormat::Unorm8x4 }, { "snorm8", wgpu::VertexFormat::Snorm8 }, { "snorm8x2", wgpu::VertexFormat::Snorm8x2 }, { "snorm8x4", wgpu::VertexFormat::Snorm8x4 },
			{ "uint16", wgpu::VertexFormat::Uint16 }, { "uint16x2", wgpu::VertexFormat::Uint16x2 }, { "uint16x4", wgpu::VertexFormat::Uint16x4 }, { "sint16", wgpu::VertexFormat::Sint16 }, { "sint16x2", wgpu::VertexFormat::Sint16x2 }, { "sint16x4", wgpu::VertexFormat::Sint16x4 },
			{ "unorm16", wgpu::VertexFormat::Unorm16 }, { "unorm16x2", wgpu::VertexFormat::Unorm16x2 }, { "unorm16x4", wgpu::VertexFormat::Unorm16x4 }, { "snorm16", wgpu::VertexFormat::Snorm16 }, { "snorm16x2", wgpu::VertexFormat::Snorm16x2 }, { "snorm16x4", wgpu::VertexFormat::Snorm16x4 },
			{ "float16", wgpu::VertexFormat::Float16 }, { "float16x2", wgpu::VertexFormat::Float16x2 }, { "float16x4", wgpu::VertexFormat::Float16x4 },
			{ "float32", wgpu::VertexFormat::Float32 }, { "float32x2", wgpu::VertexFormat::Float32x2 }, { "float32x3", wgpu::VertexFormat::Float32x3 }, { "float32x4", wgpu::VertexFormat::Float32x4 },
			{ "uint32", wgpu::VertexFormat::Uint32 }, { "uint32x2", wgpu::VertexFormat::Uint32x2 }, { "uint32x3", wgpu::VertexFormat::Uint32x3 }, { "uint32x4", wgpu::VertexFormat::Uint32x4 },
			{ "sint32", wgpu::VertexFormat::Sint32 }, { "sint32x2", wgpu::VertexFormat::Sint32x2 }, { "sint32x3", wgpu::VertexFormat::Sint32x3 }, { "sint32x4", wgpu::VertexFormat::Sint32x4 },
			{ "unorm10-10-10-2", wgpu::VertexFormat::Unorm10_10_10_2 }, { "unorm8x4-bgra", wgpu::VertexFormat::Unorm8x4BGRA },
		};

		// The features three.js asks about, both ways.
		const Name<wgpu::FeatureName> kFeatures[] = {
			{ "depth-clip-control", wgpu::FeatureName::DepthClipControl }, { "depth32float-stencil8", wgpu::FeatureName::Depth32FloatStencil8 }, { "texture-compression-bc", wgpu::FeatureName::TextureCompressionBC },
			{ "texture-compression-bc-sliced-3d", wgpu::FeatureName::TextureCompressionBCSliced3D }, { "indirect-first-instance", wgpu::FeatureName::IndirectFirstInstance }, { "shader-f16", wgpu::FeatureName::ShaderF16 },
			{ "rg11b10ufloat-renderable", wgpu::FeatureName::RG11B10UfloatRenderable }, { "bgra8unorm-storage", wgpu::FeatureName::BGRA8UnormStorage }, { "float32-filterable", wgpu::FeatureName::Float32Filterable },
			{ "float32-blendable", wgpu::FeatureName::Float32Blendable }, { "clip-distances", wgpu::FeatureName::ClipDistances }, { "dual-source-blending", wgpu::FeatureName::DualSourceBlending },
			{ "subgroups", wgpu::FeatureName::Subgroups }, { "core-features-and-limits", wgpu::FeatureName::CoreFeaturesAndLimits },
		};
	}

	struct GpuBridge::Impl {
		struct Object;
		Impl(Send send, Log log) : send_(std::move(send)), log_(std::move(log)) {}
		Object& Get(uint32_t id);
		void Reply(uint32_t callback, bool ok, const std::string& dataJson = "null", const std::string& error = "");
		void ReportError(const char* kind, const std::string& message);
		bool Start(std::string& adapterJson, std::string& reason, const std::wstring& cacheFolder);
		dawn::platform::Platform* platform_ = nullptr;
		void SetPanel(void* swapChainPanel);
		void Execute(const uint8_t* data, size_t length);
		void Tick();
		void Reset();
		void Stop();
		bool Triangle(std::string& reason);
		void DrawTriangle();
		bool EnsureSurface();

		Send send_;
		Log log_;
		wgpu::Instance instance_;
		wgpu::Adapter adapter_;
		wgpu::Device device_;
		wgpu::Queue queue_;
		wgpu::Surface surface_;
		void* panel_ = nullptr;
		wgpu::TextureFormat surfaceFormat_ = wgpu::TextureFormat::BGRA8Unorm;
		uint32_t surfaceWidth_ = 0, surfaceHeight_ = 0;
		bool surfaceConfigured_ = false;
		std::unordered_map<uint32_t, std::unique_ptr<Object>> objects_;
		Stats stats_;
		// The step-2 triangle, drawn on every Tick while it is on.
		wgpu::RenderPipeline trianglePipeline_;
		uint64_t triangleFrames_ = 0;
		bool triangleEntered_ = false;
		std::chrono::steady_clock::time_point triangleSince_;
	};

	namespace {
		std::string text(wgpu::StringView v) {
			if (!v.data) return {};
			return std::string(v.data, v.length == WGPU_STRLEN ? strlen(v.data) : v.length);
		}
	}

	// ---------------------------------------------------------------- the page's objects
	struct GpuBridge::Impl::Object {
		enum class Kind { None, Buffer, Texture, View, Sampler, BindGroupLayout, PipelineLayout, BindGroup, ShaderModule, RenderPipeline, ComputePipeline, CommandEncoder, RenderPass, ComputePass, CommandBuffer, BundleEncoder, Bundle, QuerySet } kind = Kind::None;
		wgpu::Buffer buffer;
		wgpu::Texture texture;
		wgpu::TextureView view;
		wgpu::Sampler sampler;
		wgpu::BindGroupLayout bindGroupLayout;
		wgpu::PipelineLayout pipelineLayout;
		wgpu::BindGroup bindGroup;
		wgpu::ShaderModule shaderModule;
		wgpu::RenderPipeline renderPipeline;
		wgpu::ComputePipeline computePipeline;
		wgpu::CommandEncoder commandEncoder;
		wgpu::RenderPassEncoder renderPass;
		wgpu::ComputePassEncoder computePass;
		wgpu::CommandBuffer commandBuffer;
		wgpu::RenderBundleEncoder bundleEncoder;
		wgpu::RenderBundle bundle;
		wgpu::QuerySet querySet;
		uint64_t size = 0;
	};


	GpuBridge::Impl::Object& GpuBridge::Impl::Get(uint32_t id) {
		auto& slot = objects_[id];
		if (!slot) slot = std::make_unique<Object>();
		return *slot;
	}

	void GpuBridge::Impl::Reply(uint32_t callback, bool ok, const std::string& dataJson, const std::string& error) {
		if (!callback) return;
		json m = { { "gpuBridge", "reply" }, { "callback", callback }, { "ok", ok } };
		m["data"] = json::parse(dataJson, nullptr, false);
		if (!ok) m["error"] = error;
		send_(m.dump());
	}

	void GpuBridge::Impl::ReportError(const char* kind, const std::string& message) {
		stats_.errors++;
		send_(json{ { "gpuBridge", "error" }, { "kind", kind }, { "message", message } }.dump());
	}

	bool GpuBridge::Impl::Start(std::string& adapterJson, std::string& reason, const std::wstring& cacheFolder) {
		// Worker threads for the pipelines, and what they compile kept on disk.
		if (!platform_) platform_ = MakeDawnPlatform(cacheFolder);
		dawn::native::DawnInstanceDescriptor dawnDesc;
		dawnDesc.platform = platform_;
		wgpu::InstanceDescriptor instanceDesc = {};
		instanceDesc.nextInChain = &dawnDesc;
		static const wgpu::InstanceFeatureName kTimedWait = wgpu::InstanceFeatureName::TimedWaitAny;
		instanceDesc.requiredFeatureCount = 1;
		instanceDesc.requiredFeatures = &kTimedWait;
		instance_ = wgpu::CreateInstance(&instanceDesc);
		if (!instance_) {
			reason = "no instance";
			return false;
		}
		wgpu::RequestAdapterOptions options = {};
		options.backendType = wgpu::BackendType::D3D12;
		options.powerPreference = wgpu::PowerPreference::HighPerformance;
		auto adapterFuture = instance_.RequestAdapter(&options, wgpu::CallbackMode::WaitAnyOnly,
			[this, &reason](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
				if (status == wgpu::RequestAdapterStatus::Success) adapter_ = std::move(adapter);
				else reason = std::string(message.data ? message.data : "", message.length == WGPU_STRLEN ? (message.data ? strlen(message.data) : 0) : message.length);
			});
		instance_.WaitAny(adapterFuture, UINT64_MAX);
		if (!adapter_) {
			if (reason.empty()) reason = "no D3D12 adapter";
			return false;
		}

		wgpu::SupportedFeatures supported;
		adapter_.GetFeatures(&supported);
		std::vector<wgpu::FeatureName> wanted;
		json features = json::array();
		for (size_t i = 0; i < supported.featureCount; i++) {
			for (const auto& f : kFeatures)
				if (f.value == supported.features[i]) {
					wanted.push_back(f.value);
					features.push_back(f.name);
				}
		}
		wgpu::Limits limits = {};
		adapter_.GetLimits(&limits);

		wgpu::DeviceDescriptor deviceDesc = {};
		deviceDesc.requiredFeatureCount = wanted.size();
		deviceDesc.requiredFeatures = wanted.data();
		deviceDesc.requiredLimits = &limits;
		deviceDesc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message, Impl* self) {
			const char* kind = type == wgpu::ErrorType::OutOfMemory ? "out-of-memory" : type == wgpu::ErrorType::Internal ? "internal" : "validation";
			self->ReportError(kind, text(message));
		}, this);
		deviceDesc.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous, [this](const wgpu::Device&, wgpu::DeviceLostReason why, wgpu::StringView message) {
			if (why == wgpu::DeviceLostReason::Destroyed || why == wgpu::DeviceLostReason::CallbackCancelled) return;
			send_(json{ { "gpuBridge", "lost" }, { "reason", "unknown" }, { "message", std::string(message.data ? message.data : "", message.data ? (message.length == WGPU_STRLEN ? strlen(message.data) : message.length) : 0) } }.dump());
		});
		auto deviceFuture = adapter_.RequestDevice(&deviceDesc, wgpu::CallbackMode::WaitAnyOnly,
			[this, &reason](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message) {
				if (status == wgpu::RequestDeviceStatus::Success) device_ = std::move(device);
				else reason = std::string(message.data ? message.data : "", message.data ? (message.length == WGPU_STRLEN ? strlen(message.data) : message.length) : 0);
			});
		instance_.WaitAny(deviceFuture, UINT64_MAX);
		if (!device_) {
			if (reason.empty()) reason = "no device";
			return false;
		}
		queue_ = device_.GetQueue();

		json lim = {
			{ "maxTextureDimension1D", limits.maxTextureDimension1D }, { "maxTextureDimension2D", limits.maxTextureDimension2D }, { "maxTextureDimension3D", limits.maxTextureDimension3D },
			{ "maxTextureArrayLayers", limits.maxTextureArrayLayers }, { "maxBindGroups", limits.maxBindGroups }, { "maxBindGroupsPlusVertexBuffers", limits.maxBindGroupsPlusVertexBuffers },
			{ "maxBindingsPerBindGroup", limits.maxBindingsPerBindGroup }, { "maxDynamicUniformBuffersPerPipelineLayout", limits.maxDynamicUniformBuffersPerPipelineLayout },
			{ "maxDynamicStorageBuffersPerPipelineLayout", limits.maxDynamicStorageBuffersPerPipelineLayout }, { "maxSampledTexturesPerShaderStage", limits.maxSampledTexturesPerShaderStage },
			{ "maxSamplersPerShaderStage", limits.maxSamplersPerShaderStage }, { "maxStorageBuffersPerShaderStage", limits.maxStorageBuffersPerShaderStage },
			{ "maxStorageTexturesPerShaderStage", limits.maxStorageTexturesPerShaderStage }, { "maxUniformBuffersPerShaderStage", limits.maxUniformBuffersPerShaderStage },
			{ "maxUniformBufferBindingSize", limits.maxUniformBufferBindingSize }, { "maxStorageBufferBindingSize", limits.maxStorageBufferBindingSize },
			{ "minUniformBufferOffsetAlignment", limits.minUniformBufferOffsetAlignment }, { "minStorageBufferOffsetAlignment", limits.minStorageBufferOffsetAlignment },
			{ "maxVertexBuffers", limits.maxVertexBuffers }, { "maxBufferSize", limits.maxBufferSize }, { "maxVertexAttributes", limits.maxVertexAttributes },
			{ "maxVertexBufferArrayStride", limits.maxVertexBufferArrayStride }, { "maxInterStageShaderVariables", limits.maxInterStageShaderVariables },
			{ "maxColorAttachments", limits.maxColorAttachments }, { "maxColorAttachmentBytesPerSample", limits.maxColorAttachmentBytesPerSample },
			{ "maxComputeWorkgroupStorageSize", limits.maxComputeWorkgroupStorageSize }, { "maxComputeInvocationsPerWorkgroup", limits.maxComputeInvocationsPerWorkgroup },
			{ "maxComputeWorkgroupSizeX", limits.maxComputeWorkgroupSizeX }, { "maxComputeWorkgroupSizeY", limits.maxComputeWorkgroupSizeY },
			{ "maxComputeWorkgroupSizeZ", limits.maxComputeWorkgroupSizeZ }, { "maxComputeWorkgroupsPerDimension", limits.maxComputeWorkgroupsPerDimension },
		};
		adapterJson = json{ { "features", features }, { "limits", lim }, { "info", { { "vendor", "microsoft" }, { "architecture", "d3d12" }, { "description", "Cartouche Dawn (DirectX 12)" } } } }.dump();
		return true;
	}

	void GpuBridge::Impl::SetPanel(void* swapChainPanel) {
		if (panel_ == swapChainPanel) return;
		panel_ = swapChainPanel;
		surface_ = nullptr;
		surfaceConfigured_ = false;
	}

	// DirectX 12 let go entirely: on the console a D3D12 device alive in the app keeps the stream's D3D11 from starting.
	void GpuBridge::Impl::Stop() {
		Reset();
		surface_ = nullptr;
		panel_ = nullptr;
		queue_ = nullptr;
		if (device_) device_.Destroy();
		device_ = nullptr;
		adapter_ = nullptr;
		if (instance_) instance_.ProcessEvents();
		instance_ = nullptr;
	}

	void GpuBridge::Impl::Tick() {
		if (instance_) instance_.ProcessEvents();
		if (trianglePipeline_) DrawTriangle();
	}

	bool GpuBridge::Impl::EnsureSurface() {
		if (surface_) return true;
		if (!panel_ || !instance_) return false;
		wgpu::SurfaceDescriptorFromWindowsUWPSwapChainPanel source = {};
		source.swapChainPanel = panel_;
		wgpu::SurfaceDescriptor sd = {};
		sd.nextInChain = &source;
		surface_ = instance_.CreateSurface(&sd);
		return static_cast<bool>(surface_);
	}

	bool GpuBridge::Impl::Triangle(std::string& reason) {
		if (!device_) {
			reason = "no device";
			return false;
		}
		if (!EnsureSurface()) {
			reason = "no surface (panel)";
			return false;
		}
		wgpu::SurfaceConfiguration config = {};
		config.device = device_;
		config.format = wgpu::TextureFormat::BGRA8Unorm;
		config.usage = wgpu::TextureUsage::RenderAttachment;
		config.width = 1280;
		config.height = 720;
		config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
		config.presentMode = wgpu::PresentMode::Fifo;
		surface_.Configure(&config);
		surfaceConfigured_ = true;
		surfaceFormat_ = config.format;
		surfaceWidth_ = config.width;
		surfaceHeight_ = config.height;

		static const char* kCode = R"(
struct Out { @builtin(position) pos : vec4f, @location(0) color : vec3f };
@vertex fn vs(@builtin(vertex_index) i : u32) -> Out {
  var p = array<vec2f, 3>(vec2f(0.0, 0.7), vec2f(-0.7, -0.6), vec2f(0.7, -0.6));
  var c = array<vec3f, 3>(vec3f(1.0, 0.35, 0.1), vec3f(0.2, 0.8, 1.0), vec3f(0.9, 0.2, 0.9));
  var o : Out;
  o.pos = vec4f(p[i], 0.0, 1.0);
  o.color = c[i];
  return o;
}
@fragment fn fs(@location(0) color : vec3f) -> @location(0) vec4f { return vec4f(color, 1.0); }
)";
		auto started = std::chrono::steady_clock::now();
		wgpu::ShaderSourceWGSL wgsl = {};
		wgsl.code = wgpu::StringView{ kCode, strlen(kCode) };
		wgpu::ShaderModuleDescriptor md = {};
		md.nextInChain = &wgsl;
		wgpu::ShaderModule module = device_.CreateShaderModule(&md);
		wgpu::ColorTargetState target = {};
		target.format = config.format;
		wgpu::FragmentState fragment = {};
		fragment.module = module;
		fragment.targetCount = 1;
		fragment.targets = &target;
		wgpu::RenderPipelineDescriptor pd = {};
		pd.vertex.module = module;
		pd.fragment = &fragment;
		trianglePipeline_ = device_.CreateRenderPipeline(&pd);
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		if (log_) log_("bridge: triangle pipeline in " + std::to_string(static_cast<int>(ms)) + " ms");
		triangleFrames_ = 0;
		triangleSince_ = std::chrono::steady_clock::now();
		return static_cast<bool>(trianglePipeline_);
	}

	void GpuBridge::Impl::DrawTriangle() {
		if (log_ && triangleFrames_ == 0 && !triangleEntered_) {
			triangleEntered_ = true;
			log_(std::string("bridge: triangle tick, surface ") + (surfaceConfigured_ ? "configured" : "not configured"));
		}
		if (!surfaceConfigured_) return;
		wgpu::SurfaceTexture st = {};
		surface_.GetCurrentTexture(&st);
		if (!st.texture) {
			if (log_ && triangleFrames_ < 3) log_("bridge: no surface texture, status " + std::to_string(static_cast<int>(st.status)));
			triangleFrames_++;
			return;
		}
		if (log_ && triangleFrames_ == 0) log_("bridge: first triangle frame");
		wgpu::TextureView view = st.texture.CreateView();
		double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - triangleSince_).count();
		wgpu::RenderPassColorAttachment color = {};
		color.view = view;
		color.loadOp = wgpu::LoadOp::Clear;
		color.storeOp = wgpu::StoreOp::Store;
		color.clearValue = { 0.03, 0.02 + 0.02 * (1 + std::sin(t * 2.0)), 0.06, 1.0 };
		wgpu::RenderPassDescriptor rp = {};
		rp.colorAttachmentCount = 1;
		rp.colorAttachments = &color;
		wgpu::CommandEncoder encoder = device_.CreateCommandEncoder();
		wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&rp);
		pass.SetPipeline(trianglePipeline_);
		pass.Draw(3);
		pass.End();
		wgpu::CommandBuffer commands = encoder.Finish();
		queue_.Submit(1, &commands);
		surface_.Present();
		triangleFrames_++;
		if (triangleFrames_ % 300 == 0 && log_) {
			double fps = triangleFrames_ / std::max(0.001, t);
			log_("bridge: triangle " + std::to_string(triangleFrames_) + " frames, " + std::to_string(static_cast<int>(fps)) + " fps");
		}
	}

	// ---------------------------------------------------------------- the public face
	GpuBridge::GpuBridge(Send send, Log log) : impl_(std::make_unique<Impl>(std::move(send), std::move(log))) {}
	GpuBridge::~GpuBridge() { impl_->Reset(); }
	bool GpuBridge::Start(std::string& adapterJson, std::string& reason, const std::wstring& cacheFolder) { return impl_->Start(adapterJson, reason, cacheFolder); }
	std::string GpuBridge::CacheStats() const { return DawnCacheStats(impl_->platform_); }
	void GpuBridge::SetPanel(void* swapChainPanel) { impl_->SetPanel(swapChainPanel); }
	void GpuBridge::Execute(const uint8_t* data, size_t length) { impl_->Execute(data, length); }
	void GpuBridge::Tick() { impl_->Tick(); }
	void GpuBridge::Reset() { impl_->Reset(); }
	void GpuBridge::Stop() { impl_->Stop(); }
	bool GpuBridge::Triangle(std::string& reason) { return impl_->Triangle(reason); }
	GpuBridge::Stats GpuBridge::stats() const {
		Stats s = impl_->stats_;
		s.objects = impl_->objects_.size();
		s.surfaceWidth = impl_->surfaceConfigured_ ? impl_->surfaceWidth_ : 0;
		s.surfaceHeight = impl_->surfaceConfigured_ ? impl_->surfaceHeight_ : 0;
		return s;
	}

	void GpuBridge::Impl::Reset() {
		objects_.clear();
		trianglePipeline_ = nullptr;
		if (surface_ && surfaceConfigured_) surface_.Unconfigure();
		surfaceConfigured_ = false;
	}

	void GpuBridge::Impl::Execute(const uint8_t* data, size_t length) {
		if (!device_) return;
		stats_.flushes++;
		Reader top{ data, data + length };
		while (top.ok && top.p < top.end) {
			uint16_t op = top.u16();
			top.u16();
			uint32_t size = top.u32();
			if (!top.has(size)) break;
			Reader r{ top.p, top.p + size };
			top.p += size;
			stats_.commands++;
			try {
				switch (op) {
				case 1: { // createBuffer
					uint32_t id = r.u32();
					json d = r.js();
					auto& o = Get(id);
					o.kind = Object::Kind::Buffer;
					wgpu::BufferDescriptor desc = {};
					uint32_t usage = num<uint32_t>(d, "usage", 0);
					// A buffer written by mapping is written here by the queue: copy destination, never map-write.
					if (usage & static_cast<uint32_t>(wgpu::BufferUsage::MapWrite)) usage = (usage & ~static_cast<uint32_t>(wgpu::BufferUsage::MapWrite)) | static_cast<uint32_t>(wgpu::BufferUsage::CopyDst);
					if (!(usage & static_cast<uint32_t>(wgpu::BufferUsage::MapRead))) usage |= static_cast<uint32_t>(wgpu::BufferUsage::CopyDst);
					desc.usage = static_cast<wgpu::BufferUsage>(usage);
					desc.size = (num<uint64_t>(d, "size", 0) + 3) & ~uint64_t(3);
					std::string label = str(d, "label");
					desc.label = sv(label);
					o.size = desc.size;
					o.buffer = device_.CreateBuffer(&desc);
					break;
				}
				case 2: { // createTexture
					uint32_t id = r.u32();
					json d = r.js();
					auto& o = Get(id);
					o.kind = Object::Kind::Texture;
					wgpu::TextureDescriptor desc = {};
					desc.usage = static_cast<wgpu::TextureUsage>(num<uint32_t>(d, "usage", 0));
					desc.dimension = pick(str(d, "dimension", "2d"), kDimensions, wgpu::TextureDimension::e2D);
					const json& s = d["size"];
					desc.size = { num<uint32_t>(s, "width", 1), num<uint32_t>(s, "height", 1), num<uint32_t>(s, "depthOrArrayLayers", 1) };
					desc.format = formatOf(str(d, "format"));
					desc.mipLevelCount = num<uint32_t>(d, "mipLevelCount", 1);
					desc.sampleCount = num<uint32_t>(d, "sampleCount", 1);
					std::vector<wgpu::TextureFormat> viewFormats;
					if (d.contains("viewFormats"))
						for (const auto& f : d["viewFormats"]) viewFormats.push_back(formatOf(f.get<std::string>()));
					desc.viewFormatCount = viewFormats.size();
					desc.viewFormats = viewFormats.data();
					std::string label = str(d, "label");
					desc.label = sv(label);
					o.texture = device_.CreateTexture(&desc);
					break;
				}
				case 3: { // createView
					uint32_t id = r.u32();
					uint32_t textureId = r.u32();
					json d = r.js();
					auto& o = Get(id);
					o.kind = Object::Kind::View;
					wgpu::TextureViewDescriptor desc = {};
					if (d.contains("format")) desc.format = formatOf(str(d, "format"));
					if (d.contains("dimension")) desc.dimension = pick(str(d, "dimension"), kViewDimensions, wgpu::TextureViewDimension::Undefined);
					desc.baseMipLevel = num<uint32_t>(d, "baseMipLevel", 0);
					if (d.contains("mipLevelCount")) desc.mipLevelCount = num<uint32_t>(d, "mipLevelCount", WGPU_MIP_LEVEL_COUNT_UNDEFINED);
					desc.baseArrayLayer = num<uint32_t>(d, "baseArrayLayer", 0);
					if (d.contains("arrayLayerCount")) desc.arrayLayerCount = num<uint32_t>(d, "arrayLayerCount", WGPU_ARRAY_LAYER_COUNT_UNDEFINED);
					desc.aspect = pick(str(d, "aspect", "all"), kAspects, wgpu::TextureAspect::All);
					auto& t = Get(textureId);
					if (t.texture) o.view = t.texture.CreateView(&desc);
					break;
				}
				case 4: { // createSampler
					uint32_t id = r.u32();
					json d = r.js();
					auto& o = Get(id);
					o.kind = Object::Kind::Sampler;
					wgpu::SamplerDescriptor desc = {};
					desc.addressModeU = pick(str(d, "addressModeU", "clamp-to-edge"), kAddress, wgpu::AddressMode::ClampToEdge);
					desc.addressModeV = pick(str(d, "addressModeV", "clamp-to-edge"), kAddress, wgpu::AddressMode::ClampToEdge);
					desc.addressModeW = pick(str(d, "addressModeW", "clamp-to-edge"), kAddress, wgpu::AddressMode::ClampToEdge);
					desc.magFilter = pick(str(d, "magFilter", "nearest"), kFilters, wgpu::FilterMode::Nearest);
					desc.minFilter = pick(str(d, "minFilter", "nearest"), kFilters, wgpu::FilterMode::Nearest);
					desc.mipmapFilter = pick(str(d, "mipmapFilter", "nearest"), kMipFilters, wgpu::MipmapFilterMode::Nearest);
					desc.lodMinClamp = num<float>(d, "lodMinClamp", 0.f);
					desc.lodMaxClamp = num<float>(d, "lodMaxClamp", 32.f);
					if (d.contains("compare")) desc.compare = pick(str(d, "compare"), kCompare, wgpu::CompareFunction::Undefined);
					desc.maxAnisotropy = num<uint16_t>(d, "maxAnisotropy", 1);
					o.sampler = device_.CreateSampler(&desc);
					break;
				}
				case 5: { // createBindGroupLayout
					uint32_t id = r.u32();
					json d = r.js();
					auto& o = Get(id);
					o.kind = Object::Kind::BindGroupLayout;
					std::vector<wgpu::BindGroupLayoutEntry> entries;
					for (const auto& e : d["entries"]) {
						wgpu::BindGroupLayoutEntry entry = {};
						entry.binding = num<uint32_t>(e, "binding", 0);
						entry.visibility = static_cast<wgpu::ShaderStage>(num<uint32_t>(e, "visibility", 0));
						if (e.contains("buffer")) {
							const auto& b = e["buffer"];
							entry.buffer.type = pick(str(b, "type", "uniform"), kBufferBindings, wgpu::BufferBindingType::Uniform);
							entry.buffer.hasDynamicOffset = flag(b, "hasDynamicOffset");
							entry.buffer.minBindingSize = num<uint64_t>(b, "minBindingSize", 0);
						}
						if (e.contains("sampler")) entry.sampler.type = pick(str(e["sampler"], "type", "filtering"), kSamplerBindings, wgpu::SamplerBindingType::Filtering);
						if (e.contains("texture")) {
							const auto& t = e["texture"];
							entry.texture.sampleType = pick(str(t, "sampleType", "float"), kSampleTypes, wgpu::TextureSampleType::Float);
							entry.texture.viewDimension = pick(str(t, "viewDimension", "2d"), kViewDimensions, wgpu::TextureViewDimension::e2D);
							entry.texture.multisampled = flag(t, "multisampled");
						}
						if (e.contains("storageTexture")) {
							const auto& t = e["storageTexture"];
							entry.storageTexture.access = pick(str(t, "access", "write-only"), kStorageAccess, wgpu::StorageTextureAccess::WriteOnly);
							entry.storageTexture.format = formatOf(str(t, "format"));
							entry.storageTexture.viewDimension = pick(str(t, "viewDimension", "2d"), kViewDimensions, wgpu::TextureViewDimension::e2D);
						}
						entries.push_back(entry);
					}
					wgpu::BindGroupLayoutDescriptor desc = {};
					desc.entryCount = entries.size();
					desc.entries = entries.data();
					o.bindGroupLayout = device_.CreateBindGroupLayout(&desc);
					break;
				}
				case 6: { // createPipelineLayout
					uint32_t id = r.u32();
					json d = r.js();
					auto& o = Get(id);
					o.kind = Object::Kind::PipelineLayout;
					std::vector<wgpu::BindGroupLayout> layouts;
					for (const auto& l : d["bindGroupLayouts"]) layouts.push_back(l.is_number() ? Get(l.get<uint32_t>()).bindGroupLayout : nullptr);
					wgpu::PipelineLayoutDescriptor desc = {};
					desc.bindGroupLayoutCount = layouts.size();
					desc.bindGroupLayouts = layouts.data();
					o.pipelineLayout = device_.CreatePipelineLayout(&desc);
					break;
				}
				case 7: { // createBindGroup
					uint32_t id = r.u32();
					json d = r.js();
					auto& o = Get(id);
					o.kind = Object::Kind::BindGroup;
					std::vector<wgpu::BindGroupEntry> entries;
					for (const auto& e : d["entries"]) {
						wgpu::BindGroupEntry entry = {};
						entry.binding = num<uint32_t>(e, "binding", 0);
						const auto& res = e["resource"];
						if (res.is_number()) {
							auto& target = Get(res.get<uint32_t>());
							if (target.kind == Object::Kind::Sampler) entry.sampler = target.sampler;
							else if (target.kind == Object::Kind::View) entry.textureView = target.view;
							else if (target.kind == Object::Kind::Buffer) entry.buffer = target.buffer;
							else if (target.kind == Object::Kind::Texture) entry.textureView = target.texture.CreateView();
						}
						else if (res.is_object()) {
							entry.buffer = Get(idIn(res, "buffer")).buffer;
							entry.offset = num<uint64_t>(res, "offset", 0);
							if (res.contains("size")) entry.size = num<uint64_t>(res, "size", WGPU_WHOLE_SIZE);
						}
						entries.push_back(entry);
					}
					wgpu::BindGroupDescriptor desc = {};
					desc.layout = Get(idIn(d, "layout")).bindGroupLayout;
					desc.entryCount = entries.size();
					desc.entries = entries.data();
					o.bindGroup = device_.CreateBindGroup(&desc);
					break;
				}
				case 8: { // createShaderModule
					uint32_t id = r.u32();
					json d = r.js();
					auto& o = Get(id);
					o.kind = Object::Kind::ShaderModule;
					std::string code = str(d, "code");
					wgpu::ShaderSourceWGSL wgsl = {};
					wgsl.code = sv(code);
					wgpu::ShaderModuleDescriptor desc = {};
					desc.nextInChain = &wgsl;
					o.shaderModule = device_.CreateShaderModule(&desc);
					break;
				}
				case 9:
				case 10: { // createRenderPipeline / createComputePipeline (async when a callback is given)
					uint32_t id = r.u32();
					uint32_t callback = r.u32();
					json d = r.js();
					auto& o = Get(id);
					auto stage = [&](const json& s, std::string& entry, std::vector<std::string>& keys, std::vector<wgpu::ConstantEntry>& constants) {
						entry = str(s, "entryPoint");
						if (s.contains("constants"))
							for (auto it = s["constants"].begin(); it != s["constants"].end(); ++it) keys.push_back(it.key());
						constants.resize(keys.size());
						size_t i = 0;
						if (s.contains("constants"))
							for (auto it = s["constants"].begin(); it != s["constants"].end(); ++it, ++i) {
								constants[i].key = sv(keys[i]);
								constants[i].value = it.value().get<double>();
							}
					};
					wgpu::PipelineLayout layout = nullptr;
					if (d.contains("layout") && d["layout"].is_number()) layout = Get(d["layout"].get<uint32_t>()).pipelineLayout;
					auto started = std::chrono::steady_clock::now();
					auto timeIt = [this, started]() {
						stats_.pipelines++;
						stats_.pipelineMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
					};
					if (op == 10) {
						o.kind = Object::Kind::ComputePipeline;
						std::string entry;
						std::vector<std::string> keys;
						std::vector<wgpu::ConstantEntry> constants;
						const json& c = d["compute"];
						stage(c, entry, keys, constants);
						wgpu::ComputePipelineDescriptor desc = {};
						desc.layout = layout;
						desc.compute.module = Get(idIn(c, "module")).shaderModule;
						if (!entry.empty()) desc.compute.entryPoint = sv(entry);
						desc.compute.constantCount = constants.size();
						desc.compute.constants = constants.data();
						if (!callback) {
							o.computePipeline = device_.CreateComputePipeline(&desc);
							timeIt();
						}
						else
							device_.CreateComputePipelineAsync(&desc, wgpu::CallbackMode::AllowProcessEvents, [this, id, callback, timeIt](wgpu::CreatePipelineAsyncStatus status, wgpu::ComputePipeline pipeline, wgpu::StringView message) {
								timeIt();
								if (status == wgpu::CreatePipelineAsyncStatus::Success) {
									Get(id).computePipeline = std::move(pipeline);
									Reply(callback, true);
								}
								else Reply(callback, false, "null", std::string(message.data ? message.data : "", message.data ? (message.length == WGPU_STRLEN ? strlen(message.data) : message.length) : 0));
							});
						break;
					}
					o.kind = Object::Kind::RenderPipeline;
					wgpu::RenderPipelineDescriptor desc = {};
					desc.layout = layout;
					// Vertex.
					const json& v = d["vertex"];
					std::string vEntry;
					std::vector<std::string> vKeys;
					std::vector<wgpu::ConstantEntry> vConstants;
					stage(v, vEntry, vKeys, vConstants);
					desc.vertex.module = Get(idIn(v, "module")).shaderModule;
					if (!vEntry.empty()) desc.vertex.entryPoint = sv(vEntry);
					desc.vertex.constantCount = vConstants.size();
					desc.vertex.constants = vConstants.data();
					std::vector<wgpu::VertexBufferLayout> buffers;
					std::vector<std::vector<wgpu::VertexAttribute>> attributes;
					if (v.contains("buffers"))
						for (const auto& b : v["buffers"]) {
							wgpu::VertexBufferLayout layoutB = {};
							attributes.emplace_back();
							if (b.is_null()) {
								buffers.push_back(layoutB);
								continue;
							}
							layoutB.arrayStride = num<uint64_t>(b, "arrayStride", 0);
							layoutB.stepMode = pick(str(b, "stepMode", "vertex"), kStepModes, wgpu::VertexStepMode::Vertex);
							for (const auto& a : b["attributes"]) {
								wgpu::VertexAttribute attr = {};
								attr.format = pick(str(a, "format"), kVertexFormats, wgpu::VertexFormat::Float32);
								attr.offset = num<uint64_t>(a, "offset", 0);
								attr.shaderLocation = num<uint32_t>(a, "shaderLocation", 0);
								attributes.back().push_back(attr);
							}
							buffers.push_back(layoutB);
						}
					for (size_t i = 0; i < buffers.size(); i++) {
						buffers[i].attributeCount = attributes[i].size();
						buffers[i].attributes = attributes[i].data();
					}
					desc.vertex.bufferCount = buffers.size();
					desc.vertex.buffers = buffers.data();
					// Primitive.
					if (d.contains("primitive")) {
						const json& p = d["primitive"];
						desc.primitive.topology = pick(str(p, "topology", "triangle-list"), kTopologies, wgpu::PrimitiveTopology::TriangleList);
						if (p.contains("stripIndexFormat")) desc.primitive.stripIndexFormat = pick(str(p, "stripIndexFormat"), kIndexFormats, wgpu::IndexFormat::Undefined);
						desc.primitive.frontFace = pick(str(p, "frontFace", "ccw"), kFrontFaces, wgpu::FrontFace::CCW);
						desc.primitive.cullMode = pick(str(p, "cullMode", "none"), kCullModes, wgpu::CullMode::None);
						desc.primitive.unclippedDepth = flag(p, "unclippedDepth");
					}
					// Depth and stencil.
					wgpu::DepthStencilState depth = {};
					if (d.contains("depthStencil") && d["depthStencil"].is_object()) {
						const json& ds = d["depthStencil"];
						depth.format = formatOf(str(ds, "format"));
						if (ds.contains("depthWriteEnabled")) depth.depthWriteEnabled = flag(ds, "depthWriteEnabled") ? wgpu::OptionalBool::True : wgpu::OptionalBool::False;
						if (ds.contains("depthCompare")) depth.depthCompare = pick(str(ds, "depthCompare"), kCompare, wgpu::CompareFunction::Undefined);
						auto face = [](const json& f, wgpu::StencilFaceState& s) {
							s.compare = pick(str(f, "compare", "always"), kCompare, wgpu::CompareFunction::Always);
							s.failOp = pick(str(f, "failOp", "keep"), kStencilOps, wgpu::StencilOperation::Keep);
							s.depthFailOp = pick(str(f, "depthFailOp", "keep"), kStencilOps, wgpu::StencilOperation::Keep);
							s.passOp = pick(str(f, "passOp", "keep"), kStencilOps, wgpu::StencilOperation::Keep);
						};
						if (ds.contains("stencilFront")) face(ds["stencilFront"], depth.stencilFront);
						if (ds.contains("stencilBack")) face(ds["stencilBack"], depth.stencilBack);
						depth.stencilReadMask = num<uint32_t>(ds, "stencilReadMask", 0xFFFFFFFF);
						depth.stencilWriteMask = num<uint32_t>(ds, "stencilWriteMask", 0xFFFFFFFF);
						depth.depthBias = num<int32_t>(ds, "depthBias", 0);
						depth.depthBiasSlopeScale = num<float>(ds, "depthBiasSlopeScale", 0.f);
						depth.depthBiasClamp = num<float>(ds, "depthBiasClamp", 0.f);
						desc.depthStencil = &depth;
					}
					// Multisample.
					if (d.contains("multisample")) {
						const json& m = d["multisample"];
						desc.multisample.count = num<uint32_t>(m, "count", 1);
						desc.multisample.mask = num<uint32_t>(m, "mask", 0xFFFFFFFF);
						desc.multisample.alphaToCoverageEnabled = flag(m, "alphaToCoverageEnabled");
					}
					// Fragment.
					wgpu::FragmentState fragment = {};
					std::string fEntry;
					std::vector<std::string> fKeys;
					std::vector<wgpu::ConstantEntry> fConstants;
					std::vector<wgpu::ColorTargetState> targets;
					std::vector<wgpu::BlendState> blends;
					if (d.contains("fragment") && d["fragment"].is_object()) {
						const json& f = d["fragment"];
						stage(f, fEntry, fKeys, fConstants);
						fragment.module = Get(idIn(f, "module")).shaderModule;
						if (!fEntry.empty()) fragment.entryPoint = sv(fEntry);
						fragment.constantCount = fConstants.size();
						fragment.constants = fConstants.data();
						blends.reserve(f["targets"].size());
						for (const auto& t : f["targets"]) {
							wgpu::ColorTargetState target = {};
							if (t.is_null()) {
								targets.push_back(target);
								continue;
							}
							target.format = formatOf(str(t, "format"));
							target.writeMask = static_cast<wgpu::ColorWriteMask>(num<uint32_t>(t, "writeMask", 0xF));
							if (t.contains("blend") && t["blend"].is_object()) {
								wgpu::BlendState blend = {};
								auto comp = [](const json& c, wgpu::BlendComponent& b) {
									b.operation = pick(str(c, "operation", "add"), kBlendOps, wgpu::BlendOperation::Add);
									b.srcFactor = pick(str(c, "srcFactor", "one"), kBlendFactors, wgpu::BlendFactor::One);
									b.dstFactor = pick(str(c, "dstFactor", "zero"), kBlendFactors, wgpu::BlendFactor::Zero);
								};
								comp(t["blend"]["color"], blend.color);
								comp(t["blend"]["alpha"], blend.alpha);
								blends.push_back(blend);
								target.blend = &blends.back();
							}
							targets.push_back(target);
						}
						fragment.targetCount = targets.size();
						fragment.targets = targets.data();
						desc.fragment = &fragment;
					}
					if (!callback) {
						o.renderPipeline = device_.CreateRenderPipeline(&desc);
						timeIt();
					}
					else
						device_.CreateRenderPipelineAsync(&desc, wgpu::CallbackMode::AllowProcessEvents, [this, id, callback, timeIt](wgpu::CreatePipelineAsyncStatus status, wgpu::RenderPipeline pipeline, wgpu::StringView message) {
							timeIt();
							if (status == wgpu::CreatePipelineAsyncStatus::Success) {
								Get(id).renderPipeline = std::move(pipeline);
								Reply(callback, true);
							}
							else Reply(callback, false, "null", std::string(message.data ? message.data : "", message.data ? (message.length == WGPU_STRLEN ? strlen(message.data) : message.length) : 0));
						});
					break;
				}
				case 11: { // getBindGroupLayout
					uint32_t id = r.u32();
					uint32_t pipelineId = r.u32();
					uint32_t index = r.u32();
					auto& o = Get(id);
					o.kind = Object::Kind::BindGroupLayout;
					auto& p = Get(pipelineId);
					if (p.renderPipeline) o.bindGroupLayout = p.renderPipeline.GetBindGroupLayout(index);
					else if (p.computePipeline) o.bindGroupLayout = p.computePipeline.GetBindGroupLayout(index);
					break;
				}
				case 12: { // createCommandEncoder
					auto& o = Get(r.u32());
					o.kind = Object::Kind::CommandEncoder;
					o.commandEncoder = device_.CreateCommandEncoder();
					break;
				}
				case 13: { // beginRenderPass
					uint32_t id = r.u32();
					uint32_t encoderId = r.u32();
					json d = r.js();
					auto& o = Get(id);
					o.kind = Object::Kind::RenderPass;
					std::vector<wgpu::RenderPassColorAttachment> colors;
					for (const auto& c : d["colorAttachments"]) {
						wgpu::RenderPassColorAttachment a = {};
						if (c.is_null()) {
							colors.push_back(a);
							continue;
						}
						a.view = Get(idIn(c, "view")).view;
						if (c.contains("resolveTarget") && c["resolveTarget"].is_number()) a.resolveTarget = Get(c["resolveTarget"].get<uint32_t>()).view;
						if (c.contains("depthSlice")) a.depthSlice = num<uint32_t>(c, "depthSlice", WGPU_DEPTH_SLICE_UNDEFINED);
						a.loadOp = pick(str(c, "loadOp", "load"), kLoadOps, wgpu::LoadOp::Load);
						a.storeOp = pick(str(c, "storeOp", "store"), kStoreOps, wgpu::StoreOp::Store);
						if (c.contains("clearValue")) {
							const auto& v = c["clearValue"];
							if (v.is_array()) a.clearValue = { v[0].get<double>(), v[1].get<double>(), v[2].get<double>(), v[3].get<double>() };
							else a.clearValue = { num<double>(v, "r", 0), num<double>(v, "g", 0), num<double>(v, "b", 0), num<double>(v, "a", 0) };
						}
						colors.push_back(a);
					}
					wgpu::RenderPassDepthStencilAttachment depth = {};
					wgpu::RenderPassDescriptor desc = {};
					desc.colorAttachmentCount = colors.size();
					desc.colorAttachments = colors.data();
					if (d.contains("depthStencilAttachment") && d["depthStencilAttachment"].is_object()) {
						const json& ds = d["depthStencilAttachment"];
						depth.view = Get(idIn(ds, "view")).view;
						if (ds.contains("depthLoadOp")) depth.depthLoadOp = pick(str(ds, "depthLoadOp"), kLoadOps, wgpu::LoadOp::Load);
						if (ds.contains("depthStoreOp")) depth.depthStoreOp = pick(str(ds, "depthStoreOp"), kStoreOps, wgpu::StoreOp::Store);
						if (ds.contains("depthClearValue")) depth.depthClearValue = num<float>(ds, "depthClearValue", 1.f);
						depth.depthReadOnly = flag(ds, "depthReadOnly");
						if (ds.contains("stencilLoadOp")) depth.stencilLoadOp = pick(str(ds, "stencilLoadOp"), kLoadOps, wgpu::LoadOp::Load);
						if (ds.contains("stencilStoreOp")) depth.stencilStoreOp = pick(str(ds, "stencilStoreOp"), kStoreOps, wgpu::StoreOp::Store);
						depth.stencilClearValue = num<uint32_t>(ds, "stencilClearValue", 0);
						depth.stencilReadOnly = flag(ds, "stencilReadOnly");
						desc.depthStencilAttachment = &depth;
					}
					if (d.contains("occlusionQuerySet") && d["occlusionQuerySet"].is_number()) desc.occlusionQuerySet = Get(d["occlusionQuerySet"].get<uint32_t>()).querySet;
					auto& e = Get(encoderId);
					if (e.commandEncoder) o.renderPass = e.commandEncoder.BeginRenderPass(&desc);
					break;
				}
				case 14: { // beginComputePass
					uint32_t id = r.u32();
					uint32_t encoderId = r.u32();
					r.js();
					auto& o = Get(id);
					o.kind = Object::Kind::ComputePass;
					auto& e = Get(encoderId);
					if (e.commandEncoder) o.computePass = e.commandEncoder.BeginComputePass();
					break;
				}
				case 15: { // finish
					uint32_t id = r.u32();
					uint32_t encoderId = r.u32();
					auto& o = Get(id);
					o.kind = Object::Kind::CommandBuffer;
					auto& e = Get(encoderId);
					if (e.commandEncoder) o.commandBuffer = e.commandEncoder.Finish();
					objects_.erase(encoderId);
					break;
				}
				case 16: { // submit
					uint32_t count = r.u32();
					std::vector<wgpu::CommandBuffer> list;
					for (uint32_t i = 0; i < count; i++) {
						uint32_t id = r.u32();
						auto it = objects_.find(id);
						if (it != objects_.end() && it->second->commandBuffer) list.push_back(it->second->commandBuffer);
						objects_.erase(id);
					}
					queue_.Submit(list.size(), list.data());
					break;
				}
				case 17: // writeBuffer
				case 19: { // bufferData
					uint32_t id = r.u32();
					double offset = r.f64();
					auto [at, n] = r.bytes();
					auto& b = Get(id);
					if (b.buffer && at && n) {
						// The queue writes in steps of 4 bytes: the tail rounded up with zeros.
						if (n % 4 == 0) queue_.WriteBuffer(b.buffer, static_cast<uint64_t>(offset), at, n);
						else {
							std::vector<uint8_t> padded(at, at + n);
							padded.resize((n + 3) & ~3u, 0);
							uint64_t room = b.size - static_cast<uint64_t>(offset);
							queue_.WriteBuffer(b.buffer, static_cast<uint64_t>(offset), padded.data(), static_cast<size_t>(std::min<uint64_t>(padded.size(), room)));
						}
					}
					break;
				}
				case 18: { // writeTexture
					json d = r.js();
					auto [at, n] = r.bytes();
					const json& dst = d["destination"];
					wgpu::TexelCopyTextureInfo info = {};
					info.texture = Get(idIn(dst, "texture")).texture;
					info.mipLevel = num<uint32_t>(dst, "mipLevel", 0);
					info.origin = { num<uint32_t>(dst["origin"], "x", 0), num<uint32_t>(dst["origin"], "y", 0), num<uint32_t>(dst["origin"], "z", 0) };
					info.aspect = pick(str(dst, "aspect", "all"), kAspects, wgpu::TextureAspect::All);
					const json& l = d["layout"];
					wgpu::TexelCopyBufferLayout layout = {};
					layout.offset = num<uint64_t>(l, "offset", 0);
					if (l.contains("bytesPerRow")) layout.bytesPerRow = num<uint32_t>(l, "bytesPerRow", WGPU_COPY_STRIDE_UNDEFINED);
					if (l.contains("rowsPerImage")) layout.rowsPerImage = num<uint32_t>(l, "rowsPerImage", WGPU_COPY_STRIDE_UNDEFINED);
					const json& s = d["size"];
					wgpu::Extent3D size = { num<uint32_t>(s, "width", 1), num<uint32_t>(s, "height", 1), num<uint32_t>(s, "depthOrArrayLayers", 1) };
					if (info.texture && at) queue_.WriteTexture(&info, at, n, &layout, &size);
					break;
				}
				case 20: { // mapRead
					uint32_t id = r.u32();
					uint32_t callback = r.u32();
					double offset = r.f64();
					double size = r.f64();
					auto& b = Get(id);
					if (!b.buffer) {
						Reply(callback, false, "null", "no buffer");
						break;
					}
					wgpu::Buffer buffer = b.buffer;
					size_t at = static_cast<size_t>(offset), length = static_cast<size_t>(size);
					buffer.MapAsync(wgpu::MapMode::Read, at, length, wgpu::CallbackMode::AllowProcessEvents, [this, buffer, at, length, callback](wgpu::MapAsyncStatus status, wgpu::StringView message) {
						if (status != wgpu::MapAsyncStatus::Success) {
							Reply(callback, false, "null", std::string(message.data ? message.data : "map failed", message.data ? (message.length == WGPU_STRLEN ? strlen(message.data) : message.length) : 10));
							return;
						}
						const uint8_t* data = static_cast<const uint8_t*>(buffer.GetConstMappedRange(at, length));
						// Base64, for the page's JSON message.
						static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
						std::string out;
						out.reserve((length + 2) / 3 * 4 + 2);
						out.push_back('"');
						for (size_t i = 0; i < length; i += 3) {
							uint32_t n = data[i] << 16 | (i + 1 < length ? data[i + 1] << 8 : 0) | (i + 2 < length ? data[i + 2] : 0);
							out.push_back(k[(n >> 18) & 63]);
							out.push_back(k[(n >> 12) & 63]);
							out.push_back(i + 1 < length ? k[(n >> 6) & 63] : '=');
							out.push_back(i + 2 < length ? k[n & 63] : '=');
						}
						out.push_back('"');
						buffer.Unmap();
						Reply(callback, true, out);
					});
					break;
				}
				case 21: { // destroy
					auto& o = Get(r.u32());
					if (o.buffer) o.buffer.Destroy();
					if (o.texture) o.texture.Destroy();
					if (o.querySet) o.querySet.Destroy();
					break;
				}
				case 22: // release
					objects_.erase(r.u32());
					break;
				case 23: { // configureCanvas
					json d = r.js();
					if (!panel_) break;
					if (!surface_) {
						wgpu::SurfaceDescriptorFromWindowsUWPSwapChainPanel source = {};
						source.swapChainPanel = panel_;
						wgpu::SurfaceDescriptor sd = {};
						sd.nextInChain = &source;
						surface_ = instance_.CreateSurface(&sd);
					}
					surfaceWidth_ = num<uint32_t>(d, "width", 1);
					surfaceHeight_ = num<uint32_t>(d, "height", 1);
					surfaceFormat_ = formatOf(str(d, "format", "bgra8unorm"));
					wgpu::SurfaceConfiguration config = {};
					config.device = device_;
					config.format = surfaceFormat_;
					config.usage = static_cast<wgpu::TextureUsage>(num<uint32_t>(d, "usage", static_cast<uint32_t>(wgpu::TextureUsage::RenderAttachment))) | wgpu::TextureUsage::RenderAttachment;
					config.width = surfaceWidth_;
					config.height = surfaceHeight_;
					config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
					config.presentMode = wgpu::PresentMode::Fifo;
					surface_.Configure(&config);
					surfaceConfigured_ = true;
					break;
				}
				case 24: { // currentTexture
					uint32_t id = r.u32();
					auto& o = Get(id);
					o.kind = Object::Kind::Texture;
					if (surface_ && surfaceConfigured_) {
						wgpu::SurfaceTexture st = {};
						surface_.GetCurrentTexture(&st);
						o.texture = st.texture;
					}
					break;
				}
				case 25: // present
					if (surface_ && surfaceConfigured_) {
						surface_.Present();
						stats_.presents++;
					}
					break;
				case 26: { // workDone
					uint32_t callback = r.u32();
					queue_.OnSubmittedWorkDone(wgpu::CallbackMode::AllowProcessEvents, [this, callback](wgpu::QueueWorkDoneStatus, wgpu::StringView) { Reply(callback, true); });
					break;
				}
				case 27: { // pushErrorScope
					uint32_t filter = r.u32();
					device_.PushErrorScope(filter == 1 ? wgpu::ErrorFilter::OutOfMemory : filter == 2 ? wgpu::ErrorFilter::Internal : wgpu::ErrorFilter::Validation);
					break;
				}
				case 28: { // popErrorScope
					uint32_t callback = r.u32();
					device_.PopErrorScope(wgpu::CallbackMode::AllowProcessEvents, [this, callback](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView message) {
						if (type == wgpu::ErrorType::NoError) {
							Reply(callback, true, "null");
							return;
						}
						json e = { { "kind", type == wgpu::ErrorType::OutOfMemory ? "out-of-memory" : "validation" }, { "message", std::string(message.data ? message.data : "", message.data ? (message.length == WGPU_STRLEN ? strlen(message.data) : message.length) : 0) } };
						Reply(callback, true, e.dump());
					});
					break;
				}
				case 29: { // compilationInfo
					uint32_t id = r.u32();
					uint32_t callback = r.u32();
					auto& m = Get(id);
					if (!m.shaderModule) {
						Reply(callback, true, "{\"messages\":[]}");
						break;
					}
					m.shaderModule.GetCompilationInfo(wgpu::CallbackMode::AllowProcessEvents, [this, callback](wgpu::CompilationInfoRequestStatus, const wgpu::CompilationInfo* info) {
						json messages = json::array();
						if (info)
							for (size_t i = 0; i < info->messageCount; i++) {
								const auto& msg = info->messages[i];
								messages.push_back({ { "message", std::string(msg.message.data ? msg.message.data : "", msg.message.data ? (msg.message.length == WGPU_STRLEN ? strlen(msg.message.data) : msg.message.length) : 0) }, { "type", msg.type == wgpu::CompilationMessageType::Error ? "error" : msg.type == wgpu::CompilationMessageType::Warning ? "warning" : "info" }, { "lineNum", msg.lineNum }, { "linePos", msg.linePos } });
							}
						Reply(callback, true, json{ { "messages", messages } }.dump());
					});
					break;
				}
				case 30: { // createQuerySet
					uint32_t id = r.u32();
					json d = r.js();
					auto& o = Get(id);
					o.kind = Object::Kind::QuerySet;
					wgpu::QuerySetDescriptor desc = {};
					desc.type = pick(str(d, "type", "occlusion"), kQueryTypes, wgpu::QueryType::Occlusion);
					desc.count = num<uint32_t>(d, "count", 1);
					o.querySet = device_.CreateQuerySet(&desc);
					break;
				}
				case 31: { // copyBufferToBuffer
					auto& e = Get(r.u32());
					auto& src = Get(r.u32());
					double srcOffset = r.f64();
					auto& dst = Get(r.u32());
					double dstOffset = r.f64();
					double size = r.f64();
					if (e.commandEncoder && src.buffer && dst.buffer) e.commandEncoder.CopyBufferToBuffer(src.buffer, static_cast<uint64_t>(srcOffset), dst.buffer, static_cast<uint64_t>(dstOffset), static_cast<uint64_t>(size));
					break;
				}
				case 32:
				case 33:
				case 34: { // copyBufferToTexture / copyTextureToBuffer / copyTextureToTexture
					auto& e = Get(r.u32());
					json d = r.js();
					auto textureInfo = [&](const json& t) {
						wgpu::TexelCopyTextureInfo info = {};
						info.texture = Get(idIn(t, "texture")).texture;
						info.mipLevel = num<uint32_t>(t, "mipLevel", 0);
						if (t.contains("origin")) info.origin = { num<uint32_t>(t["origin"], "x", 0), num<uint32_t>(t["origin"], "y", 0), num<uint32_t>(t["origin"], "z", 0) };
						info.aspect = pick(str(t, "aspect", "all"), kAspects, wgpu::TextureAspect::All);
						return info;
					};
					auto bufferInfo = [&](const json& b) {
						wgpu::TexelCopyBufferInfo info = {};
						info.buffer = Get(idIn(b, "buffer")).buffer;
						info.layout.offset = num<uint64_t>(b, "offset", 0);
						if (b.contains("bytesPerRow")) info.layout.bytesPerRow = num<uint32_t>(b, "bytesPerRow", WGPU_COPY_STRIDE_UNDEFINED);
						if (b.contains("rowsPerImage")) info.layout.rowsPerImage = num<uint32_t>(b, "rowsPerImage", WGPU_COPY_STRIDE_UNDEFINED);
						return info;
					};
					const json& s = d["size"];
					wgpu::Extent3D size = { num<uint32_t>(s, "width", 1), num<uint32_t>(s, "height", 1), num<uint32_t>(s, "depthOrArrayLayers", 1) };
					if (!e.commandEncoder) break;
					if (op == 32) {
						auto a = bufferInfo(d["source"]);
						auto b = textureInfo(d["destination"]);
						e.commandEncoder.CopyBufferToTexture(&a, &b, &size);
					}
					else if (op == 33) {
						auto a = textureInfo(d["source"]);
						auto b = bufferInfo(d["destination"]);
						e.commandEncoder.CopyTextureToBuffer(&a, &b, &size);
					}
					else {
						auto a = textureInfo(d["source"]);
						auto b = textureInfo(d["destination"]);
						e.commandEncoder.CopyTextureToTexture(&a, &b, &size);
					}
					break;
				}
				case 35: { // resolveQuerySet
					auto& e = Get(r.u32());
					auto& q = Get(r.u32());
					uint32_t first = r.u32();
					uint32_t count = r.u32();
					auto& dst = Get(r.u32());
					double offset = r.f64();
					if (e.commandEncoder && q.querySet && dst.buffer) e.commandEncoder.ResolveQuerySet(q.querySet, first, count, dst.buffer, static_cast<uint64_t>(offset));
					break;
				}
				case 36: { // clearBuffer
					auto& e = Get(r.u32());
					auto& b = Get(r.u32());
					double offset = r.f64();
					double size = r.f64();
					if (e.commandEncoder && b.buffer) e.commandEncoder.ClearBuffer(b.buffer, static_cast<uint64_t>(offset), size < 0 ? WGPU_WHOLE_SIZE : static_cast<uint64_t>(size));
					break;
				}
				case 37: { // createRenderBundleEncoder
					uint32_t id = r.u32();
					json d = r.js();
					auto& o = Get(id);
					o.kind = Object::Kind::BundleEncoder;
					std::vector<wgpu::TextureFormat> colors;
					for (const auto& f : d["colorFormats"]) colors.push_back(f.is_string() ? formatOf(f.get<std::string>()) : wgpu::TextureFormat::Undefined);
					wgpu::RenderBundleEncoderDescriptor desc = {};
					desc.colorFormatCount = colors.size();
					desc.colorFormats = colors.data();
					if (d.contains("depthStencilFormat")) desc.depthStencilFormat = formatOf(str(d, "depthStencilFormat"));
					desc.sampleCount = num<uint32_t>(d, "sampleCount", 1);
					desc.depthReadOnly = flag(d, "depthReadOnly");
					desc.stencilReadOnly = flag(d, "stencilReadOnly");
					o.bundleEncoder = device_.CreateRenderBundleEncoder(&desc);
					break;
				}
				case 38: { // finishBundle
					uint32_t id = r.u32();
					uint32_t encoderId = r.u32();
					auto& o = Get(id);
					o.kind = Object::Kind::Bundle;
					auto& e = Get(encoderId);
					if (e.bundleEncoder) o.bundle = e.bundleEncoder.Finish();
					break;
				}
				default: {
					// Pass commands: the pass (render, compute or bundle encoder) first.
					uint32_t passId = r.u32();
					auto& p = Get(passId);
					auto* render = p.renderPass ? &p.renderPass : nullptr;
					auto* compute = p.computePass ? &p.computePass : nullptr;
					auto* bundle = p.bundleEncoder ? &p.bundleEncoder : nullptr;
					switch (op) {
					case 50: {
						auto& pipe = Get(r.u32());
						if (render && pipe.renderPipeline) render->SetPipeline(pipe.renderPipeline);
						else if (bundle && pipe.renderPipeline) bundle->SetPipeline(pipe.renderPipeline);
						else if (compute && pipe.computePipeline) compute->SetPipeline(pipe.computePipeline);
						break;
					}
					case 51: {
						uint32_t index = r.u32();
						auto& group = Get(r.u32());
						uint32_t count = r.u32();
						std::vector<uint32_t> offsets(count);
						for (auto& o : offsets) o = r.u32();
						if (render) render->SetBindGroup(index, group.bindGroup, count, offsets.data());
						else if (bundle) bundle->SetBindGroup(index, group.bindGroup, count, offsets.data());
						else if (compute) compute->SetBindGroup(index, group.bindGroup, count, offsets.data());
						break;
					}
					case 52: {
						uint32_t slot = r.u32();
						auto& b = Get(r.u32());
						double offset = r.f64();
						double size = r.f64();
						uint64_t s = size < 0 ? WGPU_WHOLE_SIZE : static_cast<uint64_t>(size);
						if (render) render->SetVertexBuffer(slot, b.buffer, static_cast<uint64_t>(offset), s);
						else if (bundle) bundle->SetVertexBuffer(slot, b.buffer, static_cast<uint64_t>(offset), s);
						break;
					}
					case 53: {
						auto& b = Get(r.u32());
						uint32_t format = r.u32();
						double offset = r.f64();
						double size = r.f64();
						uint64_t s = size < 0 ? WGPU_WHOLE_SIZE : static_cast<uint64_t>(size);
						auto f = format == 1 ? wgpu::IndexFormat::Uint32 : wgpu::IndexFormat::Uint16;
						if (render) render->SetIndexBuffer(b.buffer, f, static_cast<uint64_t>(offset), s);
						else if (bundle) bundle->SetIndexBuffer(b.buffer, f, static_cast<uint64_t>(offset), s);
						break;
					}
					case 54: {
						uint32_t a = r.u32(), b = r.u32(), c = r.u32(), d = r.u32();
						if (render) render->Draw(a, b, c, d);
						else if (bundle) bundle->Draw(a, b, c, d);
						break;
					}
					case 55: {
						uint32_t a = r.u32(), b = r.u32(), c = r.u32();
						int32_t base = r.i32();
						uint32_t d = r.u32();
						if (render) render->DrawIndexed(a, b, c, base, d);
						else if (bundle) bundle->DrawIndexed(a, b, c, base, d);
						break;
					}
					case 56:
					case 57: {
						auto& b = Get(r.u32());
						uint64_t offset = static_cast<uint64_t>(r.f64());
						if (op == 56) {
							if (render) render->DrawIndirect(b.buffer, offset);
							else if (bundle) bundle->DrawIndirect(b.buffer, offset);
						}
						else {
							if (render) render->DrawIndexedIndirect(b.buffer, offset);
							else if (bundle) bundle->DrawIndexedIndirect(b.buffer, offset);
						}
						break;
					}
					case 58: {
						float v[6];
						for (auto& x : v) x = r.f32();
						if (render) render->SetViewport(v[0], v[1], v[2], v[3], v[4], v[5]);
						break;
					}
					case 59: {
						uint32_t x = r.u32(), y = r.u32(), w = r.u32(), h = r.u32();
						if (render) render->SetScissorRect(x, y, w, h);
						break;
					}
					case 60: {
						wgpu::Color c = { r.f64(), r.f64(), r.f64(), r.f64() };
						if (render) render->SetBlendConstant(&c);
						break;
					}
					case 61: {
						uint32_t ref = r.u32();
						if (render) render->SetStencilReference(ref);
						break;
					}
					case 62: {
						uint32_t index = r.u32();
						if (render) render->BeginOcclusionQuery(index);
						break;
					}
					case 63:
						if (render) render->EndOcclusionQuery();
						break;
					case 64: {
						uint32_t count = r.u32();
						std::vector<wgpu::RenderBundle> bundles;
						for (uint32_t i = 0; i < count; i++) bundles.push_back(Get(r.u32()).bundle);
						if (render) render->ExecuteBundles(bundles.size(), bundles.data());
						break;
					}
					case 65:
						if (render) render->End();
						if (compute) compute->End();
						objects_.erase(passId);
						break;
					case 66: {
						uint32_t x = r.u32(), y = r.u32(), z = r.u32();
						if (compute) compute->DispatchWorkgroups(x, y, z);
						break;
					}
					case 67: {
						auto& b = Get(r.u32());
						uint64_t offset = static_cast<uint64_t>(r.f64());
						if (compute) compute->DispatchWorkgroupsIndirect(b.buffer, offset);
						break;
					}
					default:
						break;
					}
				}
				}
			}
			catch (const std::exception& e) {
				ReportError("internal", std::string("bridge: ") + e.what());
			}
		}
	}
}
