#pragma once
// The WebGPU bridge, the app's side (Bridge/PROTOCOL.md): the game's page records its WebGPU calls
// (Assets/webgpu-bridge.js), this replays them on native Dawn, on DirectX 12, and shows the picture in a
// SwapChainPanel under the page. Everything here runs on the UI thread.
//
// Dawn's C++ header needs C++20, which C++/CX (/ZW) cannot compile: this header shows none of Dawn, and
// GpuBridge.cpp is built on its own, in standard C++20, without the precompiled header.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace moonlight_xbox_dx {
	class GpuBridge {
	public:
		// Messages to the page (JSON): replies, errors, the device lost.
		using Send = std::function<void(const std::string& json)>;
		// What the bridge says about itself, for diagnostics.log.
		using Log = std::function<void(const std::string& line)>;
		GpuBridge(Send send, Log log);
		~GpuBridge();

		// The adapter and the device (D3D12). False, with why, when it cannot be had: the page keeps the browser's own.
		// What Dawn compiles is kept in cacheFolder (empty: nothing kept).
		bool Start(std::string& adapterJson, std::string& reason, const std::wstring& cacheFolder);
		std::string CacheStats() const;
		// Where the picture goes: the XAML SwapChainPanel, as its IInspectable.
		void SetPanel(void* swapChainPanel);
		// One flush of the page's commands.
		void Execute(const uint8_t* data, size_t length);
		// Dawn's callbacks (pipelines made, buffers mapped, work done): called every frame.
		void Tick();
		// The page went away (navigation, closed): everything it made let go.
		void Reset();
		// Dawn let go entirely (before a stream: the console allows its D3D11 only without a D3D12 device). Start again after.
		void Stop();
		// Step 2 of the plan: one native triangle, to see Dawn draw on the console without the page.
		bool Triangle(std::string& reason);

		struct Stats {
			uint64_t commands = 0;
			uint64_t flushes = 0;
			uint64_t presents = 0;
			uint64_t pipelines = 0;
			double pipelineMs = 0;
			uint64_t errors = 0;
			uint64_t objects = 0;
			// The canvas's picture, in pixels (0 until the page configures it).
			uint32_t surfaceWidth = 0, surfaceHeight = 0;
		};
		Stats stats() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};
}
