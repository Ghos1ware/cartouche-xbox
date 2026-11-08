#pragma once
#include <string>
#include <vector>

namespace moonlight_xbox_dx {
	// Diagnostics (the « d3d12-test » file in LocalState): what DirectX 12 does in this app on this console,
	// outside the browser. The adapter and its memory budgets, a device, a graphics pipeline from DXBC (FXC)
	// and from DXIL (DXC), a triangle drawn and read back, then how much the device can allocate. One line each.
	std::vector<std::wstring> RunD3D12Probe();
	// The « pso-lab » file: lab-vs.hlsl / lab-ps.hlsl from LocalState (Dawn's dumped shaders) compiled by FXC and
	// made into a pipeline in every variant of lab.txt's lines (« vs_5_1 ps_5_1 O0 consts BGRA ieee »), a new device
	// each: which one the console's driver refuses (DXGI_ERROR_DRIVER_INTERNAL_ERROR) and which it takes.
	// d3d12Device: an ID3D12Device to run on (Dawn's), or null for a new one per variant.
	std::vector<std::wstring> RunPsoLab(const std::wstring& folder, void* d3d12Device = nullptr);
}
