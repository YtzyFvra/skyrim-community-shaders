#include "PCH.h"
#include "Metronome.h"

#ifdef ENABLE_SKYRIM_VR

#	include "../../../GpuPass.h"
#	include "../../../Utils/WinApi.h"

#	include <atomic>
#	include <chrono>
#	include <condition_variable>
#	include <cstring>
#	include <exception>
#	include <mutex>
#	include <thread>

#	include <Windows.h>
#	include <d3d12.h>
#	include <wrl/client.h>

#	include "RE/B/BSOpenVR.h"

namespace
{
	std::atomic<bool> running{ false };
	std::atomic<bool> ownsWait{ false };
	std::atomic<bool> firstSubmit{ false };
	std::atomic<DWORD> beatThreadId{ 0 };

	std::mutex waitGate;
	std::mutex mailboxMutex;
	std::mutex statusMutex;
	std::atomic<bool> ocPeriodOpen{ false };
	std::mutex periodMutex;
	std::condition_variable periodCv;
	std::mutex firstSubmitMutex;
	std::condition_variable firstSubmitCv;

	VrBeat::Frame mailbox{};
	std::atomic<bool> haveMail{ false };
	std::atomic<bool> haveGpuMail{ false };
	std::mutex submitPoseMutex;
	vr::HmdMatrix34_t submitPose{};
	bool submitPoseValid = false;

	using Microsoft::WRL::ComPtr;
	ID3D12Device* gpuDevice = nullptr;
	ID3D12CommandQueue* gpuQueue = nullptr;
	ComPtr<ID3D12CommandAllocator> copyAlloc;
	ComPtr<ID3D12GraphicsCommandList> copyList;
	ComPtr<ID3D12Fence> copyFence;
	UINT64 copyFenceValue = 0;
	ComPtr<ID3D12Resource> mailSbs;
	HANDLE copyEvent = nullptr;
	UINT mailW = 0;
	UINT mailH = 0;
	DXGI_FORMAT mailFmt = DXGI_FORMAT_UNKNOWN;

	// G's interpolated frame: a second, dedicated buffer/pose so it survives
	// long enough to get its own beat-loop iteration instead of being
	// overwritten by the real frame that lands in mailSbs moments later on
	// the same Kick. See OfferGInterpolated's header comment.
	ComPtr<ID3D12Resource> interpSbs;
	UINT interpW = 0;
	UINT interpH = 0;
	DXGI_FORMAT interpFmt = DXGI_FORMAT_UNKNOWN;
	std::atomic<bool> gInterpPending{ false };
	std::mutex interpPoseMutex;
	vr::HmdMatrix34_t interpPose{};

	void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
		D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		if (!list || !resource || before == after)
			return;
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = resource;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = after;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		list->ResourceBarrier(1, &barrier);
	}

	bool EnsureCopyList()
	{
		if (!gpuDevice)
			return false;
		if (copyList)
			return true;
		if (FAILED(gpuDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copyAlloc))) ||
			FAILED(gpuDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copyAlloc.Get(), nullptr,
				IID_PPV_ARGS(&copyList))) ||
			FAILED(copyList->Close()) ||
			FAILED(gpuDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence))))
			return false;
		if (!copyEvent)
			copyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		return copyEvent != nullptr;
	}

	bool WaitCopyDone()
	{
		if (!copyFence || copyFenceValue == 0)
			return true;
		if (copyFence->GetCompletedValue() >= copyFenceValue)
			return true;
		if (!copyEvent || FAILED(copyFence->SetEventOnCompletion(copyFenceValue, copyEvent)))
			return false;
		return WaitForSingleObject(copyEvent, INFINITE) == WAIT_OBJECT_0;
	}

	bool EnsureMailSbs(UINT w, UINT h, DXGI_FORMAT fmt)
	{
		if (!gpuDevice || !w || !h)
			return false;
		if (mailSbs && mailW == w && mailH == h && mailFmt == fmt)
			return true;
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
		mailSbs.Reset();
		if (FAILED(gpuDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mailSbs))))
			return false;
		mailSbs->SetName(L"VrBeat::Buffer");
		mailW = w;
		mailH = h;
		mailFmt = fmt;
		return true;
	}

	bool EnsureInterpSbs(UINT w, UINT h, DXGI_FORMAT fmt)
	{
		if (!gpuDevice || !w || !h)
			return false;
		if (interpSbs && interpW == w && interpH == h && interpFmt == fmt)
			return true;
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
		interpSbs.Reset();
		if (FAILED(gpuDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&interpSbs))))
			return false;
		interpSbs->SetName(L"VrBeat::InterpBuffer");
		interpW = w;
		interpH = h;
		interpFmt = fmt;
		return true;
	}

	VrBeat::Status status{};
	std::thread worker;

	using CompositorPosesFn = vr::EVRCompositorError (*)(vr::IVRCompositor*, vr::TrackedDevicePose_t*, uint32_t,
		vr::TrackedDevicePose_t*, uint32_t);
	CompositorPosesFn origWaitGetPoses = nullptr;
	CompositorPosesFn origGetLastPoses = nullptr;
	void* hookedCompositorVtbl = nullptr;
	// HookOpenVrPosesImpl is called from both the engine thread (every
	// UpdatePoses) and the beat thread (every BeatLoop). Without this, a
	// racing install on the same vtable can capture our own hook as "orig",
	// making the thunk call itself forever.
	std::mutex hookInstallMutex;

	std::mutex enginePoseMutex;
	vr::HmdMatrix34_t engineHmdPose{};
	bool engineHmdPoseValid = false;

	const char* CompositorErrorName(vr::EVRCompositorError err)
	{
		switch (err) {
		case vr::VRCompositorError_None:
			return "None";
		case vr::VRCompositorError_InvalidTexture:
			return "InvalidTexture";
		case vr::VRCompositorError_IsNotSceneApplication:
			return "IsNotSceneApplication";
		case vr::VRCompositorError_TextureUsesUnsupportedFormat:
			return "TextureUsesUnsupportedFormat";
		case vr::VRCompositorError_AlreadySubmitted:
			return "AlreadySubmitted";
		default:
			return "Other";
		}
	}

	bool IsBeatThread()
	{
		const DWORD id = beatThreadId.load(std::memory_order_acquire);
		return id != 0 && GetCurrentThreadId() == id;
	}

	/** @brief Copy the HMD pose out of a render-pose array the engine just received. */
	void SnoopEnginePose(const vr::TrackedDevicePose_t* render, uint32_t count)
	{
		if (!render || count <= vr::k_unTrackedDeviceIndex_Hmd)
			return;
		const auto& hmd = render[vr::k_unTrackedDeviceIndex_Hmd];
		std::lock_guard lock(enginePoseMutex);
		engineHmdPose = hmd.mDeviceToAbsoluteTracking;
		engineHmdPoseValid = hmd.bPoseIsValid;
	}

	/**
	 * Passthrough: always call the original. Off the beat thread this is the
	 * engine's own call, so a successful Wait just opened a compositor period
	 * (GetLastPoses never opens one) and its render pose is what the engine
	 * drew this frame with — snoop it, do not replace it.
	 */
	// Diagnostic: names which call the engine is actually driving with, and
	// at what rate, so a menu-open judder session can be checked for whether
	// the engine silently stopped calling the real Wait (UpdatePoses(true))
	// and switched to GetLastPoses (UpdatePoses(false)) -- that call opens no
	// compositor period, so BeatLoop would fall back to its own heartbeat
	// Wait for the whole time the engine does this, not just a brief idle gap.
	std::atomic<std::uint32_t> engineWaitCount{ 0 };
	std::atomic<std::uint32_t> engineGetLastCount{ 0 };

	void LogEngineCallRateIfDue()
	{
		const std::uint32_t w = engineWaitCount.load(std::memory_order_relaxed);
		const std::uint32_t g = engineGetLastCount.load(std::memory_order_relaxed);
		const std::uint32_t total = w + g;
		if (total <= 8 || (total % 120) == 0)
			logger::info("[VrBeat] engine pose calls: WaitGetPoses={} GetLastPoses={}", w, g);
	}

	vr::EVRCompositorError WaitGetPosesThunk(vr::IVRCompositor* self, vr::TrackedDevicePose_t* render, uint32_t renderCount,
		vr::TrackedDevicePose_t* game, uint32_t gameCount)
	{
		const auto err = origWaitGetPoses(self, render, renderCount, game, gameCount);
		if (!IsBeatThread() && err == vr::VRCompositorError_None) {
			SnoopEnginePose(render, renderCount);
			ocPeriodOpen.store(true, std::memory_order_release);
			periodCv.notify_all();
			engineWaitCount.fetch_add(1, std::memory_order_relaxed);
			LogEngineCallRateIfDue();
		}
		return err;
	}

	vr::EVRCompositorError GetLastPosesThunk(vr::IVRCompositor* self, vr::TrackedDevicePose_t* render, uint32_t renderCount,
		vr::TrackedDevicePose_t* game, uint32_t gameCount)
	{
		const auto err = origGetLastPoses(self, render, renderCount, game, gameCount);
		if (!IsBeatThread() && err == vr::VRCompositorError_None) {
			SnoopEnginePose(render, renderCount);
			engineGetLastCount.fetch_add(1, std::memory_order_relaxed);
			LogEngineCallRateIfDue();
		}
		return err;
	}

	bool SwapCompositorVFunc(void** vtbl, std::size_t index, void* hook, void** orig)
	{
		DWORD oldProtect = 0;
		if (!VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
			return false;
		*orig = vtbl[index];
		vtbl[index] = hook;
		VirtualProtect(&vtbl[index], sizeof(void*), oldProtect, &oldProtect);
		return *orig != nullptr;
	}

	void HookOpenVrPosesImpl(vr::IVRCompositor* openvr)
	{
		if (!openvr)
			return;
		void** vtbl = *reinterpret_cast<void***>(openvr);
		if (!vtbl || vtbl == hookedCompositorVtbl)
			return;
		std::lock_guard installLock(hookInstallMutex);
		if (vtbl == hookedCompositorVtbl)
			return;
		if (!SwapCompositorVFunc(vtbl, 2, reinterpret_cast<void*>(&WaitGetPosesThunk),
				reinterpret_cast<void**>(&origWaitGetPoses)) ||
			!SwapCompositorVFunc(vtbl, 3, reinterpret_cast<void*>(&GetLastPosesThunk),
				reinterpret_cast<void**>(&origGetLastPoses))) {
			logger::warn("[VrBeat] pose snoop hook failed");
			return;
		}
		hookedCompositorVtbl = vtbl;
		logger::info("[VrBeat] WaitGetPoses/GetLastPoses snoop hooked (passthrough)");
	}

	void PublishStatus(const VrBeat::Status& local)
	{
		std::lock_guard lock(statusMutex);
		status = local;
	}

	void SignalFirstSubmit()
	{
		if (firstSubmit.exchange(true, std::memory_order_acq_rel))
			return;
		firstSubmitCv.notify_all();
	}

	VrBeat::Frame SnapshotMail()
	{
		std::lock_guard lock(mailboxMutex);
		return mailbox;
	}

	bool WaitOnce(vr::IVRCompositor* openvr, VrBeat::Status& local, bool& loggedWaitOk,
		vr::HmdMatrix34_t* outHmdPose = nullptr, bool* outHmdPoseValid = nullptr)
	{
		vr::TrackedDevicePose_t renderPoses[vr::k_unMaxTrackedDeviceCount]{};
		vr::TrackedDevicePose_t gamePoses[vr::k_unMaxTrackedDeviceCount]{};
		LARGE_INTEGER freq{};
		LARGE_INTEGER start{};
		LARGE_INTEGER end{};
		QueryPerformanceFrequency(&freq);
		vr::EVRCompositorError waitErr = vr::VRCompositorError_None;
		{
			std::lock_guard lock(waitGate);
			ownsWait.store(true, std::memory_order_release);
			QueryPerformanceCounter(&start);
			waitErr = openvr->WaitGetPoses(renderPoses, vr::k_unMaxTrackedDeviceCount,
				gamePoses, vr::k_unMaxTrackedDeviceCount);
			QueryPerformanceCounter(&end);
			if (waitErr == vr::VRCompositorError_None)
				ocPeriodOpen.store(true, std::memory_order_release);
		}
		Util::DisableConsoleQuickEdit();
		++local.waits;
		const double waitMs = freq.QuadPart ?
			(static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)) :
			0.0;
		local.lastWaitMs = waitMs;
		local.lastWaitErr = static_cast<int>(waitErr);
		local.ownsWait = true;
		if (waitErr != vr::VRCompositorError_None) {
			logger::error("[VrBeat] WaitGetPoses err={}", static_cast<int>(waitErr));
			return false;
		}
		if (outHmdPose && outHmdPoseValid) {
			*outHmdPose = renderPoses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
			*outHmdPoseValid = renderPoses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid;
		}
		if (!loggedWaitOk) {
			logger::info("[VrBeat] WaitGetPoses ok");
			loggedWaitOk = true;
		}
		if (local.waits <= 8 || (local.waits % 120) == 0)
			logger::info("[VrBeat] WaitGetPoses {:.2f} ms err={} n={}",
				waitMs, static_cast<int>(waitErr), local.waits);
		return true;
	}

	void SubmitFrame(vr::IVRCompositor* openvr, const VrBeat::Frame& frame,
		vr::EVRCompositorError& errL, vr::EVRCompositorError& errR)
	{
		// No CS_GPU_PASS here: Profiler's internal vectors (knownTimers,
		// results, activeStack) have no lock and are written every frame by
		// the render thread's own pass instrumentation. This runs on the
		// beat thread — a concurrent push/reallocate crashed CommunityShaders.dll
		// inside std::vector<Profiler::TimerResult> (EXCEPTION_ACCESS_VIOLATION).
		const vr::VRTextureBounds_t full{ 0.f, 0.f, 1.f, 1.f };
		const vr::VRTextureBounds_t leftSbs{ 0.f, 0.f, 0.5f, 1.f };
		const vr::VRTextureBounds_t rightSbs{ 0.5f, 0.f, 1.f, 1.f };
		vr::HmdMatrix34_t pose{};
		bool poseValid = false;
		{
			std::lock_guard lock(submitPoseMutex);
			pose = submitPose;
			poseValid = submitPoseValid;
		}
		if (!frame.queue12 || !frame.left12 || !frame.right12)
			return;
		const bool withPose = poseValid;
		auto submitOne = [&](vr::EVREye eyeIndex, ID3D12Resource* resource,
							 const vr::VRTextureBounds_t& bounds, vr::EVRCompositorError& err) {
			vr::D3D12TextureData_t dx12{};
			dx12.m_pResource = resource;
			dx12.m_pCommandQueue = frame.queue12;
			if (withPose) {
				vr::VRTextureWithPose_t tex{};
				tex.handle = &dx12;
				tex.eType = vr::TextureType_DirectX12;
				tex.eColorSpace = vr::ColorSpace_Auto;
				tex.mDeviceToAbsoluteTracking = pose;
				err = openvr->Submit(eyeIndex, &tex, &bounds, vr::Submit_TextureWithPose);
			} else {
				vr::Texture_t tex{};
				tex.handle = &dx12;
				tex.eType = vr::TextureType_DirectX12;
				tex.eColorSpace = vr::ColorSpace_Auto;
				err = openvr->Submit(eyeIndex, &tex, &bounds, vr::Submit_Default);
			}
		};
		if (frame.sbs) {
			submitOne(vr::Eye_Left, frame.left12, leftSbs, errL);
			submitOne(vr::Eye_Right, frame.right12, rightSbs, errR);
		} else {
			submitOne(vr::Eye_Left, frame.left12, full, errL);
			submitOne(vr::Eye_Right, frame.right12, full, errR);
		}
	}

	bool CopyGpuInterp(ID3D12Resource* src, UINT width, UINT height, DXGI_FORMAT format)
	{
		if (!src || !gpuQueue || !EnsureCopyList() || !EnsureInterpSbs(width, height, format))
			return false;
		if (!WaitCopyDone())
			return false;
		// Same copyList/copyAlloc/copyFence as CopyGpu -- calls here and
		// there never overlap (both only ever run from wherever G's native
		// Present hook fires, one call per native Present, never
		// concurrently with itself).
		if (FAILED(copyAlloc->Reset()) || FAILED(copyList->Reset(copyAlloc.Get(), nullptr)))
			return false;
		Transition(copyList.Get(), src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Transition(copyList.Get(), interpSbs.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		copyList->CopyResource(interpSbs.Get(), src);
		Transition(copyList.Get(), interpSbs.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		Transition(copyList.Get(), src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
		if (FAILED(copyList->Close()))
			return false;
		ID3D12CommandList* lists[]{ copyList.Get() };
		gpuQueue->ExecuteCommandLists(1, lists);
		++copyFenceValue;
		gpuQueue->Signal(copyFence.Get(), copyFenceValue);
		return WaitCopyDone();
	}

	void SubmitInterpFrame(vr::IVRCompositor* openvr, vr::EVRCompositorError& errL, vr::EVRCompositorError& errR)
	{
		// No CS_GPU_PASS: see the identical note in SubmitFrame.
		const vr::VRTextureBounds_t leftSbs{ 0.f, 0.f, 0.5f, 1.f };
		const vr::VRTextureBounds_t rightSbs{ 0.5f, 0.f, 1.f, 1.f };
		vr::HmdMatrix34_t pose{};
		{
			std::lock_guard lock(interpPoseMutex);
			pose = interpPose;
		}
		if (!gpuQueue || !interpSbs)
			return;
		auto submitOne = [&](vr::EVREye eyeIndex, const vr::VRTextureBounds_t& bounds, vr::EVRCompositorError& err) {
			vr::D3D12TextureData_t dx12{};
			dx12.m_pResource = interpSbs.Get();
			dx12.m_pCommandQueue = gpuQueue;
			vr::VRTextureWithPose_t tex{};
			tex.handle = &dx12;
			tex.eType = vr::TextureType_DirectX12;
			tex.eColorSpace = vr::ColorSpace_Auto;
			tex.mDeviceToAbsoluteTracking = pose;
			err = openvr->Submit(eyeIndex, &tex, &bounds, vr::Submit_TextureWithPose);
		};
		submitOne(vr::Eye_Left, leftSbs, errL);
		submitOne(vr::Eye_Right, rightSbs, errR);
	}

	/**
	 * Diagnostic-only: the crash on 09-19 left no crash-*.log and no WER dump
	 * despite Crash Logger being installed, and the Windows Event Log showed
	 * 0xc0000409 (fail-fast, not a normal AV) — a class of crash consistent
	 * with an uncaught exception reaching std::terminate on this thread. This
	 * cannot make that safe to continue from; it only logs what() first so
	 * the next occurrence names the actual exception instead of guessing.
	 */
	void BeatLoopImpl()
	{
		SetThreadDescription(GetCurrentThread(), L"VrBeat");
		timeBeginPeriod(1);
		beatThreadId.store(GetCurrentThreadId(), std::memory_order_release);

		bool loggedWaitOk = false;
		bool loggedFirstSubmit = false;
		bool loggedTwP = false;
		VrBeat::Status local{};
		local.running = true;

		// EXPERIMENT (09-20 01:13): the engine no longer calls the real
		// blocking WaitGetPoses at all (UpdatePosesHook routes it through
		// GetLastPoses instead -- documented safe for a second thread,
		// non-blocking, opens no period). This thread is now the process's
		// *sole* real-Wait caller, so it Waits continuously, back-to-back,
		// as fast as the runtime actually returns -- no more waiting for a
		// signal from the engine first. This answers two open questions
		// live: (1) can this exceed the ~72Hz ceiling every measurement so
		// far assumed was hard, (2) do consecutive Waits actually return
		// distinct poses or does the runtime hand back a repeat until its
		// own real supply tick (revealing the true supply granularity,
		// independent of whatever Hz VD claims as a target).
		LARGE_INTEGER perfFreq{};
		QueryPerformanceFrequency(&perfFreq);
		LARGE_INTEGER lastWaitReturn{};
		bool haveLastWaitReturn = false;
		vr::HmdMatrix34_t lastLoggedPose{};
		bool haveLastLoggedPose = false;
		uint64_t repeatRun = 0;

		while (running.load(std::memory_order_acquire)) {
			auto* openvr = RE::BSOpenVR::GetIVRCompositor();
			if (!openvr) {
				Sleep(100);
				PublishStatus(local);
				continue;
			}
			HookOpenVrPosesImpl(openvr);

			vr::HmdMatrix34_t curPose{};
			bool curPoseValid = false;
			if (!WaitOnce(openvr, local, loggedWaitOk, &curPose, &curPoseValid)) {
				Sleep(1);
				PublishStatus(local);
				continue;
			}

			// Experiment logging: gap since the last Wait *returned* (not
			// since it was called -- this is "how far apart were two
			// consecutive real supply ticks", the number task 4 needs) and
			// whether the pose actually changed (a run of identical poses
			// across Waits that measure ~13.9ms apart would mean the
			// runtime is still only handing out ~72Hz-granularity data even
			// though we're asking faster).
			{
				LARGE_INTEGER now{};
				QueryPerformanceCounter(&now);
				double gapMs = 0.0;
				if (haveLastWaitReturn && perfFreq.QuadPart)
					gapMs = static_cast<double>(now.QuadPart - lastWaitReturn.QuadPart) * 1000.0 /
						static_cast<double>(perfFreq.QuadPart);
				lastWaitReturn = now;
				haveLastWaitReturn = true;
				bool samePose = curPoseValid && haveLastLoggedPose &&
					std::memcmp(&curPose, &lastLoggedPose, sizeof(curPose)) == 0;
				if (samePose)
					++repeatRun;
				else
					repeatRun = 0;
				if (curPoseValid) {
					lastLoggedPose = curPose;
					haveLastLoggedPose = true;
				}
				if (local.waits <= 40 || (local.waits % 30) == 0)
					logger::info("[VrBeat] continuous Wait n={} gap={:.2f} ms same={} repeatRun={} pos=({:.4f},{:.4f},{:.4f})",
						local.waits, gapMs, samePose ? 1 : 0, repeatRun,
						curPose.m[0][3], curPose.m[1][3], curPose.m[2][3]);
			}

			// One Submit per period: prefer a pending G interpolated frame
			// (it arrived first per SL's "generated then real" order and is
			// the one that actually tests whether an extra period beyond
			// the real frame's own is obtainable at all) over the real
			// mailbox; the real frame gets first shot at the *next*
			// iteration's period instead of being dropped -- it never
			// leaves the mailbox until a newer one overwrites it.
			std::lock_guard periodLock(waitGate);
			if (!ocPeriodOpen.load(std::memory_order_acquire)) {
				PublishStatus(local);
				continue;
			}
			if (gInterpPending.exchange(false, std::memory_order_acq_rel)) {
				vr::EVRCompositorError errL = vr::VRCompositorError_None;
				vr::EVRCompositorError errR = vr::VRCompositorError_None;
				SubmitInterpFrame(openvr, errL, errR);
				ocPeriodOpen.store(false, std::memory_order_release);
				static uint64_t interpSubmits = 0;
				++interpSubmits;
				if (interpSubmits <= 8 || (interpSubmits % 120) == 0)
					logger::info("[VrBeat] G interp Submit #{} errL={} ({}) errR={} ({})",
						interpSubmits, static_cast<int>(errL), CompositorErrorName(errL),
						static_cast<int>(errR), CompositorErrorName(errR));
				PublishStatus(local);
				continue;
			}

			const VrBeat::Frame snap = SnapshotMail();
			const bool haveTex = snap.queue12 && snap.left12 && snap.right12;
			if (haveTex) {
				vr::EVRCompositorError errL = vr::VRCompositorError_None;
				vr::EVRCompositorError errR = vr::VRCompositorError_None;
				LARGE_INTEGER submitStart{};
				LARGE_INTEGER submitEnd{};
				QueryPerformanceCounter(&submitStart);
				SubmitFrame(openvr, snap, errL, errR);
				ocPeriodOpen.store(false, std::memory_order_release);
				QueryPerformanceCounter(&submitEnd);
				bool usedTwP = false;
				{
					std::lock_guard poseLock(submitPoseMutex);
					usedTwP = submitPoseValid;
				}
				local.lastSubmitL = static_cast<int>(errL);
				local.lastSubmitR = static_cast<int>(errR);
				if (errL == vr::VRCompositorError_None && errR == vr::VRCompositorError_None)
					++local.submits;
				if (!loggedFirstSubmit) {
					const double submitMs = perfFreq.QuadPart ?
						(static_cast<double>(submitEnd.QuadPart - submitStart.QuadPart) * 1000.0 /
							static_cast<double>(perfFreq.QuadPart)) :
						0.0;
					logger::info("[VrBeat] first Submit errL={} ({}) errR={} ({}) blocked={:.0f} ms TwP={}",
						static_cast<int>(errL), CompositorErrorName(errL),
						static_cast<int>(errR), CompositorErrorName(errR),
						submitMs, usedTwP ? 1 : 0);
					loggedFirstSubmit = true;
					SignalFirstSubmit();
					local.firstSubmit = true;
				}
				if (usedTwP && !loggedTwP) {
					logger::info("[VrBeat] Submit_TextureWithPose errL={} ({}) errR={} ({})",
						static_cast<int>(errL), CompositorErrorName(errL),
						static_cast<int>(errR), CompositorErrorName(errR));
					loggedTwP = true;
				}
			}
			// else: nothing ready yet (e.g. still loading) -- leave the
			// period open; the next Wait below simply starts a new one on
			// top, same as before this experiment (no queue to leak).

			if (local.waits <= 8 || (local.waits % 120) == 0)
				logger::info("[VrBeat] beat wait={:.2f} ms n={} submits={}",
					local.lastWaitMs, local.waits, local.submits);
			PublishStatus(local);
		}

		{
			std::lock_guard lock(waitGate);
			ownsWait.store(false, std::memory_order_release);
		}
		local.running = false;
		local.ownsWait = false;
		PublishStatus(local);
		timeEndPeriod(1);
	}

	void BeatLoop()
	{
		try {
			BeatLoopImpl();
		} catch (const std::exception& e) {
			logger::critical("[VrBeat] BeatLoop uncaught exception: {}", e.what());
			throw;
		} catch (...) {
			logger::critical("[VrBeat] BeatLoop uncaught non-standard exception");
			throw;
		}
	}

	bool CopyGpu(ID3D12Resource* src, UINT width, UINT height, DXGI_FORMAT format)
	{
		if (!src || !gpuQueue || !EnsureCopyList() || !EnsureMailSbs(width, height, format))
			return false;
		if (!WaitCopyDone())
			return false;
		// No CS_GPU_PASS: see the identical note in SubmitFrame above. This
		// runs on the Doorbell worker thread, not the render thread.
		if (FAILED(copyAlloc->Reset()) || FAILED(copyList->Reset(copyAlloc.Get(), nullptr)))
			return false;
		Transition(copyList.Get(), src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Transition(copyList.Get(), mailSbs.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		copyList->CopyResource(mailSbs.Get(), src);
		Transition(copyList.Get(), mailSbs.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		Transition(copyList.Get(), src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
		if (FAILED(copyList->Close()))
			return false;
		ID3D12CommandList* lists[]{ copyList.Get() };
		gpuQueue->ExecuteCommandLists(1, lists);
		++copyFenceValue;
		gpuQueue->Signal(copyFence.Get(), copyFenceValue);
		return true;
	}

	void PublishMailTex()
	{
		VrBeat::Frame frame{};
		std::lock_guard lockMail(mailboxMutex);
		frame = mailbox;
		frame.left12 = mailSbs.Get();
		frame.right12 = mailSbs.Get();
		frame.queue12 = gpuQueue;
		frame.sbs = true;
		mailbox = frame;
		haveMail.store(true, std::memory_order_release);
		haveGpuMail.store(true, std::memory_order_release);
	}
}

void VrBeat::Start()
{
	bool expected = false;
	if (!running.compare_exchange_strong(expected, true))
		return;
	firstSubmit.store(false, std::memory_order_release);
	ownsWait.store(false, std::memory_order_release);
	logger::info("[VrBeat] metronome started");
	worker = std::thread(BeatLoop);
}

void VrBeat::Stop()
{
	if (!running.exchange(false, std::memory_order_acq_rel)) {
		firstSubmitCv.notify_all();
		return;
	}
	{
		std::lock_guard lock(waitGate);
		ownsWait.store(false, std::memory_order_release);
	}
	firstSubmitCv.notify_all();
	periodCv.notify_all();
	if (worker.joinable())
		worker.join();
	logger::info("[VrBeat] metronome stopped");
}

void VrBeat::Publish(const Frame& frame)
{
	std::lock_guard lock(mailboxMutex);
	mailbox = frame;
	haveMail.store(true, std::memory_order_release);
}

bool VrBeat::HasFirstSubmit()
{
	return firstSubmit.load(std::memory_order_acquire);
}

bool VrBeat::OwnsWait()
{
	return ownsWait.load(std::memory_order_acquire);
}

void VrBeat::HookCompositorPoses()
{
	HookOpenVrPosesImpl(RE::BSOpenVR::GetIVRCompositor());
}

bool VrBeat::CopyEngineDrawPose(vr::HmdMatrix34_t& out)
{
	std::lock_guard lock(enginePoseMutex);
	if (!engineHmdPoseValid)
		return false;
	out = engineHmdPose;
	return true;
}

VrBeat::EngineWaitClose::EngineWaitClose()
{
	// Submit must never run on the engine thread (that killed a build before).
	// The beat thread closes any leftover period on its own thread; wake on
	// its notify instead of polling, so a healthy beat thread costs ~0 here.
	{
		std::unique_lock lock(periodMutex);
		periodCv.wait_for(lock, std::chrono::milliseconds(20), [] {
			return !ocPeriodOpen.load(std::memory_order_acquire);
		});
	}
	waitGate.lock();
	static bool logged = false;
	if (!logged) {
		logger::info("[VrBeat] Engine UpdatePoses(true) WaitGetPoses allowed");
		logged = true;
	}
}

VrBeat::EngineWaitClose::~EngineWaitClose()
{
	// ocPeriodOpen is already set by the WaitGetPoses snoop hook if the
	// engine's own Wait just succeeded; this only releases the exclusion.
	waitGate.unlock();
}

bool VrBeat::WaitForFirstSubmitMs(unsigned ms)
{
	std::unique_lock lock(firstSubmitMutex);
	return firstSubmitCv.wait_for(lock, std::chrono::milliseconds(ms), [] {
		return firstSubmit.load(std::memory_order_acquire);
	});
}

VrBeat::Status VrBeat::GetStatus()
{
	std::lock_guard lock(statusMutex);
	auto copy = status;
	copy.running = running.load(std::memory_order_acquire);
	copy.ownsWait = ownsWait.load(std::memory_order_acquire);
	copy.firstSubmit = firstSubmit.load(std::memory_order_acquire);
	return copy;
}

void VrBeat::BindGpu(ID3D12Device* device, ID3D12CommandQueue* queue)
{
	gpuDevice = device;
	gpuQueue = queue;
}

bool VrBeat::CopySbs(ID3D12Resource* src, UINT width, UINT height, DXGI_FORMAT format)
{
	if (!CopyGpu(src, width, height, format))
		return false;
	PublishMailTex();
	return true;
}

bool VrBeat::CopySbs(ID3D12Resource* src, UINT width, UINT height, DXGI_FORMAT format,
	const vr::HmdMatrix34_t& pose, bool poseValid)
{
	if (!CopyGpu(src, width, height, format))
		return false;
	{
		std::lock_guard lockMail(mailboxMutex);
		Frame frame = mailbox;
		frame.left12 = mailSbs.Get();
		frame.right12 = mailSbs.Get();
		frame.queue12 = gpuQueue;
		frame.sbs = true;
		mailbox = frame;
		haveMail.store(true, std::memory_order_release);
		std::lock_guard lockPose(submitPoseMutex);
		submitPose = pose;
		submitPoseValid = poseValid;
	}
	haveGpuMail.store(true, std::memory_order_release);
	return true;
}

bool VrBeat::HasGpuMail()
{
	return haveGpuMail.load(std::memory_order_acquire);
}

bool VrBeat::OfferGInterpolated(ID3D12Resource* src, UINT width, UINT height, DXGI_FORMAT format,
	const vr::HmdMatrix34_t& pose)
{
	if (!CopyGpuInterp(src, width, height, format))
		return false;
	{
		std::lock_guard lock(interpPoseMutex);
		interpPose = pose;
	}
	{
		std::lock_guard lock(periodMutex);
		gInterpPending.store(true, std::memory_order_release);
	}
	periodCv.notify_all();
	return true;
}

#endif
