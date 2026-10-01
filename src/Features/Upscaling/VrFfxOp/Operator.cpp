#include "PCH.h"
#include "Operator.h"

#ifdef ENABLE_SKYRIM_VR

#	include "Features/Upscaling.h"
#	include "Features/Upscaling/FidelityFX.h"
#	include "Features/Upscaling/VrAriseTail.h"
#	include "Features/Upscaling/VrPassTimer.h"
#	include "Features/Upscaling/VrSharedDevice.h"
#	include "Features/Upscaling/VrBeat/Metronome.h"
#	include "Features/Upscaling/VrPose/Absolute.h"
#	include "Globals.h"
#	include "Utils/FileSystem.h"
#	include "Utils/Game.h"

#	include <algorithm>
#	include <array>
#	include <atomic>
#	include <cmath>
#	include <filesystem>
#	include <mutex>
#	include <vector>

#	include <d3dcompiler.h>
#	include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
	std::mutex opMutex;
	ID3D12Device* device = nullptr;
	ID3D12CommandQueue* queue = nullptr;
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12Fence> fence;
	HANDLE fenceEvent = nullptr;
	UINT64 fenceValue = 0;
	ID3D12Resource* pairedHud = nullptr;
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
	// EMA of FG's own dispatch+wait cost; DispatchUnlocked bails out before spending GPU
	// time once this eats most of the period, instead of risking the next real frame's phase.
	float fsrEma = 0.0f;
	struct StoredView
	{
		float right[3]{};
		float up[3]{};
		float fwd[3]{};
		float pos[3]{};
		float jitterX = 0.0f;
		float jitterY = 0.0f;
		float nearZ = 0.15f;
		float farZ = 2000.0f;
		float fov = 1.0f;
		float frameMs = 11.11f;
		bool valid = false;
	};
	StoredView latchedView;

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
			(colorW != inColorW || colorH != inColorH || guideW != inGuideW || guideH != inGuideH ||
				colorFmt != inColorFmt);
		if (sizeChanged && !DestroyContextIfAny())
			return false;

		if (!EnsureCmd() || !EnsureInterp(inColorW, inColorH))
			return false;

		if (!haveContext) {
			ffx::CreateContextDescFrameGeneration createFg{};
			createFg.displaySize = { inColorW, inColorH };
			createFg.maxRenderSize = { (std::max)(inColorW, inGuideW), (std::max)(inColorH, inGuideH) };
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
		UINT inColorW, UINT inColorH, UINT inGuideW, UINT inGuideH, float mvecScaleX, float mvecScaleY)
	{
		if (!haveContext || !list || !queue || !interp || !color || !depth || !mvec)
			return false;
		if (!globals::game::cameraNear || !globals::game::cameraFar)
			return false;

		// FG admission: bail before spending any GPU time if the last dispatch already
		// ate most of this period's budget. OnDbFinalColor's dispatchOk=false path already
		// presents the real frame without an interp companion, so skipping here just keeps
		// the next real frame's phase steady instead of risking a late Evaluate.
		const float periodMs = latchedView.valid ? latchedView.frameMs : 11.11f;
		constexpr float kFgBudgetFraction = 0.85f;
		if (fsrEma > periodMs * kFgBudgetFraction) {
			static uint32_t admissionSkips = 0;
			++admissionSkips;
			if (admissionSkips <= 8 || (admissionSkips % 120) == 0)
				logger::info("[VrFfxOp] FG admission skip ema={:.2f} ms budget={:.2f} ms", fsrEma, periodMs * kFgBudgetFraction);
			fsrEma *= 0.5f; // decay so one spike doesn't wedge FG off indefinitely
			return false;
		}

		if (!WaitOwnFence(INFINITE))
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
		// UI is stamped after dispatch. A HUDLess image equal to the scene extracts nothing.
		config.HUDLessColor = {};
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
		// A stitched center passes the eye size. SBS keeps half the guide width: 1.0 is one eye.
		prepare.motionVectorScale.x = mvecScaleX > 0.0f ? mvecScaleX : static_cast<float>(inGuideW) * 0.5f;
		prepare.motionVectorScale.y = mvecScaleY > 0.0f ? mvecScaleY : static_cast<float>(inGuideH);
		static bool loggedScale = false;
		if (!loggedScale && (inColorW != inGuideW || inColorH != inGuideH)) {
			loggedScale = true;
			logger::info("[VrFfxOp] FG render {}x{} display {}x{} mvecScale=({:.0f},{:.0f})",
				inGuideW, inGuideH, inColorW, inColorH,
				prepare.motionVectorScale.x, prepare.motionVectorScale.y);
		}
		prepare.jitterOffset.x = latchedView.valid ? latchedView.jitterX : -upscaling.jitter.x;
		prepare.jitterOffset.y = latchedView.valid ? latchedView.jitterY : -upscaling.jitter.y;
		prepare.frameTimeDelta = latchedView.valid ? latchedView.frameMs : 11.11f;
		prepare.cameraNear = latchedView.valid ? latchedView.nearZ : *globals::game::cameraNear;
		prepare.cameraFar = latchedView.valid ? latchedView.farZ : *globals::game::cameraFar;
		prepare.cameraFovAngleVertical = latchedView.valid ? latchedView.fov : Util::GetVerticalFOVRad();
		prepare.viewSpaceToMetersFactor = 0.01428222656f;
		prepare.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMMON);
		prepare.motionVectors = ffxApiGetResourceDX12(mvec, FFX_API_RESOURCE_STATE_COMMON);

		ffx::DispatchDescFrameGenerationPrepareCameraInfo cameraConfig{};
		if (latchedView.valid) {
			cameraConfig.cameraRight[0] = latchedView.right[0];
			cameraConfig.cameraRight[1] = latchedView.right[1];
			cameraConfig.cameraRight[2] = latchedView.right[2];
			cameraConfig.cameraUp[0] = latchedView.up[0];
			cameraConfig.cameraUp[1] = latchedView.up[1];
			cameraConfig.cameraUp[2] = latchedView.up[2];
			cameraConfig.cameraForward[0] = latchedView.fwd[0];
			cameraConfig.cameraForward[1] = latchedView.fwd[1];
			cameraConfig.cameraForward[2] = latchedView.fwd[2];
			cameraConfig.cameraPosition[0] = latchedView.pos[0];
			cameraConfig.cameraPosition[1] = latchedView.pos[1];
			cameraConfig.cameraPosition[2] = latchedView.pos[2];
		} else {
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
		}

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
		HRESULT signalHr = S_OK;
		const UINT64 signaled = fenceValue + 1;
		VrSharedDevice::WithQueue([&] {
			queue->ExecuteCommandLists(1, lists);
			signalHr = queue->Signal(fence.Get(), signaled);
		});
		if (FAILED(signalHr))
			return false;
		fenceValue = signaled;
		++frameID;
		float fsrMs = 0.0f;
		auto waitFsr = [&]() {
			LARGE_INTEGER freq{};
			LARGE_INTEGER start{};
			LARGE_INTEGER end{};
			QueryPerformanceFrequency(&freq);
			QueryPerformanceCounter(&start);
			const bool done = WaitOwnFence(INFINITE);
			QueryPerformanceCounter(&end);
			if (freq.QuadPart)
				fsrMs += static_cast<float>(static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart));
			return done;
		};
		if (!waitFsr())
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
		signalHr = S_OK;
		const UINT64 signaledCommon = fenceValue + 1;
		VrSharedDevice::WithQueue([&] {
			queue->ExecuteCommandLists(1, toCommonLists);
			signalHr = queue->Signal(fence.Get(), signaledCommon);
		});
		if (FAILED(signalHr))
			return false;
		fenceValue = signaledCommon;
		if (!waitFsr())
			return false;
		fsrEma = fsrEma > 0.0f ? fsrEma * 0.8f + fsrMs * 0.2f : fsrMs;
		VrPassTimer::ReportSpan("VrDb::Fsr", fsrMs);
		// Folds into the same T_rdb the pace gate reads; FG's own cost was invisible to it before.
		VrPassTimer::AddRdbGpuMs(fsrMs);
		return true;
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

void VrFfxOp::NotePairedHud(ID3D12Resource* hud)
{
	pairedHud = hud;
}

void VrFfxOp::NoteLatchedView(const LatchedView& view)
{
	latchedView.right[0] = view.right[0];
	latchedView.right[1] = view.right[1];
	latchedView.right[2] = view.right[2];
	latchedView.up[0] = view.up[0];
	latchedView.up[1] = view.up[1];
	latchedView.up[2] = view.up[2];
	latchedView.fwd[0] = view.fwd[0];
	latchedView.fwd[1] = view.fwd[1];
	latchedView.fwd[2] = view.fwd[2];
	latchedView.pos[0] = view.pos[0];
	latchedView.pos[1] = view.pos[1];
	latchedView.pos[2] = view.pos[2];
	latchedView.jitterX = view.jitterX;
	latchedView.jitterY = view.jitterY;
	latchedView.nearZ = view.nearZ;
	latchedView.farZ = view.farZ;
	latchedView.fov = view.fov;
	latchedView.frameMs = std::isfinite(view.frameMs) ? std::clamp(view.frameMs, 1.0f, 100.0f) : 11.11f;
	latchedView.valid = true;
}

void VrFfxOp::NoteDiscontinuity(bool resetFg)
{
	std::lock_guard lock(opMutex);
	if (resetFg) {
		const bool already = fgResetPending.exchange(true, std::memory_order_acq_rel);
		if (!already || prevKick.valid)
			logger::info("[VrFfxOp] discontinuity: FG reset, prevKick cleared");
	}
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
				// kMENUBG's alpha runs the opposite of the usual "0 =
				// transparent" convention on this engine: with h.a used
				// directly, the headset showed solid black everywhere
				// *except* where UI was on screen, with the scene showing
				// through *there* instead -- the mirror image of a correct
				// composite (a bug probe separately showed most of kMENUBG
				// sampling back near-opaque even off any drawn UI). 1-h.a
				// matches the engine's own kMENUBG-onto-kTOTAL composite.
				"  float a = saturate(1.0 - h.a);\n"
				"  c.rgb = lerp(c.rgb, h.rgb, a);\n"
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
			// Zero-init leaves this at 0 for *both* ranges, which is a
			// literal offset (not D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND) --
			// the root signature then believes t0 and u0 both live at
			// table offset 0, even though the CPU descriptor writes below
			// put the UAV one slot later. The compute shader's UAV write
			// (meant for composite/dst) resolved against that wrong offset
			// and landed back on the SRV's own resource (hudCopy) instead,
			// which is why it came back solid black after StampOne despite
			// only ever being bound read-only.
			ranges[0].OffsetInDescriptorsFromTableStart = 0;
			ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			ranges[1].NumDescriptors = 1;
			ranges[1].OffsetInDescriptorsFromTableStart = 1;
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
		if (!WaitOwnFence(INFINITE)) {
			static bool loggedOwn = false;
			if (!loggedOwn) {
				loggedOwn = true;
				logger::warn("[VrFfxOp] HUD stamp skipped: own fence not ready");
			}
			return false;
		}
		if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
			return false;
		D3D12_RESOURCE_BARRIER toUav{};
		toUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toUav.Transition.pResource = dst;
		toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		toUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		toUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		list->ResourceBarrier(1, &toUav);
		D3D12_RESOURCE_BARRIER hudToRead{};
		hudToRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		hudToRead.Transition.pResource = hud;
		hudToRead.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		hudToRead.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		hudToRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		list->ResourceBarrier(1, &hudToRead);
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
		D3D12_RESOURCE_BARRIER hudToCommon = hudToRead;
		hudToCommon.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		hudToCommon.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		list->ResourceBarrier(1, &hudToCommon);
		D3D12_RESOURCE_BARRIER toCommon = toUav;
		toCommon.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		toCommon.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
		list->ResourceBarrier(1, &toCommon);
		if (FAILED(list->Close()))
			return false;
		ID3D12CommandList* lists[]{ list.Get() };
		HRESULT signalHr = S_OK;
		const UINT64 signaled = fenceValue + 1;
		VrSharedDevice::WithQueue([&] {
			queue->ExecuteCommandLists(1, lists);
			signalHr = queue->Signal(fence.Get(), signaled);
		});
		if (FAILED(signalHr))
			return false;
		fenceValue = signaled;
		if (!WaitOwnFence(INFINITE))
			return false;
		return true;
	}

namespace
{
	VrPose::Body heldPose{};
	bool heldPoseValid = false;
}

bool VrFfxOp::OnDbFinalColor(ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* mvec,
	UINT inColorW, UINT inColorH, UINT inGuideW, UINT inGuideH,
	DXGI_FORMAT inColorFmt, DXGI_FORMAT inDepthFmt, DXGI_FORMAT inMvecFmt,
	float mvecScaleX, float mvecScaleY, bool submit, ID3D12Resource** interpOut)
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
		dispatchOk = DispatchUnlocked(color, depth, mvec, inColorW, inColorH, inGuideW, inGuideH, mvecScaleX, mvecScaleY);
	QueryPerformanceCounter(&end);
	++dispatches;
	const double ms = freq.QuadPart ?
		(static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)) :
		0.0;
	if (dispatches <= 8 || (dispatches % 120) == 0)
		logger::info("[VrFfxOp] dispatch {:.2f} ms ok={}", ms, dispatchOk ? 1 : 0);

	if (!submit) {
		heldPose = cur;
		heldPoseValid = dispatchOk && cur.valid;
		if (interpOut)
			*interpOut = dispatchOk ? interp.Get() : nullptr;
		return dispatchOk;
	}

	// Composite the HUD onto what's actually about to be submitted. FSR's
	// own dispatch already ran on the HUD-free color/depth/mvec above
	// (interp was generated from pure scene motion; baking the HUD in
	// before that would have handed FSR's motion estimation static UI
	// pixels to warp, ghosting the HUD in the generated frame), so this
	// must run after DispatchUnlocked and before either buffer leaves this
	// function. pairedHud is read-only here (SRV bind inside StampOne), so
	// it is safe to read the Doorbell-owned keepHud directly.
	if (pairedHud) {
		const D3D12_RESOURCE_DESC hudDesc = pairedHud->GetDesc();
		const UINT hudW = static_cast<UINT>(hudDesc.Width);
		const UINT hudH = hudDesc.Height;
		LARGE_INTEGER hudFreq{};
		LARGE_INTEGER hudStart{};
		LARGE_INTEGER hudEnd{};
		QueryPerformanceFrequency(&hudFreq);
		QueryPerformanceCounter(&hudStart);
		if (dispatchOk && cur.valid)
			StampOne(interp.Get(), pairedHud, inColorW, inColorH, hudW, hudH);
		StampOne(color, pairedHud, inColorW, inColorH, hudW, hudH);
		QueryPerformanceCounter(&hudEnd);
		if (hudFreq.QuadPart)
			VrPassTimer::ReportSpan("VrDb::Hud", static_cast<float>(
				static_cast<double>(hudEnd.QuadPart - hudStart.QuadPart) * 1000.0 / static_cast<double>(hudFreq.QuadPart)));
	}

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
		logger::info("[VrFfxOp] pace dispatch={:.2f} ms color={}x{} td={} nr={} q={} engine={:.1f} ms want={} consumed={}",
			ms, inColorW, inColorH, fullTd ? 1 : 0,
			fov.settings.neuralRenderingEnabled ? 1 : 0, fov.GetQualityModeAtBoot(),
			engineMs, VrAriseTail::LastPublishFence(), VrAriseTail::LastConsumedFence());
	}
	return copied;
}

bool VrFfxOp::PresentComposited(ID3D12Resource* real, ID3D12Resource* generated, UINT width, UINT height, DXGI_FORMAT format)
{
	if (!real || !generated || !width || !height)
		return false;
	std::lock_guard lock(opMutex);
	const VrPose::Body cur = heldPose;
	if (pairedHud) {
		const D3D12_RESOURCE_DESC hudDesc = pairedHud->GetDesc();
		const UINT hudW = static_cast<UINT>(hudDesc.Width);
		const UINT hudH = hudDesc.Height;
		if (heldPoseValid)
			StampOne(generated, pairedHud, width, height, hudW, hudH);
		StampOne(real, pairedHud, width, height, hudW, hudH);
	}
	const std::uint64_t pair = VrBeat::NextPair();
	if (heldPoseValid) {
		const vr::HmdMatrix34_t mid = prevKick.valid ? InterpolatePose(prevKick.m, cur.m, 0.5f) : cur.m;
		VrBeat::OfferGInterpolated(generated, width, height, format, mid, pair);
	}
	const bool copied = VrBeat::CopySbs(real, width, height, format, cur.m, cur.valid, pair);
	if (!copied)
		VrBeat::CancelInterp();
	if (copied && cur.valid)
		prevKick = cur;
	heldPoseValid = false;
	return copied;
}

void VrFfxOp::Shutdown()
{
	std::lock_guard lock(opMutex);
	ShutdownUnlocked();
}

#endif
