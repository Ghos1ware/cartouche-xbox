#include "pch.h"
#include "D3D12Probe.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <d3dcompiler.h>
#include <d3d12shader.h>
#pragma comment(lib, "d3dcompiler.lib")

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

	namespace {
		std::string ReadText(const std::wstring& path) {
			std::ifstream in(path, std::ios::binary);
			return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		}
		std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
		// Dawn's own bytes (lab-vs.hex, lab-ps.hex, lab-root.hex), replayed as they are.
		std::vector<uint8_t> rawVs, rawPs, rawRoot;
		std::vector<uint8_t> Unhex(const std::string& text) {
			std::vector<uint8_t> out;
			auto digit = [](char c) { return c >= 97 ? c - 87 : c >= 65 ? c - 55 : c - 48; };
			for (size_t i = 0; i + 1 < text.size(); i += 2) {
				if (!isxdigit(static_cast<unsigned char>(text[i]))) break;
				out.push_back(static_cast<uint8_t>(digit(text[i]) * 16 + digit(text[i + 1])));
			}
			return out;
		}

		// One variant: compile both stages, a root signature with what the vertex and fragment shaders ask
		// (root constants for each cbuffer, a table for the rest), the pipeline on a new device.
		std::wstring LabVariant(std::string vsCode, std::string psCode, const std::string& line, ID3D12Device* given = nullptr) {
			auto has = [&](const char* w) { return (" " + line + " ").find(std::string(" ") + w + " ") != std::string::npos; };
			auto replaceAll = [](std::string& text, const std::string& from, const std::string& to) {
				for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
			};
			// Variants of the code itself: the cbuffer's space 8 moved to 0; a plain shader with no array (control).
			if (has("space0")) {
				replaceAll(vsCode, "space8", "space0");
				replaceAll(psCode, "space8", "space0");
			}
			if (has("plain")) {
				vsCode = "struct O { float3 c : TEXCOORD0; float4 p : SV_Position; }; O dawn_entry_point(uint i : SV_VertexID) { O o; o.p = float4(i == 0 ? 0.0 : (i == 1 ? -0.7 : 0.7), i == 0 ? 0.7 : -0.6, 0, 1); o.c = float3(1, 0.5, 0.2); return o; }";
				psCode = "float4 dawn_entry_point(float3 c : TEXCOORD0) : SV_Target0 { return float4(c, 1); }";
			}
			std::string vsTarget = has("vs_5_0") ? "vs_5_0" : "vs_5_1";
			std::string psTarget = has("ps_5_0") ? "ps_5_0" : "ps_5_1";
			UINT flags = has("O0") ? D3DCOMPILE_OPTIMIZATION_LEVEL0 : has("O3") ? D3DCOMPILE_OPTIMIZATION_LEVEL3 : D3DCOMPILE_OPTIMIZATION_LEVEL1;
			if (has("ieee")) flags |= D3DCOMPILE_IEEE_STRICTNESS;
			if (has("skip")) flags |= D3DCOMPILE_SKIP_OPTIMIZATION;
			std::string vsEntry = "vs", psEntry = "fs";
			{
				size_t at = line.find("entry=");
				if (at != std::string::npos) {
					std::string pair = line.substr(at + 6, line.find(' ', at) - at - 6);
					size_t comma = pair.find(',');
					vsEntry = pair.substr(0, comma);
					if (comma != std::string::npos) psEntry = pair.substr(comma + 1);
				}
			}
			ComPtr<ID3DBlob> vs, ps, error;
			HRESULT hr = D3DCompile(vsCode.data(), vsCode.size(), "vs", nullptr, nullptr, vsEntry.c_str(), vsTarget.c_str(), flags, 0, &vs, &error);
			if (FAILED(hr)) return Wide(line) + L": vs compile " + Hex(hr) + L" " + Wide(error ? std::string(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize()).substr(0, 300) : "");
			hr = D3DCompile(psCode.data(), psCode.size(), "ps", nullptr, nullptr, psEntry.c_str(), psTarget.c_str(), flags, 0, &ps, &error);
			if (FAILED(hr)) return Wide(line) + L": ps compile " + Hex(hr) + L" " + Wide(error ? std::string(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize()).substr(0, 300) : "");

			// Root signature: every cbuffer of both stages as root constants (or a CBV), registers and spaces kept.
			std::vector<D3D12_ROOT_PARAMETER> params;
			std::wstring rootText;
			for (auto* blob : { vs.Get(), ps.Get() }) {
				ComPtr<ID3D12ShaderReflection> reflect;
				if (FAILED(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&reflect)))) continue;
				D3D12_SHADER_DESC sd = {};
				reflect->GetDesc(&sd);
				for (UINT i = 0; i < sd.BoundResources; i++) {
					D3D12_SHADER_INPUT_BIND_DESC bind = {};
					reflect->GetResourceBindingDesc(i, &bind);
					if (bind.Type != D3D_SIT_CBUFFER) continue;
					bool known = false;
					for (auto& p : params)
						if (p.Constants.ShaderRegister == bind.BindPoint && p.Constants.RegisterSpace == bind.Space) known = true;
					if (known) continue;
					D3D12_ROOT_PARAMETER p = {};
					auto* cb = reflect->GetConstantBufferByName(bind.Name);
					D3D12_SHADER_BUFFER_DESC cbd = {};
					cb->GetDesc(&cbd);
					if (has("cbv")) {
						p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
						p.Descriptor.ShaderRegister = bind.BindPoint;
						p.Descriptor.RegisterSpace = bind.Space;
					}
					else {
						p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
						p.Constants.ShaderRegister = bind.BindPoint;
						p.Constants.RegisterSpace = bind.Space;
						p.Constants.Num32BitValues = (cbd.Size + 3) / 4;
						size_t n = line.find("n=");
						if (n != std::string::npos) p.Constants.Num32BitValues = static_cast<UINT>(atoi(line.c_str() + n + 2));
					}
					p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
					params.push_back(p);
					rootText += L" " + Wide(bind.Name) + L"@b" + std::to_wstring(bind.BindPoint) + L",space" + std::to_wstring(bind.Space) + L"(" + std::to_wstring(cbd.Size) + L"B)";
				}
			}
			if (has("noroot")) params.clear();

			ComPtr<IDXGIFactory4> factory;
			CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
			ComPtr<IDXGIAdapter1> adapter;
			factory->EnumAdapters1(0, &adapter);
			ComPtr<ID3D12Device> device;
			if (given) device = given;
			else hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
			if (FAILED(hr)) return Wide(line) + L": device " + Hex(hr);
			// What Dawn makes before its first pipeline, as asked.
			ComPtr<ID3D12CommandQueue> labQueue;
			ComPtr<ID3D12Fence> labFence;
			ComPtr<ID3D12DescriptorHeap> labViews, labSamplers;
			HANDLE labShared = nullptr;
			std::wstring setup;
			if (has("queue")) {
				D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
				setup += L" queue=" + Hex(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&labQueue)));
				UINT64 freq = 0;
				if (labQueue) setup += L" freq=" + Hex(labQueue->GetTimestampFrequency(&freq));
				if (labQueue && has("names")) labQueue->SetName(L"Dawn queue");
			}
			if (has("sharedfence")) {
				setup += L" fence=" + Hex(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&labFence)));
				if (labFence) setup += L" handle=" + Hex(device->CreateSharedHandle(labFence.Get(), nullptr, GENERIC_ALL, nullptr, &labShared));
			}
			if (has("heaps")) {
				D3D12_DESCRIPTOR_HEAP_DESC v = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
				D3D12_DESCRIPTOR_HEAP_DESC sm = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
				setup += L" views=" + Hex(device->CreateDescriptorHeap(&v, IID_PPV_ARGS(&labViews)));
				setup += L" samplers=" + Hex(device->CreateDescriptorHeap(&sm, IID_PPV_ARGS(&labSamplers)));
			}
			if (!setup.empty()) setup += L" removed=" + Hex(device->GetDeviceRemovedReason());
			D3D12_ROOT_SIGNATURE_DESC rsd = {};
			rsd.NumParameters = static_cast<UINT>(params.size());
			rsd.pParameters = params.data();
			rsd.Flags = has("ia") ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT : D3D12_ROOT_SIGNATURE_FLAG_NONE;
			ComPtr<ID3DBlob> rootBlob;
			if (has("v11")) {
				std::vector<D3D12_ROOT_PARAMETER1> params1;
				for (auto& p : params) {
					D3D12_ROOT_PARAMETER1 q = {};
					q.ParameterType = p.ParameterType;
					q.ShaderVisibility = p.ShaderVisibility;
					if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) q.Constants = p.Constants;
					else {
						q.Descriptor.ShaderRegister = p.Descriptor.ShaderRegister;
						q.Descriptor.RegisterSpace = p.Descriptor.RegisterSpace;
					}
					params1.push_back(q);
				}
				D3D12_VERSIONED_ROOT_SIGNATURE_DESC v = {};
				v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
				v.Desc_1_1.NumParameters = static_cast<UINT>(params1.size());
				v.Desc_1_1.pParameters = params1.data();
				v.Desc_1_1.Flags = rsd.Flags;
				hr = D3D12SerializeVersionedRootSignature(&v, &rootBlob, &error);
			}
			else
				hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &rootBlob, &error);
			ComPtr<ID3D12RootSignature> root;
			if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&root));
			if (FAILED(hr)) return Wide(line) + L": root " + Hex(hr);

			D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
			desc.pRootSignature = root.Get();
			desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
			desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
			if (has("rawvs") && !rawVs.empty()) desc.VS = { rawVs.data(), rawVs.size() };
			if (has("rawps") && !rawPs.empty()) desc.PS = { rawPs.data(), rawPs.size() };
			ComPtr<ID3D12RootSignature> rawRootSig;
			if (has("rawroot") && !rawRoot.empty()) {
				HRESULT r = device->CreateRootSignature(0, rawRoot.data(), rawRoot.size(), IID_PPV_ARGS(&rawRootSig));
				if (FAILED(r)) return Wide(line) + L": raw root " + Hex(r);
				desc.pRootSignature = rawRootSig.Get();
			}
			desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			desc.SampleMask = UINT_MAX;
			desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			desc.RasterizerState.DepthClipEnable = TRUE;
			desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			desc.NumRenderTargets = 1;
			desc.RTVFormats[0] = has("BGRA") ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			if (has("dawnpso")) {
				// As Dawn's RenderPipelineD3D12.cpp fills it for a pass with one colour target and no depth.
				desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
				desc.RasterizerState.FrontCounterClockwise = TRUE;
				desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
				for (int i = 0; i < 8; i++) desc.BlendState.RenderTarget[i].LogicOp = D3D12_LOGIC_OP_NOOP;
				desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
				desc.BlendState.IndependentBlendEnable = TRUE;
				desc.DepthStencilState.DepthEnable = FALSE;
				desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
				desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
				desc.DepthStencilState.StencilEnable = FALSE;
				desc.DepthStencilState.StencilReadMask = 0xFF;
				desc.DepthStencilState.StencilWriteMask = 0xFF;
				desc.DepthStencilState.FrontFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
				desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
			}
			ComPtr<ID3D12PipelineState> pso;
			ULONGLONG t = GetTickCount64();
			hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
			std::wstring after;
			if (pso && has("getblob")) {
				ComPtr<ID3DBlob> cached;
				HRESULT b = pso->GetCachedBlob(&cached);
				after += L" getblob=" + Hex(b) + L"(" + std::to_wstring(cached ? cached->GetBufferSize() : 0) + L"B) removed=" + Hex(device->GetDeviceRemovedReason());
			}
			if (pso && has("setname")) {
				pso->SetName(L"Dawn pipeline");
				after += L" setname removed=" + Hex(device->GetDeviceRemovedReason());
			}
			HRESULT removed = device->GetDeviceRemovedReason();
			if (labShared) CloseHandle(labShared);
			return Wide(line) + (setup.empty() ? L"" : L" [" + setup + L" ]") + L": pipeline " + Hex(hr) + L" removed=" + Hex(removed) + L" in " + std::to_wstring(GetTickCount64() - t) + L"ms, vs " + std::to_wstring(vs->GetBufferSize()) + L"B ps " + std::to_wstring(ps->GetBufferSize()) + L"B, root" + (rootText.empty() ? L" empty" : rootText) + after;
		}
	}

	std::vector<std::wstring> RunPsoLab(const std::wstring& folder, void* d3d12Device) {
		std::vector<std::wstring> out;
		std::string vsCode = ReadText(folder + L"\\lab-vs.hlsl"), psCode = ReadText(folder + L"\\lab-ps.hlsl"), plan = ReadText(folder + L"\\lab.txt");
		if (psCode.empty()) psCode = vsCode;
		if (vsCode.empty()) return { L"pso lab: no lab-vs.hlsl" };
		rawVs = Unhex(ReadText(folder + L"\\lab-vs.hex"));
		rawPs = Unhex(ReadText(folder + L"\\lab-ps.hex"));
		rawRoot = Unhex(ReadText(folder + L"\\lab-root.hex"));
		std::istringstream lines(plan);
		std::string line;
		while (std::getline(lines, line)) {
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
			if (line.empty() || line[0] == '#') continue;
			try {
				out.push_back(L"pso lab " + LabVariant(vsCode, psCode, line, static_cast<ID3D12Device*>(d3d12Device)));
			}
			catch (...) {
				out.push_back(L"pso lab " + Wide(line) + L": threw");
			}
		}
		return out;
	}
}
