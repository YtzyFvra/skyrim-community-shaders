#include "PCH.h"
#include "Metronome.h"

#ifdef ENABLE_SKYRIM_VR

#	include "../../../GpuPass.h"
#	include "../../../Utils/WinApi.h"
#	include "../VrAriseTail.h"
#	include "../VrPassTimer.h"
#	include "../VrSharedDevice.h"

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
	std::atomic<bool> firstSubmit{ false };
	std::atomic<DWORD> beatThreadId{ 0 };

	std::mutex mailboxMutex;
	std::mutex statusMutex;
	std::atomic<bool> ocPeriodOpen{ false };
	std::mutex periodMutex;
	std::condition_variable periodCv;
	std::condition_variable mailCv;
	std::mutex firstSubmitMutex;
	std::condition_variable firstSubmitCv;

	VrBeat::Frame mailbox{};
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
	std::atomic<std::uint64_t> mailSerial{ 0 };
	std::atomic<std::uint64_t> mailDone{ 0 };
	std::mutex mailDoneMutex;
	std::condition_variable mailDoneCv;
	std::atomic<uint64_t> pairSeq{ 0 };
	std::atomic<uint64_t> interpPair{ 0 };
	std::mutex interpPoseMutex;
	vr::HmdMatrix34_t interpPose{};

	struct PendingSpan
	{
		std::int64_t start = 0;
		std::uint64_t serial = 0;
		std::uint64_t publishFence = 0;
		bool armed = false;
		bool releaseAfterSubmit = false;
	};
	PendingSpan pendingSpan;

	using SetEnqueueKindFn = void (*)(int);
	using SetEnqueuePairFn = void (*)(std::uint64_t);
	SetEnqueueKindFn ResolveSetEnqueueKind()
	{
		static SetEnqueueKindFn fn = []() -> SetEnqueueKindFn {
			HMODULE module = GetModuleHandleW(L"openvr_api.dll");
			if (!module)
				module = GetModuleHandleW(L"vrclient_x64.dll");
			if (!module)
				return nullptr;
			return reinterpret_cast<SetEnqueueKindFn>(GetProcAddress(module, "OC_L1_SetNextEnqueueKind"));
		}();
		return fn;
	}

	void SetOcEnqueueKind(int kind)
	{
		if (auto fn = ResolveSetEnqueueKind())
			fn(kind);
	}

	SetEnqueuePairFn ResolveSetEnqueuePair()
	{
		static SetEnqueuePairFn fn = []() -> SetEnqueuePairFn {
			HMODULE module = GetModuleHandleW(L"openvr_api.dll");
			if (!module)
				module = GetModuleHandleW(L"vrclient_x64.dll");
			if (!module)
				return nullptr;
			return reinterpret_cast<SetEnqueuePairFn>(GetProcAddress(module, "OC_L1_SetNextEnqueuePair"));
		}();
		return fn;
	}

	void SetOcEnqueuePair(std::uint64_t pair)
	{
		if (auto fn = ResolveSetEnqueuePair())
			fn(pair);
	}

	using InterpAllowedFn = int (*)();
	InterpAllowedFn ResolveInterpAllowed()
	{
		static InterpAllowedFn fn = []() -> InterpAllowedFn {
			HMODULE module = GetModuleHandleW(L"openvr_api.dll");
			if (!module)
				module = GetModuleHandleW(L"vrclient_x64.dll");
			if (!module)
				return nullptr;
			return reinterpret_cast<InterpAllowedFn>(GetProcAddress(module, "OC_L1_InterpAllowed"));
		}();
		return fn;
	}

	bool OcInterpAllowed()
	{
		if (auto fn = ResolveInterpAllowed())
			return fn() != 0;
		return true;
	}

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
		const bool done = WaitForSingleObject(copyEvent, 1000) == WAIT_OBJECT_0;
		if (!done)
			logger::warn("[VrBeat] copy fence timeout");
		return done;
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

	void LogEngineCallRateIfDue(double gapMs)
	{
		const std::uint32_t w = engineWaitCount.load(std::memory_order_relaxed);
		const std::uint32_t g = engineGetLastCount.load(std::memory_order_relaxed);
		const std::uint32_t total = w + g;
		static double accMs = 0;
		static std::uint32_t accN = 0;
		if (gapMs > 0) {
			accMs += gapMs;
			++accN;
		}
		if (total <= 8 || (total % 120) == 0) {
			const double avg = accN ? accMs / static_cast<double>(accN) : 0;
			logger::info("[VrBeat] engine pose calls: WaitGetPoses={} GetLastPoses={} avgWait={:.2f} ms", w, g, avg);
			accMs = 0;
			accN = 0;
		}
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
			const auto now = std::chrono::steady_clock::now();
			static std::chrono::steady_clock::time_point lastWait{};
			static bool haveLast = false;
			double gapMs = 0;
			if (haveLast)
				gapMs = std::chrono::duration<double, std::milli>(now - lastWait).count();
			lastWait = now;
			haveLast = true;
			LogEngineCallRateIfDue(gapMs);
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
			LogEngineCallRateIfDue(0);
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

	bool SubmitFrame(vr::IVRCompositor* openvr, const VrBeat::Frame& frame,
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
		vr::HmdMatrix34_t pose = frame.pose;
		const bool poseValid = frame.poseValid;
		if (!frame.queue12 || !frame.left12 || !frame.right12)
			return false;
		if (!poseValid) {
			static std::atomic<uint32_t> skipped{ 0 };
			const auto n = ++skipped;
			if (n <= 8 || (n % 120) == 0)
				logger::warn("[VrBeat] skip Submit: no declared pose n={}", n);
			return false;
		}
		auto submitOne = [&](vr::EVREye eyeIndex, ID3D12Resource* resource,
							 const vr::VRTextureBounds_t& bounds, vr::EVRCompositorError& err) {
			vr::D3D12TextureData_t dx12{};
			dx12.m_pResource = resource;
			dx12.m_pCommandQueue = frame.queue12;
			vr::VRTextureWithPose_t tex{};
			tex.handle = &dx12;
			tex.eType = vr::TextureType_DirectX12;
			tex.eColorSpace = vr::ColorSpace_Auto;
			tex.mDeviceToAbsoluteTracking = pose;
			VrSharedDevice::WithQueue([&] {
				err = openvr->Submit(eyeIndex, &tex, &bounds, vr::Submit_TextureWithPose);
			});
		};
		if (frame.sbs) {
			submitOne(vr::Eye_Left, frame.left12, leftSbs, errL);
			submitOne(vr::Eye_Right, frame.right12, rightSbs, errR);
		} else {
			submitOne(vr::Eye_Left, frame.left12, full, errL);
			submitOne(vr::Eye_Right, frame.right12, full, errR);
		}
		return true;
	}

	void WaitMailWritable()
	{
		const std::uint64_t published = mailSerial.load(std::memory_order_acquire);
		if (mailDone.load(std::memory_order_acquire) >= published)
			return;
		std::unique_lock lock(mailDoneMutex);
		mailDoneCv.wait(lock, [&] {
			return mailDone.load(std::memory_order_acquire) >= published;
		});
	}

	void FinishMail()
	{
		mailDone.store(mailSerial.load(std::memory_order_acquire), std::memory_order_release);
		mailDoneCv.notify_all();
	}

	bool CopyGpuInterp(ID3D12Resource* src, UINT width, UINT height, DXGI_FORMAT format)
	{
		if (!src || !gpuQueue || !EnsureCopyList() || !EnsureInterpSbs(width, height, format))
			return false;
		WaitMailWritable();
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
		++copyFenceValue;
		const UINT64 signaled = copyFenceValue;
		VrSharedDevice::WithQueue([&] {
			gpuQueue->ExecuteCommandLists(1, lists);
			gpuQueue->Signal(copyFence.Get(), signaled);
		});
		return WaitCopyDone();
	}

	void SubmitInterpFrame(vr::IVRCompositor* openvr, const vr::HmdMatrix34_t& pose,
		vr::EVRCompositorError& errL, vr::EVRCompositorError& errR)
	{
		// No CS_GPU_PASS: see the identical note in SubmitFrame.
		const vr::VRTextureBounds_t leftSbs{ 0.f, 0.f, 0.5f, 1.f };
		const vr::VRTextureBounds_t rightSbs{ 0.5f, 0.f, 1.f, 1.f };
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
			VrSharedDevice::WithQueue([&] {
				err = openvr->Submit(eyeIndex, &tex, &bounds, vr::Submit_TextureWithPose);
			});
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

		bool loggedFirstSubmit = false;
		bool loggedTwP = false;
		VrBeat::Status local{};
		local.running = true;
		LARGE_INTEGER perfFreq{};
		QueryPerformanceFrequency(&perfFreq);

		while (running.load(std::memory_order_acquire)) {
			auto* openvr = RE::BSOpenVR::GetIVRCompositor();
			if (!openvr) {
				Sleep(100);
				PublishStatus(local);
				continue;
			}
			HookOpenVrPosesImpl(openvr);

			// L3 owns Wait. This thread only enqueues; interp before real.
			VrBeat::Frame snap{};
			bool haveNew = false;
			{
				std::unique_lock lock(mailboxMutex);
				mailCv.wait_for(lock, std::chrono::milliseconds(5), [] {
					return haveGpuMail.load(std::memory_order_acquire) ||
						!running.load(std::memory_order_acquire);
				});
				haveNew = haveGpuMail.exchange(false, std::memory_order_acq_rel);
				snap = mailbox;
			}
			if (!running.load(std::memory_order_acquire))
				break;
			if (!haveNew) {
				PublishStatus(local);
				continue;
			}
			const bool haveTex = snap.queue12 && snap.left12 && snap.right12;
			if (!haveTex) {
				FinishMail();
				PublishStatus(local);
				continue;
			}

			const bool hadInterp = snap.interpPending && snap.poseValid;
			vr::EVRCompositorError errL = vr::VRCompositorError_None;
			vr::EVRCompositorError errR = vr::VRCompositorError_None;
			vr::EVRCompositorError interpL = vr::VRCompositorError_None;
			vr::EVRCompositorError interpR = vr::VRCompositorError_None;
			LARGE_INTEGER submitStart{};
			LARGE_INTEGER submitEnd{};
			QueryPerformanceCounter(&submitStart);
			if (hadInterp && interpSbs && OcInterpAllowed()) {
				SetOcEnqueuePair(snap.pair);
				SetOcEnqueueKind(1);
				SubmitInterpFrame(openvr, snap.interpPose, interpL, interpR);
			}
			SetOcEnqueuePair(snap.pair);
			SetOcEnqueueKind(0);
			const bool submitted = SubmitFrame(openvr, snap, errL, errR);
			QueryPerformanceCounter(&submitEnd);
			FinishMail();
			if (snap.spanArmed) {
				VrPassTimer::BindReportSerial(snap.spanSerial);
				VrPassTimer::ReportWallClock(snap.releaseAfterSubmit ? "VrSingle::Total" : "VrArise::Total", snap.spanStart, false);
			}
			if (snap.releaseAfterSubmit && snap.publishFence)
				VrAriseTail::ReleaseEngine(snap.publishFence);
			const bool usedTwP = submitted;
			local.lastSubmitL = static_cast<int>(errL);
			local.lastSubmitR = static_cast<int>(errR);
			if (submitted && errL == vr::VRCompositorError_None && errR == vr::VRCompositorError_None)
				++local.submits;
			if (submitted && !loggedFirstSubmit) {
				const double submitMs = perfFreq.QuadPart ?
					(static_cast<double>(submitEnd.QuadPart - submitStart.QuadPart) * 1000.0 /
						static_cast<double>(perfFreq.QuadPart)) :
					0.0;
				logger::info("[VrBeat] L1 enqueue first Submit errL={} ({}) errR={} ({}) blocked={:.0f} ms TwP={}",
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
			PublishStatus(local);
		}

		local.running = false;
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
		if (!src || !gpuQueue || !EnsureCopyList())
			return false;
		// A caller that still holds the first publish size must not crop a larger keep.
		const auto srcDesc = src->GetDesc();
		const UINT copyW = static_cast<UINT>(srcDesc.Width);
		const UINT copyH = srcDesc.Height;
		if (!copyW || !copyH || !EnsureMailSbs(copyW, copyH, format))
			return false;
		if (copyW != width || copyH != height) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				logger::error("[VrBeat] CopySbs caller {}x{} != source {}x{}; submitting source",
					width, height, copyW, copyH);
			}
		}
		if (!WaitCopyDone())
			return false;
		WaitMailWritable();
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
		++copyFenceValue;
		const UINT64 signaled = copyFenceValue;
		VrSharedDevice::WithQueue([&] {
			gpuQueue->ExecuteCommandLists(1, lists);
			gpuQueue->Signal(copyFence.Get(), signaled);
		});
		return true;
	}

}

void VrBeat::Start()
{
	bool expected = false;
	if (!running.compare_exchange_strong(expected, true))
		return;
	firstSubmit.store(false, std::memory_order_release);
	logger::info("[VrBeat] metronome started");
	worker = std::thread(BeatLoop);
}

void VrBeat::Stop()
{
	if (!running.exchange(false, std::memory_order_acq_rel)) {
		firstSubmitCv.notify_all();
		return;
	}
	firstSubmitCv.notify_all();
	periodCv.notify_all();
	mailCv.notify_all();
	if (worker.joinable())
		worker.join();
	logger::info("[VrBeat] metronome stopped");
}

bool VrBeat::HasFirstSubmit()
{
	return firstSubmit.load(std::memory_order_acquire);
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
	copy.firstSubmit = firstSubmit.load(std::memory_order_acquire);
	return copy;
}

void VrBeat::BindGpu(ID3D12Device* device, ID3D12CommandQueue* queue)
{
	gpuDevice = device;
	gpuQueue = queue;
}

std::uint64_t VrBeat::NextPair()
{
	return pairSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void VrBeat::NoteSpan(std::int64_t startQpc, std::uint64_t serial, std::uint64_t publishFence, bool releaseAfterSubmit)
{
	pendingSpan.start = startQpc;
	pendingSpan.serial = serial;
	pendingSpan.publishFence = publishFence;
	pendingSpan.armed = startQpc > 0;
	pendingSpan.releaseAfterSubmit = releaseAfterSubmit;
}

void VrBeat::ClearSpan()
{
	pendingSpan = {};
}

bool VrBeat::CopySbs(ID3D12Resource* src, UINT width, UINT height, DXGI_FORMAT format,
	const vr::HmdMatrix34_t& pose, bool poseValid, std::uint64_t pair)
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
		frame.pair = pair;
		frame.pose = pose;
		frame.poseValid = poseValid;
		{
			std::lock_guard lockInterp(interpPoseMutex);
			frame.interpPending = gInterpPending.load(std::memory_order_acquire) &&
				interpPair.load(std::memory_order_acquire) == pair;
			frame.interpPose = interpPose;
		}
		frame.spanStart = pendingSpan.start;
		frame.spanSerial = pendingSpan.serial;
		frame.publishFence = pendingSpan.publishFence;
		frame.spanArmed = pendingSpan.armed;
		frame.releaseAfterSubmit = pendingSpan.releaseAfterSubmit;
		pendingSpan = {};
		mailbox = frame;
	}
	mailSerial.fetch_add(1, std::memory_order_acq_rel);
	haveGpuMail.store(true, std::memory_order_release);
	mailCv.notify_all();
	return true;
}

bool VrBeat::HasGpuMail()
{
	return haveGpuMail.load(std::memory_order_acquire);
}

bool VrBeat::OfferGInterpolated(ID3D12Resource* src, UINT width, UINT height, DXGI_FORMAT format,
	const vr::HmdMatrix34_t& pose, std::uint64_t pair)
{
	if (!CopyGpuInterp(src, width, height, format))
		return false;
	{
		std::lock_guard lock(interpPoseMutex);
		interpPose = pose;
	}
	interpPair.store(pair, std::memory_order_release);
	gInterpPending.store(true, std::memory_order_release);
	periodCv.notify_all();
	mailCv.notify_all();
	return true;
}

void VrBeat::CancelInterp()
{
	gInterpPending.store(false, std::memory_order_release);
}

#endif
