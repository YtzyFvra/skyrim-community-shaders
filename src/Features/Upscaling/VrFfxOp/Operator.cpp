#include "PCH.h"
#include "Operator.h"

#ifdef ENABLE_SKYRIM_VR

#	include "Features/Upscaling.h"
#	include "Features/Upscaling/FidelityFX.h"
#	include "Features/Upscaling/VrAriseTail.h"
#	include "Features/Upscaling/VrBeat/Metronome.h"
#	include "Features/Upscaling/VrPose/Absolute.h"
#	include "Globals.h"
#	include "Utils/Game.h"

#	include <array>
#	include <atomic>
#	include <cmath>
#	include <mutex>

#	include <d3dcompiler.h>
#	include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
	constexpr DWORD kFenceWaitMs = 16;

	std::mutex opMutex;
	ID3D12Device* device = nullptr;
	ID3D12CommandQueue* queue = nullptr;
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12Fence> fence;
	HANDLE fenceEvent = nullptr;
	UINT64 fenceValue = 0;
	ComPtr<ID3D12Resource> interp;
	ffx::Context context = nullptr;
	bool haveContext = false;
	bool loggedCtxOk = false;
	UINT colorW = 0;
	UINT colorH = 0;
	UINT guideW = 0;
	UINT guideH = 0;
	DXGI_FORMAT colorFmt = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT depthFmt = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT mvecFmt = DXGI_FORMAT_UNKNOWN;
	uint64_t frameID = 0;
	uint64_t dispatches = 0;
	VrPose::Body prevKick{};
	std::atomic<bool> fgResetPending{ false };

	std::array<float, 4> ToQuaternion(const vr::HmdMatrix34_t& m)
	{
		const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
		std::array<float, 4> q{};
		if (trace > 0.0f) {
			const float s = std::sqrt(trace + 1.0f) * 2.0f;
			q = { (m.m[2][1] - m.m[1][2]) / s, (m.m[0][2] - m.m[2][0]) / s, (m.m[1][0] - m.m[0][1]) / s, 0.25f * s };
		} else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
			const float s = std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.0f;
			q = { 0.25f * s, (m.m[0][1] + m.m[1][0]) / s, (m.m[0][2] + m.m[2][0]) / s, (m.m[2][1] - m.m[1][2]) / s };
		} else if (m.m[1][1] > m.m[2][2]) {
			const float s = std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.0f;
			q = { (m.m[0][1] + m.m[1][0]) / s, 0.25f * s, (m.m[1][2] + m.m[2][1]) / s, (m.m[0][2] - m.m[2][0]) / s };
		} else {
			const float s = std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.0f;
			q = { (m.m[0][2] + m.m[2][0]) / s, (m.m[1][2] + m.m[2][1]) / s, 0.25f * s, (m.m[1][0] - m.m[0][1]) / s };
		}
		return q;
	}

	vr::HmdMatrix34_t FromQuaternion(const std::array<float, 4>& q)
	{
		const float x = q[0], y = q[1], z = q[2], w = q[3];
		vr::HmdMatrix34_t m{};
		m.m[0][0] = 1.0f - 2.0f * (y * y + z * z);
		m.m[0][1] = 2.0f * (x * y - z * w);
		m.m[0][2] = 2.0f * (x * z + y * w);
		m.m[1][0] = 2.0f * (x * y + z * w);
		m.m[1][1] = 1.0f - 2.0f * (x * x + z * z);
		m.m[1][2] = 2.0f * (y * z - x * w);
		m.m[2][0] = 2.0f * (x * z - y * w);
		m.m[2][1] = 2.0f * (y * z + x * w);
		m.m[2][2] = 1.0f - 2.0f * (x * x + y * y);
		return m;
	}

	vr::HmdMatrix34_t InterpolatePose(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b, float t)
	{
		std::array<float, 4> qa = ToQuaternion(a);
		std::array<float, 4> qb = ToQuaternion(b);
		float dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
		if (dot < 0.0f) {
			for (float& v : qb)
				v = -v;
			dot = -dot;
		}
		std::array<float, 4> q{};
		if (dot > 0.9995f) {
			for (int i = 0; i < 4; ++i)
				q[i] = qa[i] + (qb[i] - qa[i]) * t;
		} else {
			const float theta = std::acos(dot);
			const float sinTheta = std::sin(theta);
			const float wa = std::sin((1.0f - t) * theta) / sinTheta;
			const float wb = std::sin(t * theta) / sinTheta;
			for (int i = 0; i < 4; ++i)
				q[i] = qa[i] * wa + qb[i] * wb;
		}
		const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
		for (float& v : q)
			v /= length;
		vr::HmdMatrix34_t out = FromQuaternion(q);
		for (int row = 0; row < 3; ++row)
			out.m[row][3] = a.m[row][3] + (b.m[row][3] - a.m[row][3]) * t;
		return out;
	}

	bool WaitOwnFence(DWORD ms)
	{
		if (!fence || fenceValue == 0)
			return true;
		if (fence->GetCompletedValue() >= fenceValue)
			return true;
		if (!fenceEvent || FAILED(fence->SetEventOnCompletion(fenceValue, fenceEvent)))
			return false;
		return WaitForSingleObject(fenceEvent, ms) == WAIT_OBJECT_0;
	}

	bool DestroyContextIfAny()
	{
		if (!haveContext && !context)
			return true;
		// Queue12 still holds FFX internals. An unbounded wait here is the doorbell going silent.
		LARGE_INTEGER freq{}, start{}, end{};
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&start);
		const bool done = WaitOwnFence(1000);
		QueryPerformanceCounter(&end);
		const double ms = freq.QuadPart ?
			(static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)) : 0.0;
		if (!done) {
			logger::warn("[VrFfxOp] destroy fence timeout {:.0f} ms", ms);
			return false;
		}
		if (ms > 3.0)
			logger::info("[VrFfxOp] destroy fence {:.2f} ms", ms);
		if (context && ffxModule.DestroyContext)
			(void)ffx::DestroyContext(context);
		context = nullptr;
		haveContext = false;
		return true;
	}

	void ShutdownUnlocked()
	{
		if (!DestroyContextIfAny())
			return;
		interp.Reset();
		list.Reset();
		allocator.Reset();
		fence.Reset();
		if (fenceEvent) {
			CloseHandle(fenceEvent);
			fenceEvent = nullptr;
		}
		device = nullptr;
		queue = nullptr;
		fenceValue = 0;
		colorW = 0;
		colorH = 0;
		guideW = 0;
		guideH = 0;
		colorFmt = DXGI_FORMAT_UNKNOWN;
		depthFmt = DXGI_FORMAT_UNKNOWN;
		mvecFmt = DXGI_FORMAT_UNKNOWN;
		frameID = 0;
		loggedCtxOk = false;
		prevKick = {};
	}

	bool EnsureCmd()
	{
		if (list)
			return true;
		if (!device)
			return false;
		if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
			FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
				IID_PPV_ARGS(&list))) ||
			FAILED(list->Close()) ||
			FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
			return false;
		fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		return fenceEvent != nullptr;
	}

	bool EnsureInterp(UINT w, UINT h)
	{
		if (interp) {
			const auto desc = interp->GetDesc();
			if (desc.Width == w && desc.Height == h && desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM)
				return true;
		}
		interp.Reset();
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = w;
		desc.Height = h;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&interp))))
			return false;
		interp->SetName(L"VrFfxOp::Interp");
		return true;
	}

	bool EnsureUnlocked(ID3D12Device* inDevice, ID3D12CommandQueue* inQueue,
		UINT inColorW, UINT inColorH, UINT inGuideW, UINT inGuideH,
		DXGI_FORMAT inColorFmt, DXGI_FORMAT inDepthFmt, DXGI_FORMAT inMvecFmt)
	{
		if (!inDevice || !inQueue || !inColorW || !inColorH || !inGuideW || !inGuideH)
			return false;
		if (!globals::features::upscaling.fidelityFX.featureFSR3FG ||
			!ffxModule.CreateContext || !ffxModule.DestroyContext ||
			!ffxModule.Configure || !ffxModule.Dispatch)
			return false;

		if (device && (device != inDevice || queue != inQueue))
			ShutdownUnlocked();
		device = inDevice;
		queue = inQueue;

		const bool sizeChanged = haveContext &&
			(colorW != inColorW || colorH != inColorH || colorFmt != inColorFmt);
		if (sizeChanged && !DestroyContextIfAny())
			return false;

		if (!EnsureCmd() || !EnsureInterp(inColorW, inColorH))
			return false;

		if (!haveContext) {
			ffx::CreateContextDescFrameGeneration createFg{};
			createFg.displaySize = { inColorW, inColorH };
			createFg.maxRenderSize = { inColorW, inColorH };
			createFg.flags = 0;
			createFg.backBufferFormat = ffxApiGetSurfaceFormatDX12(DXGI_FORMAT_R8G8B8A8_UNORM);
			ffx::CreateBackendDX12Desc backendDesc{};
			backendDesc.device = inDevice;
			if (ffx::CreateContext(context, nullptr, createFg, backendDesc) != ffx::ReturnCode::Ok) {
				context = nullptr;
				logger::error("[VrFfxOp] CreateContext failed");
				return false;
			}
			haveContext = true;
			if (!loggedCtxOk) {
				loggedCtxOk = true;
				logger::info("[VrFfxOp] ctx ok {}x{} guide {}x{}", inColorW, inColorH, inGuideW, inGuideH);
			}
		}

		colorW = inColorW;
		colorH = inColorH;
		guideW = inGuideW;
		guideH = inGuideH;
		colorFmt = inColorFmt;
		depthFmt = inDepthFmt;
		mvecFmt = inMvecFmt;
		return true;
	}

	bool DispatchUnlocked(ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* mvec,
		UINT inColorW, UINT inColorH, UINT inGuideW, UINT inGuideH)
	{
		if (!haveContext || !list || !queue || !interp || !color || !depth || !mvec)
			return false;
		if (!globals::game::cameraNear || !globals::game::cameraFar)
			return false;
		if (!WaitOwnFence(kFenceWaitMs))
			return false;
		if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
			return false;

		auto& upscaling = globals::features::upscaling;
		const uint64_t id = frameID;

		ffx::ConfigureDescFrameGeneration config{};
		config.swapChain = nullptr;
		config.presentCallback = nullptr;
		config.presentCallbackUserContext = nullptr;
		config.frameGenerationCallback = nullptr;
		config.frameGenerationCallbackUserContext = nullptr;
		config.frameGenerationEnabled = true;
		config.allowAsyncWorkloads = false;
		// Dispatch interpolates presentColor. HUDLess is the scene; UI is stamped after.
		config.HUDLessColor = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_COMMON);
		config.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
		config.onlyPresentGenerated = false;
		config.generationRect.left = 0;
		config.generationRect.top = 0;
		config.generationRect.width = inColorW;
		config.generationRect.height = inColorH;
		config.frameID = id;

		ffx::DispatchDescFrameGenerationPrepare prepare{};
		prepare.commandList = list.Get();
		prepare.frameID = id;
		prepare.flags = 0;
		// motionVectorScale is a multiplier. MV stays at render size and the
		// display-resolution flag is off, so 1.0 is one render-size eye.
		prepare.renderSize = { inGuideW, inGuideH };
		prepare.motionVectorScale.x = static_cast<float>(inGuideW) * 0.5f;
		prepare.motionVectorScale.y = static_cast<float>(inGuideH);
		static bool loggedScale = false;
		if (!loggedScale && (inColorW != inGuideW || inColorH != inGuideH)) {
			loggedScale = true;
			logger::info("[VrFfxOp] FG render {}x{} display {}x{} mvecScale=({:.0f},{:.0f})",
				inGuideW, inGuideH, inColorW, inColorH,
				prepare.motionVectorScale.x, prepare.motionVectorScale.y);
		}
		prepare.jitterOffset.x = -upscaling.jitter.x;
		prepare.jitterOffset.y = -upscaling.jitter.y;
		prepare.frameTimeDelta = RE::GetSecondsSinceLastFrame() * 1000.f;
		prepare.cameraNear = *globals::game::cameraNear;
		prepare.cameraFar = *globals::game::cameraFar;
		prepare.cameraFovAngleVertical = Util::GetVerticalFOVRad();
		prepare.viewSpaceToMetersFactor = 0.01428222656f;
		prepare.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMMON);
		prepare.motionVectors = ffxApiGetResourceDX12(mvec, FFX_API_RESOURCE_STATE_COMMON);

		ffx::DispatchDescFrameGenerationPrepareCameraInfo cameraConfig{};
		auto viewMatrix = globals::game::frameBufferCached.GetCameraViewInverse(0).Transpose();
		cameraConfig.cameraRight[0] = viewMatrix._11;
		cameraConfig.cameraRight[1] = viewMatrix._12;
		cameraConfig.cameraRight[2] = viewMatrix._13;
		cameraConfig.cameraUp[0] = viewMatrix._21;
		cameraConfig.cameraUp[1] = viewMatrix._22;
		cameraConfig.cameraUp[2] = viewMatrix._23;
		cameraConfig.cameraForward[0] = viewMatrix._31;
		cameraConfig.cameraForward[1] = viewMatrix._32;
		cameraConfig.cameraForward[2] = viewMatrix._33;
		cameraConfig.cameraPosition[0] = globals::game::frameBufferCached.GetCameraPosAdjust(0).x;
		cameraConfig.cameraPosition[1] = globals::game::frameBufferCached.GetCameraPosAdjust(0).y;
		cameraConfig.cameraPosition[2] = globals::game::frameBufferCached.GetCameraPosAdjust(0).z;

		ffx::DispatchDescFrameGeneration generate{};
		generate.commandList = list.Get();
		generate.presentColor = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_COMMON);
		generate.outputs[0] = ffxApiGetResourceDX12(interp.Get(), FFX_API_RESOURCE_STATE_COMMON);
		generate.numGeneratedFrames = 1;
		generate.reset = fgResetPending.exchange(false, std::memory_order_acq_rel);
		generate.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
		generate.generationRect.left = 0;
		generate.generationRect.top = 0;
		generate.generationRect.width = inColorW;
		generate.generationRect.height = inColorH;
		generate.frameID = id;

		bool ok = ffx::Configure(context, config) == ffx::ReturnCode::Ok &&
			ffx::Dispatch(context, prepare, cameraConfig) == ffx::ReturnCode::Ok &&
			ffx::Dispatch(context, generate) == ffx::ReturnCode::Ok;
		if (FAILED(list->Close()))
			ok = false;
		if (!ok)
			return false;

		ID3D12CommandList* lists[]{ list.Get() };
		queue->ExecuteCommandLists(1, lists);
		++fenceValue;
		if (FAILED(queue->Signal(fence.Get(), fenceValue)))
			return false;
		++frameID;
		if (!WaitOwnFence(kFenceWaitMs))
			return false;
		// Same Queue12 as VrBeat::CopyGpuInterp (Tail dx12->queue). In-queue
		// order already covers this barrier; the second wait is extra.
		if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
			return false;
		D3D12_RESOURCE_BARRIER toCommon{};
		toCommon.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toCommon.Transition.pResource = interp.Get();
		toCommon.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		toCommon.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		toCommon.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		list->ResourceBarrier(1, &toCommon);
		if (FAILED(list->Close()))
			return false;
		ID3D12CommandList* toCommonLists[]{ list.Get() };
		queue->ExecuteCommandLists(1, toCommonLists);
		++fenceValue;
		if (FAILED(queue->Signal(fence.Get(), fenceValue)))
			return false;
		return WaitOwnFence(kFenceWaitMs);
	}
}

bool VrFfxOp::Ensure(ID3D12Device* inDevice, ID3D12CommandQueue* inQueue,
	UINT inColorW, UINT inColorH, UINT inGuideW, UINT inGuideH,
	DXGI_FORMAT inColorFmt, DXGI_FORMAT inDepthFmt, DXGI_FORMAT inMvecFmt)
{
	std::lock_guard lock(opMutex);
	return EnsureUnlocked(inDevice, inQueue, inColorW, inColorH, inGuideW, inGuideH,
		inColorFmt, inDepthFmt, inMvecFmt);
}

void VrFfxOp::NoteDiscontinuity()
{
	std::lock_guard lock(opMutex);
	const bool already = fgResetPending.exchange(true, std::memory_order_acq_rel);
	if (!already || prevKick.valid)
		logger::info("[VrFfxOp] discontinuity: FG reset, prevKick cleared");
	prevKick = {};
}

	bool StampOne(ID3D12Resource* dst, ID3D12Resource* hud, UINT outW, UINT outH, UINT hudW, UINT hudH)
	{
		static ComPtr<ID3D12RootSignature> root;
		static ComPtr<ID3D12PipelineState> pso;
		static ComPtr<ID3D12DescriptorHeap> heap;
		if (!dst || !hud || !outW || !outH)
			return false;
		if (!root) {
			static const char kSrc[] =
				"Texture2D<float4> Hud : register(t0);\n"
				"SamplerState Samp : register(s0);\n"
				"RWTexture2D<float4> Out : register(u0);\n"
				"cbuffer Size : register(b0) { uint2 outSize; uint2 hudSize; };\n"
				"[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {\n"
				"  if (id.x >= outSize.x || id.y >= outSize.y) return;\n"
				"  float2 uv = (float2(id.xy) + 0.5) / float2(outSize);\n"
				"  float4 h = Hud.SampleLevel(Samp, uv, 0);\n"
				"  float4 c = Out[id.xy];\n"
				"  float a = saturate(h.a);\n"
				"  c.rgb = c.rgb * (1.0 - a) + h.rgb;\n"
				"  c.a = 1.0;\n"
				"  Out[id.xy] = c;\n"
				"}\n";
			ComPtr<ID3DBlob> blob;
			ComPtr<ID3DBlob> errors;
			if (FAILED(D3DCompile(kSrc, sizeof(kSrc) - 1, "VrHudStamp", nullptr, nullptr, "main", "cs_5_1",
					0, 0, &blob, &errors))) {
				logger::warn("[VrFfxOp] HUD stamp CS failed: {}",
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
				return false;
			}
			D3D12_DESCRIPTOR_RANGE ranges[2]{};
			ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			ranges[0].NumDescriptors = 1;
			ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			ranges[1].NumDescriptors = 1;
			D3D12_ROOT_PARAMETER params[2]{};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[0].DescriptorTable.NumDescriptorRanges = 2;
			params[0].DescriptorTable.pDescriptorRanges = ranges;
			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[1].Constants.Num32BitValues = 4;
			params[1].Constants.ShaderRegister = 0;
			D3D12_STATIC_SAMPLER_DESC samp{};
			samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			D3D12_ROOT_SIGNATURE_DESC rsDesc{};
			rsDesc.NumParameters = 2;
			rsDesc.pParameters = params;
			rsDesc.NumStaticSamplers = 1;
			rsDesc.pStaticSamplers = &samp;
			ComPtr<ID3DBlob> rsBlob;
			if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr)) ||
				FAILED(device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&root))))
				return false;
			D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
			psoDesc.pRootSignature = root.Get();
			psoDesc.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
			if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso))))
				return false;
			D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
			heapDesc.NumDescriptors = 2;
			heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap))))
				return false;
		}
		if (!WaitOwnFence(kFenceWaitMs) || FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
			return false;
		D3D12_RESOURCE_BARRIER toUav{};
		toUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toUav.Transition.pResource = dst;
		toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		toUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		toUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		list->ResourceBarrier(1, &toUav);
		const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = hud->GetDesc().Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(hud, &srvDesc, cpu);
		cpu.ptr += inc;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = dst->GetDesc().Format;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device->CreateUnorderedAccessView(dst, nullptr, &uavDesc, cpu);
		ID3D12DescriptorHeap* heaps[]{ heap.Get() };
		list->SetDescriptorHeaps(1, heaps);
		list->SetComputeRootSignature(root.Get());
		list->SetPipelineState(pso.Get());
		list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
		const UINT constants[4]{ outW, outH, hudW, hudH };
		list->SetComputeRoot32BitConstants(1, 4, constants, 0);
		list->Dispatch((outW + 7) / 8, (outH + 7) / 8, 1);
		D3D12_RESOURCE_BARRIER toCommon = toUav;
		toCommon.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		toCommon.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		list->ResourceBarrier(1, &toCommon);
		if (FAILED(list->Close()))
			return false;
		ID3D12CommandList* lists[]{ list.Get() };
		queue->ExecuteCommandLists(1, lists);
		++fenceValue;
		if (FAILED(queue->Signal(fence.Get(), fenceValue)))
			return false;
		return WaitOwnFence(kFenceWaitMs);
	}

	bool LogStampedPixel(ID3D12Resource* src, UINT x, UINT y)
	{
		if (!src || !device || !queue || !list || !allocator)
			return false;
		ComPtr<ID3D12Resource> readback;
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC buffer{};
		buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buffer.Width = 256;
		buffer.Height = 1;
		buffer.DepthOrArraySize = 1;
		buffer.MipLevels = 1;
		buffer.Format = DXGI_FORMAT_UNKNOWN;
		buffer.SampleDesc.Count = 1;
		buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
			return false;
		if (!WaitOwnFence(kFenceWaitMs) || FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
			return false;
		D3D12_RESOURCE_BARRIER toCopy{};
		toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toCopy.Transition.pResource = src;
		toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		list->ResourceBarrier(1, &toCopy);
		D3D12_TEXTURE_COPY_LOCATION dstLoc{};
		dstLoc.pResource = readback.Get();
		dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		D3D12_RESOURCE_DESC srcDesc = src->GetDesc();
		device->GetCopyableFootprints(&srcDesc, 0, 1, 0, &dstLoc.PlacedFootprint, nullptr, nullptr, nullptr);
		dstLoc.PlacedFootprint.Footprint.Width = 1;
		dstLoc.PlacedFootprint.Footprint.Height = 1;
		D3D12_TEXTURE_COPY_LOCATION srcLoc{};
		srcLoc.pResource = src;
		srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_BOX box{ x, y, 0, x + 1, y + 1, 1 };
		list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);
		D3D12_RESOURCE_BARRIER back = toCopy;
		back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
		back.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		list->ResourceBarrier(1, &back);
		if (FAILED(list->Close()))
			return false;
		ID3D12CommandList* lists[]{ list.Get() };
		queue->ExecuteCommandLists(1, lists);
		++fenceValue;
		if (FAILED(queue->Signal(fence.Get(), fenceValue)) || !WaitOwnFence(kFenceWaitMs))
			return false;
		void* mapped = nullptr;
		if (FAILED(readback->Map(0, nullptr, &mapped)) || !mapped)
			return false;
		const auto* px = static_cast<const std::uint8_t*>(mapped);
		logger::info("[VrFfxOp] HUD stamp readback tex={} px={},{} rgba={},{},{},{}",
			static_cast<void*>(src), x, y, px[0], px[1], px[2], px[3]);
		readback->Unmap(0, nullptr);
		return true;
	}

	void StampHud(ID3D12Resource* generated, ID3D12Resource* scene, UINT outW, UINT outH)
	{
		const UINT64 want = VrAriseTail::HudFenceValue();
		void* texHandle = VrAriseTail::HudSharedHandle();
		void* fenceHandle = VrAriseTail::HudFenceHandle();
		if (!want || !texHandle || !fenceHandle || !device || !queue || !list)
			return;
		static ComPtr<ID3D12Resource> hud12;
		static ComPtr<ID3D12Fence> hudFence12;
		static void* openedTex = nullptr;
		static void* openedFence = nullptr;
		static HANDLE hudEvent = nullptr;
		static bool logged = false;
		if (openedTex != texHandle || openedFence != fenceHandle) {
			hud12.Reset();
			hudFence12.Reset();
			openedTex = nullptr;
			openedFence = nullptr;
			if (FAILED(device->OpenSharedHandle(static_cast<HANDLE>(texHandle), IID_PPV_ARGS(&hud12))) ||
				FAILED(device->OpenSharedHandle(static_cast<HANDLE>(fenceHandle), IID_PPV_ARGS(&hudFence12))))
				return;
			openedTex = texHandle;
			openedFence = fenceHandle;
		}
		if (!hudEvent)
			hudEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!hudEvent)
			return;
		ResetEvent(hudEvent);
		if (hudFence12->GetCompletedValue() < want) {
			if (FAILED(hudFence12->SetEventOnCompletion(want, hudEvent)) ||
				WaitForSingleObject(hudEvent, kFenceWaitMs) != WAIT_OBJECT_0)
				return;
		}
		const UINT hudW = static_cast<UINT>(hud12->GetDesc().Width);
		const UINT hudH = hud12->GetDesc().Height;
		const bool sceneOk = StampOne(scene, hud12.Get(), outW, outH, hudW, hudH);
		const bool genOk = StampOne(generated, hud12.Get(), outW, outH, hudW, hudH);
		if (!logged && (sceneOk || genOk)) {
			logged = true;
			logger::info("[VrFfxOp] HUD stamp {}x{} onto {}x{} scene={} gen={} submitTex={}",
				hudW, hudH, outW, outH, sceneOk ? 1 : 0, genOk ? 1 : 0,
				static_cast<void*>(scene));
		}
		static bool loggedPixel = false;
		if (sceneOk && scene && !loggedPixel)
			loggedPixel = LogStampedPixel(scene, outW / 4, outH / 2);
	}

bool VrFfxOp::OnDbFinalColor(ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* mvec,
	UINT inColorW, UINT inColorH, UINT inGuideW, UINT inGuideH,
	DXGI_FORMAT inColorFmt, DXGI_FORMAT inDepthFmt, DXGI_FORMAT inMvecFmt)
{
	if (!color || !inColorW || !inColorH)
		return false;

	std::lock_guard lock(opMutex);
	VrPoseAbs::StampReady();
	const VrPose::Body cur = VrPoseAbs::Ready();

	bool dispatchOk = false;
	LARGE_INTEGER freq{}, start{}, end{};
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&start);
	if (depth && mvec && inGuideW && inGuideH && device && queue &&
		EnsureUnlocked(device, queue, inColorW, inColorH, inGuideW, inGuideH,
			inColorFmt, inDepthFmt, inMvecFmt))
		dispatchOk = DispatchUnlocked(color, depth, mvec, inColorW, inColorH, inGuideW, inGuideH);
	QueryPerformanceCounter(&end);
	++dispatches;
	const double ms = freq.QuadPart ?
		(static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)) :
		0.0;
	if (dispatches <= 8 || (dispatches % 120) == 0)
		logger::info("[VrFfxOp] dispatch {:.2f} ms ok={}", ms, dispatchOk ? 1 : 0);

	StampHud(dispatchOk ? interp.Get() : nullptr, color, inColorW, inColorH);

	const std::uint64_t pair = VrBeat::NextPair();
	if (dispatchOk && cur.valid) {
		// Midpoint of the HMD (head). OC expands both eyes; per-eye slerp
		// would break the rigid IPD.
		const vr::HmdMatrix34_t mid = prevKick.valid ?
			InterpolatePose(prevKick.m, cur.m, 0.5f) :
			cur.m;
		VrBeat::OfferGInterpolated(interp.Get(), inColorW, inColorH, inColorFmt, mid, pair);
	}

	const bool copied = VrBeat::CopySbs(color, inColorW, inColorH, inColorFmt, cur.m, cur.valid, pair);
	if (!copied)
		VrBeat::CancelInterp();
	if (copied && cur.valid)
		prevKick = cur;

	static LARGE_INTEGER paceFreq{}, paceNext{};
	if (!paceFreq.QuadPart)
		QueryPerformanceFrequency(&paceFreq);
	LARGE_INTEGER paceNow{};
	QueryPerformanceCounter(&paceNow);
	if (paceNext.QuadPart == 0 || paceNow.QuadPart >= paceNext.QuadPart) {
		paceNext.QuadPart = paceNow.QuadPart + paceFreq.QuadPart * 4;
		auto& perf = globals::features::upscaling.perfMode;
		const UINT tdW = perf.GetDisplayEyeWidth() * 2;
		const UINT tdH = perf.GetDisplayEyeHeight();
		const bool fullTd = tdW && tdH && inColorW == tdW && inColorH == tdH;
		const auto& fov = globals::features::upscaling.foveatedRender;
		const double engineMs = RE::GetSecondsSinceLastFrame() * 1000.0;
		logger::info("[VrFfxOp] pace dispatch={:.2f} ms color={}x{} td={} nr={} q={} engine={:.1f} ms kickFence={:.2f} copyFence={:.2f} want={} consumed={}",
			ms, inColorW, inColorH, fullTd ? 1 : 0,
			fov.settings.neuralRenderingEnabled ? 1 : 0, fov.GetQualityModeAtBoot(),
			engineMs, VrAriseTail::LastKickFenceMs(), VrBeat::LastCopyFenceMs(),
			VrAriseTail::LastPublishFence(), VrAriseTail::LastConsumedFence());
	}
	return copied;
}

void VrFfxOp::Shutdown()
{
	std::lock_guard lock(opMutex);
	ShutdownUnlocked();
}

#endif
