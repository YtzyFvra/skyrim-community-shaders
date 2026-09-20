#include "PCH.h"
#include "PresentGate.h"

#ifdef ENABLE_SKYRIM_VR

#	include "../../../Globals.h"
#	include "../../../GpuPass.h"
#	include "../../Upscaling.h"
#	include "../VrCount/PreAfter.h"
#	include "../VrGWrap/GPresent.h"
#	include "../VrPose/Absolute.h"
#	include "../VrSwitch/Master.h"

#	include <atomic>
#	include <mutex>

#	include <Windows.h>
#	include <dxgi.h>
#	include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
	std::atomic<bool> ready{ false };
	std::atomic<bool> tagged{ false };
	std::atomic<bool> lastEnableG{ false };

	ID3D12Device* device = nullptr;
	ID3D12CommandQueue* queue = nullptr;
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12Fence> fence;
	HANDLE fenceEvent = nullptr;
	UINT64 fenceValue = 0;

	ComPtr<ID3D12Resource> readyColor;
	ComPtr<ID3D12Resource> readyDepth;
	ComPtr<ID3D12Resource> readyMvec;
	ComPtr<ID3D12Resource> readyUi;
	sl::Constants readyConstants{};
	sl::FrameToken* token = nullptr;
	uint32_t tokenId = 0;

	UINT colorW = 0;
	UINT colorH = 0;
	UINT guideW = 0;
	UINT guideH = 0;
	DXGI_FORMAT colorFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
	DXGI_FORMAT depthFmt = DXGI_FORMAT_R32_FLOAT;
	DXGI_FORMAT mvecFmt = DXGI_FORMAT_UNKNOWN;
	bool haveUi = false;

	std::mutex gateMutex;

	void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
		D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		if (!cmd || !resource || before == after)
			return;
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = resource;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = after;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cmd->ResourceBarrier(1, &barrier);
	}

	bool WaitGpu()
	{
		if (!fence || fenceValue == 0)
			return true;
		if (fence->GetCompletedValue() >= fenceValue)
			return true;
		if (!fenceEvent || FAILED(fence->SetEventOnCompletion(fenceValue, fenceEvent)))
			return false;
		return WaitForSingleObject(fenceEvent, INFINITE) == WAIT_OBJECT_0;
	}

	bool CreateTex(ComPtr<ID3D12Resource>& out, UINT w, UINT h, DXGI_FORMAT fmt, const wchar_t* name)
	{
		out.Reset();
		if (!device || !w || !h)
			return false;
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = w;
		desc.Height = h;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = fmt;
		desc.SampleDesc.Count = 1;
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out))))
			return false;
		out->SetName(name);
		return true;
	}

	bool EnsureReady(UINT cW, UINT cH, UINT gW, UINT gH,
		DXGI_FORMAT cFmt, DXGI_FORMAT dFmt, DXGI_FORMAT mFmt, bool wantUi)
	{
		const bool same = readyColor && colorW == cW && colorH == cH && colorFmt == cFmt &&
			readyDepth && guideW == gW && guideH == gH && depthFmt == dFmt &&
			readyMvec && mvecFmt == mFmt && haveUi == wantUi;
		if (same)
			return true;
		if (!CreateTex(readyColor, cW, cH, cFmt, L"VrSimGate::ReadyColor") ||
			!CreateTex(readyDepth, gW, gH, dFmt, L"VrSimGate::ReadyDepth") ||
			!CreateTex(readyMvec, gW, gH, mFmt, L"VrSimGate::ReadyMvec"))
			return false;
		if (wantUi) {
			if (!CreateTex(readyUi, cW, cH, cFmt, L"VrSimGate::ReadyUi"))
				return false;
		} else
			readyUi.Reset();
		colorW = cW;
		colorH = cH;
		guideW = gW;
		guideH = gH;
		colorFmt = cFmt;
		depthFmt = dFmt;
		mvecFmt = mFmt;
		haveUi = wantUi;
		return true;
	}

	bool EnsureList()
	{
		if (list)
			return true;
		if (!device)
			return false;
		if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
			FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
				IID_PPV_ARGS(&list))) ||
			FAILED(list->Close()) ||
			FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
			return false;
		fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		return fenceEvent != nullptr;
	}

	bool EnsureToken()
	{
		if (token)
			return true;
		auto& sl = globals::features::upscaling.streamlineDX12;
		if (!sl.AcquireFrameToken(++tokenId, token) || !token) {
			token = nullptr;
			return false;
		}
		if (sl.slReflexSleep)
			sl.slReflexSleep(*token);
		return true;
	}

	bool BlitReadyToBackbuffer()
	{
		auto* sc = VrGWrap::SwapChain();
		if (!sc || !readyColor || !queue || !EnsureList() || !WaitGpu())
			return false;
		const UINT index = sc->GetCurrentBackBufferIndex();
		ID3D12Resource* bb = VrGWrap::BackBuffer(index);
		if (!bb)
			return false;
		if (FAILED(alloc->Reset()) || FAILED(list->Reset(alloc.Get(), nullptr)))
			return false;
		Transition(list.Get(), readyColor.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Transition(list.Get(), bb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		list->CopyResource(bb, readyColor.Get());
		Transition(list.Get(), bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		Transition(list.Get(), readyColor.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
		if (FAILED(list->Close()))
			return false;
		ID3D12CommandList* lists[]{ list.Get() };
		queue->ExecuteCommandLists(1, lists);
		++fenceValue;
		queue->Signal(fence.Get(), fenceValue);
		return WaitGpu();
	}

	bool PresentNow()
	{
		if (!VrSwitch::On() || !tagged.load(std::memory_order_acquire) || !token)
			return false;
		// No CS_GPU_PASS: Profiler's internal vectors have no lock and are
		// written every frame by the render thread's own instrumentation.
		// This fires off the Doorbell worker thread; a concurrent push/
		// reallocate there crashed inside std::vector<Profiler::TimerResult>.
		auto& sl = globals::features::upscaling.streamlineDX12;
		const sl::ViewportHandle vp = sl.viewport;
		sl.ConfigureDLSSG(vp, lastEnableG.load(std::memory_order_acquire), guideW, guideH, colorW, colorH,
			depthFmt, mvecFmt, colorFmt, DXGI_FORMAT_UNKNOWN, 3);
		if (!BlitReadyToBackbuffer())
			return false;
		auto* sc = VrGWrap::SwapChain();
		if (!sc)
			return false;
		// Diagnostic only (feeds X4 G kick= P=/A=) -- pose binding by P index
		// is gone, see GPresent.cpp::OnPresent for why.
		if (lastEnableG.load(std::memory_order_acquire))
			VrCount::AddPre();
		// Roll this Kick's real pose into VrGWrap's prev/cur pair and reset
		// its per-Kick native-fire counter *before* Present -- OnPresent's
		// hook needs both already in place whenever it ends up firing.
		VrGWrap::BeginKickPresent(VrPoseAbs::Ready());
		if (sl.featurePCL)
			sl.EmitPCLMarker(sl::PCLMarker::ePresentStart, *token);
		const HRESULT hr = sc->Present(0, DXGI_PRESENT_ALLOW_TEARING);
		if (sl.featurePCL)
			sl.EmitPCLMarker(sl::PCLMarker::ePresentEnd, *token);
		if (sl.slDLSSGGetState) {
			sl::DLSSGState state{};
			if (sl.slDLSSGGetState(sl.viewport, state, nullptr) == sl::Result::eOk) {
				sl.lastDLSSGStatus = state.status;
				sl.lastDLSSGFramesPresented = state.numFramesActuallyPresented;
			}
		}
		if (FAILED(hr)) {
			logger::warn("[VrSimGate] Present failed hr={:08X}", static_cast<unsigned>(hr));
			return false;
		}
		token = nullptr;
		tagged.store(false, std::memory_order_release);
		return true;
	}
}

bool VrSimGate::Ensure(ID3D12Device* inDevice, ID3D12CommandQueue* inQueue,
	UINT inColorW, UINT inColorH, UINT inGuideW, UINT inGuideH,
	DXGI_FORMAT inColorFmt, DXGI_FORMAT inDepthFmt, DXGI_FORMAT inMvecFmt)
{
	std::lock_guard lock(gateMutex);
	device = inDevice;
	queue = inQueue;
	if (!EnsureList())
		return false;
	if (inColorW && inColorH && inGuideW && inGuideH &&
		!EnsureReady(inColorW, inColorH, inGuideW, inGuideH, inColorFmt, inDepthFmt, inMvecFmt, false))
		return false;
	ready.store(true, std::memory_order_release);
	logger::info("[VrSimGate] ready set color={}x{} guide={}x{}",
		inColorW, inColorH, inGuideW, inGuideH);
	return true;
}

bool VrSimGate::OnDbFinalColor(const DbFrame& frame)
{
	std::lock_guard lock(gateMutex);
	if (!VrSwitch::On())
		return false;
	if (!device || !queue || !frame.color || !frame.depth || !frame.mvec)
		return false;
	if (!EnsureList() || !WaitGpu())
		return false;
	if (!EnsureReady(frame.colorW, frame.colorH, frame.guideW, frame.guideH,
			frame.colorFmt, frame.depthFmt, frame.mvecFmt, frame.ui != nullptr))
		return false;
	if (!EnsureToken()) {
		logger::error("[VrSimGate] AcquireFrameToken failed");
		return false;
	}
	readyConstants = frame.constants;
	lastEnableG.store(frame.enableG, std::memory_order_release);

	auto& sl = globals::features::upscaling.streamlineDX12;
	const sl::ViewportHandle vp = sl.viewport;
	if (sl.featurePCL)
		sl.EmitPCLMarker(sl::PCLMarker::eSimulationStart, *token);
	if (!sl.SetConstants(*token, readyConstants, vp)) {
		logger::error("[VrSimGate] SetConstants failed");
		return false;
	}
	if (FAILED(alloc->Reset()) || FAILED(list->Reset(alloc.Get(), nullptr)))
		return false;
	VrPoseAbs::StampReady();

	auto copyOne = [&](ID3D12Resource* src, ID3D12Resource* dst) {
		Transition(list.Get(), src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Transition(list.Get(), dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		list->CopyResource(dst, src);
		Transition(list.Get(), dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		Transition(list.Get(), src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
	};
	copyOne(frame.color, readyColor.Get());
	copyOne(frame.depth, readyDepth.Get());
	copyOne(frame.mvec, readyMvec.Get());
	if (frame.ui && readyUi)
		copyOne(frame.ui, readyUi.Get());

	if (!sl.TagDX12Resources(*token, list.Get(), readyDepth.Get(), readyMvec.Get(), readyColor.Get(),
			haveUi ? readyUi.Get() : nullptr, guideW, guideH, colorW, colorH, vp)) {
		list->Close();
		logger::error("[VrSimGate] tag ready set failed");
		return false;
	}
	if (sl.featurePCL) {
		sl.EmitPCLMarker(sl::PCLMarker::eSimulationEnd, *token);
		sl.EmitPCLMarker(sl::PCLMarker::eRenderSubmitStart, *token);
	}
	if (FAILED(list->Close()))
		return false;
	ID3D12CommandList* lists[]{ list.Get() };
	queue->ExecuteCommandLists(1, lists);
	++fenceValue;
	queue->Signal(fence.Get(), fenceValue);
	if (!WaitGpu())
		return false;
	if (sl.featurePCL)
		sl.EmitPCLMarker(sl::PCLMarker::eRenderSubmitEnd, *token);
	tagged.store(true, std::memory_order_release);
	return PresentNow();
}

void VrSimGate::Shutdown()
{
	std::lock_guard lock(gateMutex);
	ready.store(false, std::memory_order_release);
	tagged.store(false, std::memory_order_release);
	token = nullptr;
	readyColor.Reset();
	readyDepth.Reset();
	readyMvec.Reset();
	readyUi.Reset();
	list.Reset();
	alloc.Reset();
	fence.Reset();
	if (fenceEvent) {
		CloseHandle(fenceEvent);
		fenceEvent = nullptr;
	}
	device = nullptr;
	queue = nullptr;
}

#endif
