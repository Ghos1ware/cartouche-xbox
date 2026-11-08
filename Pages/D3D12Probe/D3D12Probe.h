#pragma once
#include <string>
#include <vector>

namespace moonlight_xbox_dx {
	// Diagnostics (the « d3d12-test » file in LocalState): what DirectX 12 does in this app on this console,
	// outside the browser. The adapter and its memory budgets, a device, a graphics pipeline from DXBC (FXC)
	// and from DXIL (DXC), a triangle drawn and read back, then how much the device can allocate. One line each.
	std::vector<std::wstring> RunD3D12Probe();
}
