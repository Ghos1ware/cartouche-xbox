#include "pch.h"
#include "D3D12Probe.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <sstream>
#include <iomanip>

#include "vs_dxbc.h"
#include "ps_dxbc.h"
#include "vs_dxil.h"
#include "ps_dxil.h"

using Microsoft::WRL::ComPtr;

namespace moonlight_xbox_dx {
	namespace {
		std::wstring Hex(HRESULT hr) {
			std::wstringstream s;
			s << L"0x" << std::hex << std::setw(8) << std::setfill(L'0') << static_cast<unsigned long>(hr);
			return s.str();
		}
		std::wstring Mb(UINT64 bytes) { return std::to_wstring(bytes / 1048576) + L"MB"; }

		// A pipeline from these shaders, a triangle drawn into a 64×64 target, its centre read back.
		std::wstring DrawWith(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12RootSignature* root, const void* vs, size_t vsSize, const void* ps, size_t psSize, const wchar_t* label) {
			D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
			desc.pRootSignature = root;
			desc.VS = { vs, vsSize };
			desc.PS = { ps, psSize };
			desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			desc.SampleMask = UINT_MAX;
			desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			desc.RasterizerState.DepthClipEnable = TRUE;
			desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			desc.NumRenderTargets = 1;
			desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			ComPtr<ID3D12PipelineState> pso;
			ULONGLONG t = GetTickCount64();
			HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
			std::wstring line = std::wstring(label) + L" pipeline " + Hex(hr) + L" in " + std::to_wstring(GetTickCount64() - t) + L"ms";
			if (FAILED(hr)) return line + L" removed=" + Hex(device->GetDeviceRemovedReason());

			D3D12_HEAP_PROPERTIES gpuHeap = { D3D12_HEAP_TYPE_DEFAULT };
			D3D12_RESOURCE_DESC tex = {};
			tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			tex.Width = 64;
			tex.Height = 64;
			tex.DepthOrArraySize = 1;
			tex.MipLevels = 1;
			tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			tex.SampleDesc.Count = 1;
			tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			ComPtr<ID3D12Resource> target;
			hr = device->CreateCommittedResource(&gpuHeap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target));
			if (FAILED(hr)) return line + L", target " + Hex(hr);
			D3D12_HEAP_PROPERTIES readHeap = { D3D12_HEAP_TYPE_READBACK };
			D3D12_RESOURCE_DESC buf = {};
			buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			buf.Width = 256 * 64;
			buf.Height = 1;
			buf.DepthOrArraySize = 1;
			buf.MipLevels = 1;
			buf.SampleDesc.Count = 1;
			buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ComPtr<ID3D12Resource> readback;
			hr = device->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
			if (FAILED(hr)) return line + L", readback " + Hex(hr);

			D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
			ComPtr<ID3D12DescriptorHeap> rtvHeap;
			device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));
			D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
			device->CreateRenderTargetView(target.Get(), nullptr, rtv);

			ComPtr<ID3D12CommandAllocator> alloc;
			ComPtr<ID3D12GraphicsCommandList> list;
			device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
			device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(), IID_PPV_ARGS(&list));
			const float clear[4] = { 0, 0, 0, 1 };
			list->ClearRenderTargetView(rtv, clear, 0, nullptr);
			list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			list->SetGraphicsRootSignature(root);
			D3D12_VIEWPORT vp = { 0, 0, 64, 64, 0, 1 };
			D3D12_RECT sc = { 0, 0, 64, 64 };
			list->RSSetViewports(1, &vp);
			list->RSSetScissorRects(1, &sc);
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			list->DrawInstanced(3, 1, 0, 0);
			D3D12_RESOURCE_BARRIER barrier = {};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = target.Get();
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			list->ResourceBarrier(1, &barrier);
			D3D12_TEXTURE_COPY_LOCATION src = { target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
			D3D12_TEXTURE_COPY_LOCATION dst = { readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
			dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			hr = list->Close();
			if (FAILED(hr)) return line + L", close " + Hex(hr);
			ID3D12CommandList* lists[] = { list.Get() };
			queue->ExecuteCommandLists(1, lists);
			ComPtr<ID3D12Fence> fence;
			device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
			HANDLE done = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
			queue->Signal(fence.Get(), 1);
			fence->SetEventOnCompletion(1, done);
			DWORD waited = WaitForSingleObject(done, 5000);
			CloseHandle(done);
			HRESULT removed = device->GetDeviceRemovedReason();
			if (waited != WAIT_OBJECT_0 || FAILED(removed)) return line + L", wait " + std::to_wstring(waited) + L" removed=" + Hex(removed);
			BYTE* data = nullptr;
			D3D12_RANGE range = { 0, 256 * 64 };
			if (FAILED(readback->Map(0, &range, reinterpret_cast<void**>(&data)))) return line + L", map failed";
			const BYTE* px = data + 32 * 256 + 32 * 4;
			std::wstring color = std::to_wstring(px[0]) + L"," + std::to_wstring(px[1]) + L"," + std::to_wstring(px[2]);
			readback->Unmap(0, nullptr);
			return line + L", drawn: centre " + color + (px[1] > 200 && px[0] < 50 ? L" (green: ok)" : L" (unexpected)");
		}
	}

	std::vector<std::wstring> RunD3D12Probe() {
		std::vector<std::wstring> out;
		ComPtr<IDXGIFactory4> factory;
		HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
		if (FAILED(hr)) return { L"d3d12 factory " + Hex(hr) };
		ComPtr<IDXGIAdapter1> adapter1;
		hr = factory->EnumAdapters1(0, &adapter1);
		if (FAILED(hr)) return { L"d3d12 adapter " + Hex(hr) };
		DXGI_ADAPTER_DESC1 desc = {};
		adapter1->GetDesc1(&desc);
		out.push_back(std::wstring(L"d3d12 adapter ") + desc.Description + L" dedicated=" + Mb(desc.DedicatedVideoMemory) + L" shared=" + Mb(desc.SharedSystemMemory));
		ComPtr<ID3D12Device> device;
		hr = D3D12CreateDevice(adapter1.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
		out.push_back(L"d3d12 device " + Hex(hr));
		if (FAILED(hr)) return out;
		ComPtr<IDXGIAdapter3> adapter3;
		if (SUCCEEDED(adapter1.As(&adapter3))) {
			DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, nonLocal = {};
			adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
			adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
			out.push_back(L"d3d12 budget local=" + Mb(local.Budget) + L" (used " + Mb(local.CurrentUsage) + L") nonlocal=" + Mb(nonLocal.Budget) + L" (used " + Mb(nonLocal.CurrentUsage) + L")");
		}
		D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_6 };
		device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
		out.push_back(L"d3d12 shader model 0x" + std::to_wstring(sm.HighestShaderModel >> 4) + L"." + std::to_wstring(sm.HighestShaderModel & 0xF));

		D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
		ComPtr<ID3D12CommandQueue> queue;
		device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
		D3D12_ROOT_SIGNATURE_DESC rsd = {};
		ComPtr<ID3DBlob> blob, error;
		hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error);
		ComPtr<ID3D12RootSignature> root;
		if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
		out.push_back(L"d3d12 root signature " + Hex(hr));
		if (FAILED(hr)) return out;
		out.push_back(DrawWith(device.Get(), queue.Get(), root.Get(), g_vs_dxbc, sizeof(g_vs_dxbc), g_ps_dxbc, sizeof(g_ps_dxbc), L"d3d12 DXBC"));
		out.push_back(DrawWith(device.Get(), queue.Get(), root.Get(), g_vs_dxil, sizeof(g_vs_dxil), g_ps_dxil, sizeof(g_ps_dxil), L"d3d12 DXIL"));

		// How much it can hold: 64 MB textures in the GPU's heap until one is refused (4 GB at most), then let go.
		std::vector<ComPtr<ID3D12Resource>> held;
		D3D12_HEAP_PROPERTIES gpuHeap = { D3D12_HEAP_TYPE_DEFAULT };
		D3D12_RESOURCE_DESC tex = {};
		tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		tex.Width = 4096;
		tex.Height = 4096;
		tex.DepthOrArraySize = 1;
		tex.MipLevels = 1;
		tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		tex.SampleDesc.Count = 1;
		HRESULT last = S_OK;
		for (int i = 0; i < 64; i++) {
			ComPtr<ID3D12Resource> r;
			last = device->CreateCommittedResource(&gpuHeap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
			if (FAILED(last)) break;
			held.push_back(r);
		}
		std::wstring budget;
		if (adapter3) {
			DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
			adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
			budget = L" (local now " + Mb(local.CurrentUsage) + L" of budget " + Mb(local.Budget) + L")";
		}
		out.push_back(L"d3d12 allocated " + std::to_wstring(held.size() * 64) + L"MB of textures, stopped by " + Hex(last) + budget);
		held.clear();
		return out;
	}
}
