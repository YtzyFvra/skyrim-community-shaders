#include "PCH.h"
#include "VrAriseTail.h"

#include "../../Globals.h"
#include "../../State.h"
#include "../../Utils/D3D.h"
#include "../../Utils/WinApi.h"
#include "../Upscaling.h"
#include "GpuPass.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#ifdef ENABLE_SKYRIM_VR
#	include "RE/B/BSOpenVR.h"
#	include "VrFrameGen/VrPoseRing.h"
#endif

using Microsoft::WRL::ComPtr;

namespace
{
	std::atomic<bool> installed{ false };
	std::atomic<bool> running{ false };
	std::atomic<bool> active{ false };
	std::atomic<bool> vrReady{ false };
	std::atomic<bool> tailOwnsLoop{ false };
	std::atomic<bool> abandoned{ false };
	std::atomic<bool> firstOwnedSubmit{ false };
	std::mutex vrLoop;
	std::mutex firstSubmitMutex;
	std::condition_variable firstSubmitCv;
	std::thread worker;

	std::mutex statusMutex;
	VrAriseTail::Status status{};

	constexpr UINT kSharedSlots = 2;
	constexpr double kHmdHz = 72.0;
	constexpr double kAppHz = 36.0;
	constexpr DWORD kFenceWaitMs = 16;
	// 09-15 23:20: one SBS chain on Queue12. Extra queues + pair assembly are what TDR'd.
	constexpr bool kFrameGenerationDoorbell = true;

	bool WaitFenceCpu(ID3D12Fence* fence, HANDLE event, UINT64 value, DWORD ms)
	{
		if (!fence || value == 0)
			return true;
		if (fence->GetCompletedValue() >= value)
			return true;
		if (!event)
			return false;
		if (FAILED(fence->SetEventOnCompletion(value, event)))
			return false;
		return WaitForSingleObject(event, ms) == WAIT_OBJECT_0;
	}

	void PaceUntil(LARGE_INTEGER start, double hz)
	{
		LARGE_INTEGER freq{};
		QueryPerformanceFrequency(&freq);
		if (!freq.QuadPart || hz <= 0.0)
			return;
		const double period = static_cast<double>(freq.QuadPart) / hz;
		for (;;) {
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			const double elapsed = static_cast<double>(now.QuadPart - start.QuadPart);
			if (elapsed >= period)
				return;
			const double remainMs = (period - elapsed) * 1000.0 / static_cast<double>(freq.QuadPart);
			if (remainMs > 2.0)
				Sleep(1);
			else
				SwitchToThread();
		}
	}

	struct SharedPublish
	{
		UINT64 fenceValue = 0;
		UINT32 slot = 0;
		UINT32 generation = 0;
		UINT32 width = 0;
		UINT32 height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		UINT32 guideWidth = 0;
		UINT32 guideHeight = 0;
		DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT mvecFormat = DXGI_FORMAT_UNKNOWN;
		bool hasGuides = false;
	};

	std::mutex sharedMutex;
	SharedPublish sharedPublished{};
	ComPtr<ID3D11Texture2D> sharedTex11[kSharedSlots];
	HANDLE sharedTexHandle[kSharedSlots]{};
	ComPtr<ID3D11Texture2D> sharedDepth11[kSharedSlots];
	HANDLE sharedDepthHandle[kSharedSlots]{};
	ComPtr<ID3D11RenderTargetView> sharedDepthRtv[kSharedSlots];
	ComPtr<ID3D11Texture2D> sharedMvec11[kSharedSlots];
	HANDLE sharedMvecHandle[kSharedSlots]{};
	ComPtr<ID3D11Fence> sharedFence11;
	HANDLE sharedFenceHandle = nullptr;
	UINT64 sharedFenceValue = 0;
	UINT32 sharedGeneration = 0;
	UINT32 sharedWidth = 0;
	UINT32 sharedHeight = 0;
	DXGI_FORMAT sharedFormat = DXGI_FORMAT_UNKNOWN;
	UINT32 sharedGuideWidth = 0;
	UINT32 sharedGuideHeight = 0;
	DXGI_FORMAT sharedMvecFormat = DXGI_FORMAT_UNKNOWN;
	bool sharedHasGuides = false;
	sl::Constants sharedConstants[2]{};
	bool sharedConstantsValid = false;
	std::atomic<UINT64> publishedThisFrame{ UINT64_MAX };
	std::atomic<bool> loggedPublish{ false };
	std::atomic<bool> loggedPublishFail{ false };
	std::atomic<bool> publishPausedForLoad{ false };
	std::atomic<LONGLONG> publishResumeQpc{ 0 };
	std::atomic<bool> loggedGuides{ false };

	bool CreateSharedTex(ID3D11Device5* device5, UINT width, UINT height, DXGI_FORMAT format, UINT bindFlags,
		ComPtr<ID3D11Texture2D>& tex, HANDLE& handle, const char* name)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = bindFlags;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		if (FAILED(device5->CreateTexture2D(&desc, nullptr, &tex)))
			return false;
		Util::SetResourceName(tex.Get(), name);
		ComPtr<IDXGIResource1> dxgi;
		if (FAILED(tex.As(&dxgi)))
			return false;
		return SUCCEEDED(dxgi->CreateSharedHandle(nullptr,
			DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle));
	}

	void CloseSharedHandles()
	{
		for (UINT i = 0; i < kSharedSlots; ++i) {
			if (sharedTexHandle[i]) {
				CloseHandle(sharedTexHandle[i]);
				sharedTexHandle[i] = nullptr;
			}
			sharedTex11[i].Reset();
			if (sharedDepthHandle[i]) {
				CloseHandle(sharedDepthHandle[i]);
				sharedDepthHandle[i] = nullptr;
			}
			sharedDepthRtv[i].Reset();
			sharedDepth11[i].Reset();
			if (sharedMvecHandle[i]) {
				CloseHandle(sharedMvecHandle[i]);
				sharedMvecHandle[i] = nullptr;
			}
			sharedMvec11[i].Reset();
		}
		if (sharedFenceHandle) {
			CloseHandle(sharedFenceHandle);
			sharedFenceHandle = nullptr;
		}
		sharedFence11.Reset();
		sharedHasGuides = false;
	}

	bool EnsureSharedSlots(ID3D11Device5* device5, UINT width, UINT height, DXGI_FORMAT format,
		UINT guideWidth, UINT guideHeight, DXGI_FORMAT mvecFormat)
	{
		const bool wantGuides = guideWidth && guideHeight && mvecFormat != DXGI_FORMAT_UNKNOWN;
		if (sharedTex11[0] && sharedWidth == width && sharedHeight == height && sharedFormat == format &&
			sharedFence11 && sharedHasGuides == wantGuides &&
			(!wantGuides || (sharedGuideWidth == guideWidth && sharedGuideHeight == guideHeight &&
								sharedMvecFormat == mvecFormat)))
			return true;
		CloseSharedHandles();
		for (UINT i = 0; i < kSharedSlots; ++i) {
			if (!CreateSharedTex(device5, width, height, format, 0, sharedTex11[i], sharedTexHandle[i],
					"VrAriseTail::SharedKTemp"))
				return false;
		}
		if (wantGuides) {
			for (UINT i = 0; i < kSharedSlots; ++i) {
				if (!CreateSharedTex(device5, guideWidth, guideHeight, DXGI_FORMAT_R32_FLOAT,
						D3D11_BIND_RENDER_TARGET, sharedDepth11[i], sharedDepthHandle[i],
						"VrAriseTail::SharedDepth") ||
					FAILED(device5->CreateRenderTargetView(sharedDepth11[i].Get(), nullptr, &sharedDepthRtv[i])))
					return false;
				Util::SetResourceName(sharedDepthRtv[i].Get(), "VrAriseTail::SharedDepth RTV");
				if (!CreateSharedTex(device5, guideWidth, guideHeight, mvecFormat, 0, sharedMvec11[i],
						sharedMvecHandle[i], "VrAriseTail::SharedMvec"))
					return false;
			}
			sharedGuideWidth = guideWidth;
			sharedGuideHeight = guideHeight;
			sharedMvecFormat = mvecFormat;
			sharedHasGuides = true;
		}
		if (FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sharedFence11))))
			return false;
		if (FAILED(sharedFence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle)))
			return false;
		sharedWidth = width;
		sharedHeight = height;
		sharedFormat = format;
		++sharedGeneration;
		sharedFenceValue = 0;
		return true;
	}

	SharedPublish SnapshotShared()
	{
		std::lock_guard lock(sharedMutex);
		return sharedPublished;
	}

	bool CopySharedHandle(UINT slot, HANDLE& out)
	{
		std::lock_guard lock(sharedMutex);
		if (slot >= kSharedSlots || !sharedTexHandle[slot])
			return false;
		out = sharedTexHandle[slot];
		return true;
	}

	bool CopyGuideHandle(const HANDLE* handles, UINT slot, HANDLE& out)
	{
		std::lock_guard lock(sharedMutex);
		if (slot >= kSharedSlots || !handles[slot])
			return false;
		out = handles[slot];
		return true;
	}

	HANDLE SharedFenceHandle()
	{
		std::lock_guard lock(sharedMutex);
		return sharedFenceHandle;
	}

	void PublishStatus(const VrAriseTail::Status& next)
	{
		std::scoped_lock lock(statusMutex);
		status = next;
	}

	bool BootLatchedArise()
	{
		if (!globals::game::isVR)
			return false;
		auto& upscaling = globals::features::upscaling;
		upscaling.bootSnapshot.LatchIfNeeded(upscaling.settings);
		return upscaling.bootSnapshot.Boot(&Upscaling::Settings::vrArise);
	}

#ifdef ENABLE_SKYRIM_VR
	std::mutex poseMutex;
	bool posesCached = false;
	vr::TrackedDevicePose_t cachedRender[vr::k_unMaxTrackedDeviceCount]{};
	vr::TrackedDevicePose_t cachedGame[vr::k_unMaxTrackedDeviceCount]{};
	VrFrameGen::PoseRing poseRing;

	void CachePoses(const vr::TrackedDevicePose_t* render, const vr::TrackedDevicePose_t* game)
	{
		if (!render)
			return;
		std::scoped_lock lock(poseMutex);
		std::memcpy(cachedRender, render, sizeof(cachedRender));
		if (game)
			std::memcpy(cachedGame, game, sizeof(cachedGame));
		else
			std::memcpy(cachedGame, render, sizeof(cachedGame));
		posesCached = true;
	}

	std::atomic<DWORD> tailThreadId{ 0 };
	using CompositorPosesFn = vr::EVRCompositorError (*)(vr::IVRCompositor*, vr::TrackedDevicePose_t*, uint32_t,
		vr::TrackedDevicePose_t*, uint32_t);
	CompositorPosesFn origWaitGetPoses = nullptr;
	CompositorPosesFn origGetLastPoses = nullptr;
	void* hookedCompositorVtbl = nullptr;

	vr::EVRCompositorError FillCachedPoses(vr::TrackedDevicePose_t* render, uint32_t renderCount,
		vr::TrackedDevicePose_t* game, uint32_t gameCount)
	{
		std::scoped_lock lock(poseMutex);
		if (!posesCached)
			return vr::VRCompositorError_RequestFailed;
		if (render && renderCount) {
			const uint32_t n = (std::min)(renderCount, vr::k_unMaxTrackedDeviceCount);
			std::memcpy(render, cachedRender, sizeof(vr::TrackedDevicePose_t) * n);
		}
		if (game && gameCount) {
			const uint32_t n = (std::min)(gameCount, vr::k_unMaxTrackedDeviceCount);
			std::memcpy(game, cachedGame, sizeof(vr::TrackedDevicePose_t) * n);
		}
		return vr::VRCompositorError_None;
	}

	bool GameThreadWantsCachedPoses()
	{
		return tailOwnsLoop.load(std::memory_order_acquire) &&
			!abandoned.load(std::memory_order_acquire) &&
			GetCurrentThreadId() != tailThreadId.load(std::memory_order_acquire);
	}

	vr::EVRCompositorError WaitGetPosesThunk(vr::IVRCompositor* self, vr::TrackedDevicePose_t* render, uint32_t renderCount,
		vr::TrackedDevicePose_t* game, uint32_t gameCount)
	{
		if (GameThreadWantsCachedPoses())
			return FillCachedPoses(render, renderCount, game, gameCount);
		return origWaitGetPoses(self, render, renderCount, game, gameCount);
	}

	vr::EVRCompositorError GetLastPosesThunk(vr::IVRCompositor* self, vr::TrackedDevicePose_t* render, uint32_t renderCount,
		vr::TrackedDevicePose_t* game, uint32_t gameCount)
	{
		if (GameThreadWantsCachedPoses()) {
			static bool logged = false;
			if (!logged) {
				logger::info("[VrAriseTail] Engine GetLastPoses fed from Tail cache");
				logged = true;
			}
			return FillCachedPoses(render, renderCount, game, gameCount);
		}
		return origGetLastPoses(self, render, renderCount, game, gameCount);
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

	void HookCompositorPoses(vr::IVRCompositor* compositor)
	{
		if (!compositor)
			return;
		void** vtbl = *reinterpret_cast<void***>(compositor);
		if (!vtbl || vtbl == hookedCompositorVtbl)
			return;
		// IVRCompositor_021: 2 = WaitGetPoses, 3 = GetLastPoses.
		if (!SwapCompositorVFunc(vtbl, 2, reinterpret_cast<void*>(&WaitGetPosesThunk),
				reinterpret_cast<void**>(&origWaitGetPoses)) ||
			!SwapCompositorVFunc(vtbl, 3, reinterpret_cast<void*>(&GetLastPosesThunk),
				reinterpret_cast<void**>(&origGetLastPoses))) {
			logger::warn("[VrAriseTail] compositor pose hook failed");
			return;
		}
		hookedCompositorVtbl = vtbl;
		logger::info("[VrAriseTail] compositor WaitGetPoses/GetLastPoses hooked");
	}

	std::mutex throttleMutex;
	std::condition_variable throttleCv;
	int enginePermits = 0;
	std::uint32_t hmdCycles = 0;

	void ReleaseEngineFrame()
	{
		{
			std::lock_guard lock(throttleMutex);
			if (enginePermits < 1)
				enginePermits = 1;
		}
		throttleCv.notify_one();
	}

	bool AcquireEngineFrame()
	{
		std::unique_lock lock(throttleMutex);
		while (enginePermits <= 0 && !abandoned.load(std::memory_order_acquire) &&
			running.load(std::memory_order_acquire)) {
			throttleCv.wait_for(lock, std::chrono::milliseconds(100));
		}
		if (abandoned.load(std::memory_order_acquire) || !running.load(std::memory_order_acquire))
			return false;
		--enginePermits;
		return true;
	}

	void AbandonTakeover()
	{
		abandoned.store(true, std::memory_order_release);
		tailOwnsLoop.store(false, std::memory_order_release);
		running.store(false, std::memory_order_release);
		active.store(false, std::memory_order_release);
		firstSubmitCv.notify_all();
		throttleCv.notify_all();
	}

	void SignalFirstOwnedSubmit()
	{
		if (firstOwnedSubmit.exchange(true, std::memory_order_acq_rel))
			return;
		firstSubmitCv.notify_all();
	}

	struct UpdatePosesHook
	{
		static void thunk(RE::BSOpenVR* openVR, bool useLivePoses)
		{
			if (!BootLatchedArise() || abandoned.load(std::memory_order_acquire)) {
				func(openVR, useLivePoses);
				return;
			}
			if (!tailOwnsLoop.load(std::memory_order_acquire)) {
				std::lock_guard lock(vrLoop);
				if (!tailOwnsLoop.load(std::memory_order_acquire)) {
					func(openVR, useLivePoses);
					return;
				}
			}
			if (useLivePoses && !AcquireEngineFrame()) {
				func(openVR, false);
				return;
			}
			func(openVR, false);
			static bool logged = false;
			if (!logged) {
				logger::info("[VrAriseTail] Engine UpdatePoses via GetLastPoses; X3 throttle on live");
				logged = true;
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

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
			return "other";
		}
	}

	void InstallUpdatePosesHook()
	{
		stl::write_vfunc<0x10, UpdatePosesHook>(RE::VTABLE_BSOpenVR[0]);
	}

	IDXGIAdapter* QueryGameAdapter(ComPtr<IDXGIAdapter>& keep)
	{
		keep.Reset();
		auto* device11 = globals::d3d::device;
		if (!device11)
			return nullptr;
		ComPtr<IDXGIDevice> dxgiDevice;
		if (FAILED(device11->QueryInterface(IID_PPV_ARGS(&dxgiDevice))))
			return nullptr;
		if (FAILED(dxgiDevice->GetAdapter(&keep)))
			return nullptr;
		return keep.Get();
	}

	void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
		D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = resource;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = after;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		list->ResourceBarrier(1, &barrier);
	}

	void CropCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* dst,
		D3D12_RESOURCE_STATES& srcState, D3D12_RESOURCE_STATES& dstState,
		UINT srcX, UINT srcY, UINT w, UINT h)
	{
		if (srcState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
			Transition(list, src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
			srcState = D3D12_RESOURCE_STATE_COPY_SOURCE;
		}
		if (dstState != D3D12_RESOURCE_STATE_COPY_DEST) {
			Transition(list, dst, dstState, D3D12_RESOURCE_STATE_COPY_DEST);
			dstState = D3D12_RESOURCE_STATE_COPY_DEST;
		}
		D3D12_TEXTURE_COPY_LOCATION dstLoc{};
		dstLoc.pResource = dst;
		dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_TEXTURE_COPY_LOCATION srcLoc{};
		srcLoc.pResource = src;
		srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_BOX box{};
		box.left = srcX;
		box.top = srcY;
		box.right = srcX + w;
		box.bottom = srcY + h;
		box.back = 1;
		list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);
		Transition(list, dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		dstState = D3D12_RESOURCE_STATE_COMMON;
	}

	DXGI_FORMAT UavTypedFormat(DXGI_FORMAT format)
	{
		switch (format) {
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return DXGI_FORMAT_B8G8R8A8_UNORM;
		default:
			return format;
		}
	}

	bool FormatAllowsTypedUav(DXGI_FORMAT format)
	{
		switch (format) {
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
			return true;
		default:
			return false;
		}
	}

	struct Dx12Path
	{
		ComPtr<IDXGIAdapter> adapter;
		ComPtr<ID3D12Device> device;
		ComPtr<ID3D12CommandQueue> queue;
		ComPtr<ID3D12CommandQueue> gQueue;
		ComPtr<ID3D12CommandQueue> copyQueue;
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		ComPtr<ID3D12CommandAllocator> ingestAlloc[2];
		ComPtr<ID3D12GraphicsCommandList> ingestList[2];
		UINT ingestAllocSlot = 0;
		UINT64 ingestDone[2]{};
		UINT64 ingestFenceValue = 0;
		UINT64 prepareFenceValue = 0;
		ComPtr<ID3D12DescriptorHeap> rtvHeap;
		ComPtr<ID3D12DescriptorHeap> uavHeap;
		ComPtr<ID3D12RootSignature> opaqueRoot;
		ComPtr<ID3D12PipelineState> opaquePso;
		ComPtr<ID3D12RootSignature> convertRoot;
		ComPtr<ID3D12PipelineState> convertPso;
		ComPtr<ID3D12DescriptorHeap> convertHeap;
		ComPtr<ID3D12Resource> color;
		ComPtr<ID3D12Resource> eye[2][2];
		UINT eyeSubmit = 0;
		UINT pendingEye = 0;
		UINT64 pendingEyeFence = 0;
		ComPtr<ID3D12Resource> barUpload;
		ComPtr<ID3D12Resource> shared12[kSharedSlots];
		ComPtr<ID3D12Resource> sharedDepth12[kSharedSlots];
		ComPtr<ID3D12Resource> sharedMvec12[kSharedSlots];
		ComPtr<ID3D12Resource> depth;
		ComPtr<ID3D12Resource> mvec;
		D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES mvecState = D3D12_RESOURCE_STATE_COMMON;
		UINT guideWidth = 0;
		UINT guideHeight = 0;
		DXGI_FORMAT depthFormat = DXGI_FORMAT_R32_FLOAT;
		DXGI_FORMAT mvecFormat = DXGI_FORMAT_UNKNOWN;
		bool guidesReady = false;
		ComPtr<ID3D12Fence> fence;
		ComPtr<ID3D12Fence> openedSharedFence;
		HANDLE openedFenceHandle = nullptr;
		UINT32 openedGeneration = 0;
		HANDLE fenceEvent = nullptr;
		UINT64 fenceValue = 0;
		UINT64 lastConsumed = 0;
		D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_COMMON;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
		bool sbsBounds = false;
		vr::HmdMatrix34_t submitPose{};
		bool submitPoseValid = false;
		bool submitInterp = false;

		~Dx12Path()
		{
			if (fenceEvent)
				CloseHandle(fenceEvent);
		}

		bool CreateDevice()
		{
			if (device)
				return true;
			IDXGIAdapter* chosen = QueryGameAdapter(adapter);
			const HRESULT hr = D3D12CreateDevice(chosen, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
			if (FAILED(hr)) {
				logger::warn("[VrAriseTail] D3D12CreateDevice failed hr={:08X}", static_cast<unsigned>(hr));
				return false;
			}
			auto& sl = globals::features::upscaling.streamlineDX12;
			if (sl.initialized) {
				sl.SetD3DDevice12(device.Get());
				sl.CheckFeatures(chosen);
				sl.PostDevice();
				if (sl.featureDLSSG && sl.slUpgradeInterface) {
					ID3D12Device* raw = device.Detach();
					const sl::Result upgraded = sl.slUpgradeInterface(reinterpret_cast<void**>(&raw));
					device.Attach(raw);
					if (upgraded == sl::Result::eOk)
						logger::info("[VrAriseTail] Tail D3D12 device SL-upgraded");
					else
						logger::warn("[VrAriseTail] slUpgradeInterface device failed");
				} else
					logger::info("[VrAriseTail] DLSS-G not bound; X4 doorbell off");
			} else
				logger::warn("[VrAriseTail] Streamline DX12 not initialized; X4 doorbell off");
			auto& sl11 = globals::features::upscaling.streamline;
			if (sl11.initialized && sl11.slReflexSetOptions) {
				sl::ReflexOptions off{};
				off.mode = sl::ReflexMode::eOff;
				if (sl11.slReflexSetOptions(off) == sl::Result::eOk)
					logger::info("[VrAriseTail] game DX11 Reflex off; G/Reflex on Tail DX12 only");
			}
			D3D12_COMMAND_QUEUE_DESC queueDesc{};
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
				return false;
			if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&gQueue))))
				return false;
			if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&copyQueue))))
				return false;
			if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
				return false;
			if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
				return false;
			list->Close();
			for (int i = 0; i < 2; ++i) {
				if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ingestAlloc[i]))) ||
					FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ingestAlloc[i].Get(), nullptr,
						IID_PPV_ARGS(&ingestList[i]))) ||
					FAILED(ingestList[i]->Close()))
					return false;
			}
			if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
				return false;
			fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			device->SetName(L"VrAriseTail::Device12");
			queue->SetName(L"VrAriseTail::Queue12");
			gQueue->SetName(L"VrAriseTail::GQueue12");
			copyQueue->SetName(L"VrAriseTail::CopyQueue12");
			return fenceEvent != nullptr;
		}

		bool CreateColor(std::uint32_t w, std::uint32_t h)
		{
			if (color && rtvHeap && barUpload)
				return true;
			color.Reset();
			rtvHeap.Reset();
			barUpload.Reset();
			width = w;
			height = h;
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
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			D3D12_CLEAR_VALUE clear{};
			clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			clear.Color[0] = 0.75f;
			clear.Color[1] = 0.05f;
			clear.Color[2] = 0.85f;
			clear.Color[3] = 1.0f;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&color))))
				return false;
			colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
			color->SetName(L"VrAriseTail::Solid");

			D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
			heapDesc.NumDescriptors = 1;
			heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap))))
				return false;
			rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
			device->CreateRenderTargetView(color.Get(), nullptr, rtv);

			const UINT rowPitch = 256;
			D3D12_HEAP_PROPERTIES uploadHeap{};
			uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
			D3D12_RESOURCE_DESC uploadDesc{};
			uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			uploadDesc.Width = static_cast<UINT64>(rowPitch) * h;
			uploadDesc.Height = 1;
			uploadDesc.DepthOrArraySize = 1;
			uploadDesc.MipLevels = 1;
			uploadDesc.SampleDesc.Count = 1;
			uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&barUpload))))
				return false;
			void* mapped = nullptr;
			if (FAILED(barUpload->Map(0, nullptr, &mapped)) || !mapped)
				return false;
			auto* bytes = static_cast<std::uint8_t*>(mapped);
			for (std::uint32_t y = 0; y < h; ++y) {
				auto* row = bytes + static_cast<std::size_t>(y) * rowPitch;
				for (std::uint32_t x = 0; x < 16; ++x) {
					row[x * 4 + 0] = 255;
					row[x * 4 + 1] = 255;
					row[x * 4 + 2] = 255;
					row[x * 4 + 3] = 255;
				}
			}
			barUpload->Unmap(0, nullptr);
			return true;
		}

		bool FenceReady(UINT64 value) const
		{
			if (!fence || value == 0)
				return true;
			return fence->GetCompletedValue() >= value;
		}

		void WaitFenceValue(UINT64 value)
		{
			WaitFenceTimeout(value, INFINITE);
		}

		bool WaitFenceTimeout(UINT64 value, DWORD ms = kFenceWaitMs)
		{
			return WaitFenceCpu(fence.Get(), fenceEvent, value, ms);
		}

		void WaitFence()
		{
			WaitFenceValue(fenceValue);
		}

		bool HasSubmitEyes() const
		{
			return eye[eyeSubmit][0] && eye[eyeSubmit][1];
		}

		bool SetMatches(UINT set, UINT eyeW, UINT eyeH, DXGI_FORMAT format) const
		{
			if (set > 1 || !eye[set][0] || !eye[set][1])
				return false;
			const auto left = eye[set][0]->GetDesc();
			const auto right = eye[set][1]->GetDesc();
			return left.Width == eyeW && left.Height == eyeH && left.Format == format &&
				right.Width == eyeW && right.Height == eyeH && right.Format == format;
		}

		bool EyesMatch(UINT eyeW, UINT eyeH, DXGI_FORMAT format) const
		{
			return SetMatches(0, eyeW, eyeH, format) && SetMatches(1, eyeW, eyeH, format);
		}

		bool Paint(std::uint32_t frame)
		{
			WaitFence();
			if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
				return false;
			if (colorState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
				Transition(list.Get(), color.Get(), colorState, D3D12_RESOURCE_STATE_RENDER_TARGET);
				colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
			}
			const float clear[4]{ 0.75f, 0.05f, 0.85f, 1.0f };
			list->ClearRenderTargetView(rtv, clear, 0, nullptr);
			Transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
			colorState = D3D12_RESOURCE_STATE_COPY_DEST;

			const std::uint32_t barW = 16;
			const std::uint32_t barX = width > barW ? ((frame * 8u) % (width - barW)) : 0;
			D3D12_TEXTURE_COPY_LOCATION dst{};
			dst.pResource = color.Get();
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			D3D12_TEXTURE_COPY_LOCATION src{};
			src.pResource = barUpload.Get();
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			src.PlacedFootprint.Footprint.Width = barW;
			src.PlacedFootprint.Footprint.Height = height;
			src.PlacedFootprint.Footprint.Depth = 1;
			src.PlacedFootprint.Footprint.RowPitch = 256;
			list->CopyTextureRegion(&dst, barX, 0, 0, &src, nullptr);
			Transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
			colorState = D3D12_RESOURCE_STATE_COMMON;
			if (FAILED(list->Close()))
				return false;
			ID3D12CommandList* lists[]{ list.Get() };
			queue->ExecuteCommandLists(1, lists);
			++fenceValue;
			queue->Signal(fence.Get(), fenceValue);
			return true;
		}

		void SubmitEyes(vr::IVRCompositor* compositor, vr::EVRCompositorError& errL, vr::EVRCompositorError& errR)
		{
			const vr::VRTextureBounds_t bounds{ 0.f, 0.f, 1.f, 1.f };
			const bool withPose = false;
			auto submitOne = [&](vr::EVREye eyeIndex, ID3D12Resource* resource, vr::EVRCompositorError& err) {
				vr::D3D12TextureData_t dx12{};
				dx12.m_pResource = resource;
				dx12.m_pCommandQueue = queue.Get();
				if (withPose) {
					vr::VRTextureWithPose_t tex{};
					tex.handle = &dx12;
					tex.eType = vr::TextureType_DirectX12;
					tex.eColorSpace = vr::ColorSpace_Auto;
					tex.mDeviceToAbsoluteTracking = submitPose;
					err = compositor->Submit(eyeIndex, &tex, &bounds, vr::Submit_TextureWithPose);
				} else {
					vr::Texture_t tex{};
					tex.handle = &dx12;
					tex.eType = vr::TextureType_DirectX12;
					tex.eColorSpace = vr::ColorSpace_Auto;
					err = compositor->Submit(eyeIndex, &tex, &bounds, vr::Submit_Default);
				}
			};
			ID3D12Resource* left = HasSubmitEyes() ? eye[eyeSubmit][0].Get() : color.Get();
			ID3D12Resource* right = HasSubmitEyes() ? eye[eyeSubmit][1].Get() : color.Get();
			submitOne(vr::Eye_Left, left, errL);
			submitOne(vr::Eye_Right, right, errR);
			if (left) {
				const auto desc = left->GetDesc();
				static UINT loggedW = 0;
				static UINT loggedH = 0;
				static bool loggedTwP = false;
				if (desc.Width != loggedW || desc.Height != loggedH) {
					logger::info("[VrAriseTail] Submit eye {}x{} via={}",
						static_cast<unsigned>(desc.Width), desc.Height,
						(left == color.Get()) ? "color" : "eyes");
					loggedW = static_cast<UINT>(desc.Width);
					loggedH = desc.Height;
				}
				if (withPose && !loggedTwP) {
					logger::info("[VrAriseTail] X4 Submit_TextureWithPose interp errL={} ({}) errR={} ({})",
						static_cast<int>(errL), CompositorErrorName(errL),
						static_cast<int>(errR), CompositorErrorName(errR));
					loggedTwP = true;
				}
			}
		}

		bool EnsureOpaqueCs()
		{
			if (opaquePso)
				return true;
			static const char kSrc[] =
				"RWTexture2D<float4> Out : register(u0);\n"
				"[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {\n"
				"  float4 c = Out[id.xy]; c.a = 1.0; Out[id.xy] = c;\n"
				"}\n";
			ComPtr<ID3DBlob> blob;
			ComPtr<ID3DBlob> errors;
			if (FAILED(D3DCompile(kSrc, sizeof(kSrc) - 1, "VrAriseOpaque", nullptr, nullptr, "main", "cs_5_1",
					0, 0, &blob, &errors))) {
				logger::warn("[VrAriseTail] ForceOpaque CS compile failed: {}",
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
				return false;
			}
			D3D12_DESCRIPTOR_RANGE range{};
			range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			range.NumDescriptors = 1;
			D3D12_ROOT_PARAMETER param{};
			param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			param.DescriptorTable.NumDescriptorRanges = 1;
			param.DescriptorTable.pDescriptorRanges = &range;
			D3D12_ROOT_SIGNATURE_DESC rsDesc{};
			rsDesc.NumParameters = 1;
			rsDesc.pParameters = &param;
			ComPtr<ID3DBlob> rsBlob;
			if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr)))
				return false;
			if (FAILED(device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
					IID_PPV_ARGS(&opaqueRoot))))
				return false;
			D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
			pso.pRootSignature = opaqueRoot.Get();
			pso.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
			if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&opaquePso))))
				return false;
			D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
			heapDesc.NumDescriptors = 1;
			heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&uavHeap))))
				return false;
			return true;
		}

		bool EnsureConvertCs()
		{
			if (convertPso)
				return true;
			static const char kSrc[] =
				"Texture2D<float4> InTex : register(t0);\n"
				"RWTexture2D<float4> OutTex : register(u0);\n"
				"[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {\n"
				"  OutTex[id.xy] = float4(InTex.Load(int3(id.xy, 0)).rgb, 1.0);\n"
				"}\n";
			ComPtr<ID3DBlob> blob;
			ComPtr<ID3DBlob> errors;
			if (FAILED(D3DCompile(kSrc, sizeof(kSrc) - 1, "VrAriseConvert", nullptr, nullptr, "main", "cs_5_1",
					0, 0, &blob, &errors))) {
				logger::warn("[VrAriseTail] Convert CS compile failed: {}",
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
				return false;
			}
			D3D12_DESCRIPTOR_RANGE ranges[2]{};
			ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			ranges[0].NumDescriptors = 1;
			ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			ranges[1].NumDescriptors = 1;
			ranges[1].OffsetInDescriptorsFromTableStart = 1;
			D3D12_ROOT_PARAMETER param{};
			param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			param.DescriptorTable.NumDescriptorRanges = 2;
			param.DescriptorTable.pDescriptorRanges = ranges;
			D3D12_ROOT_SIGNATURE_DESC rsDesc{};
			rsDesc.NumParameters = 1;
			rsDesc.pParameters = &param;
			ComPtr<ID3DBlob> rsBlob;
			if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr)))
				return false;
			if (FAILED(device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
					IID_PPV_ARGS(&convertRoot))))
				return false;
			D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
			pso.pRootSignature = convertRoot.Get();
			pso.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
			if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&convertPso))))
				return false;
			D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
			heapDesc.NumDescriptors = 2;
			heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			return SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&convertHeap)));
		}

		bool ConvertToRgba8(ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, ID3D12Resource* dst,
			UINT w, UINT h, D3D12_RESOURCE_STATES& srcState, D3D12_RESOURCE_STATES& dstState)
		{
			if (!EnsureConvertCs() || !cmd || !src || !dst)
				return false;
			if (srcState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
				Transition(cmd, src, srcState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				srcState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			}
			if (dstState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
				Transition(cmd, dst, dstState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				dstState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			}
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = src->GetDesc().Format;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			auto cpu = convertHeap->GetCPUDescriptorHandleForHeapStart();
			device->CreateShaderResourceView(src, &srv, cpu);
			cpu.ptr += stride;
			device->CreateUnorderedAccessView(dst, nullptr, &uav, cpu);
			ID3D12DescriptorHeap* heaps[]{ convertHeap.Get() };
			cmd->SetDescriptorHeaps(1, heaps);
			cmd->SetComputeRootSignature(convertRoot.Get());
			cmd->SetPipelineState(convertPso.Get());
			cmd->SetComputeRootDescriptorTable(0, convertHeap->GetGPUDescriptorHandleForHeapStart());
			cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
			Transition(cmd, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			dstState = D3D12_RESOURCE_STATE_COMMON;
			Transition(cmd, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			srcState = D3D12_RESOURCE_STATE_COMMON;
			return true;
		}

		bool EnsureColorFor(UINT w, UINT h, DXGI_FORMAT format)
		{
			if (color && width == w && height == h && color->GetDesc().Format == format)
				return true;
			color.Reset();
			rtvHeap.Reset();
			barUpload.Reset();
			width = w;
			height = h;
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = w;
			desc.Height = h;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&color)))) {
				desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
				if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
						D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&color))))
					return false;
			}
			colorState = D3D12_RESOURCE_STATE_COMMON;
			color->SetName(L"VrAriseTail::SharedCopy");
			return true;
		}

		bool EnsureEyes(UINT eyeW, UINT eyeH, DXGI_FORMAT format)
		{
			auto match = [&](ID3D12Resource* resource) {
				if (!resource)
					return false;
				const auto desc = resource->GetDesc();
				return desc.Width == eyeW && desc.Height == eyeH && desc.Format == format;
			};
			if (match(eye[0][0].Get()) && match(eye[0][1].Get()) &&
				match(eye[1][0].Get()) && match(eye[1][1].Get()))
				return true;
			if (HasSubmitEyes())
				return false;
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = eyeW;
			desc.Height = eyeH;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			static const wchar_t* kNames[2][2]{
				{ L"VrAriseTail::EyeL0", L"VrAriseTail::EyeR0" },
				{ L"VrAriseTail::EyeL1", L"VrAriseTail::EyeR1" }
			};
			for (int set = 0; set < 2; ++set) {
				for (int i = 0; i < 2; ++i) {
					if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
							D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&eye[set][i]))))
						return false;
					eye[set][i]->SetName(kNames[set][i]);
				}
			}
			eyeSubmit = 0;
			pendingEye = 0;
			pendingEyeFence = 0;
			return HasSubmitEyes() && eye[1][0] && eye[1][1];
		}

		bool EnsureGuide(ComPtr<ID3D12Resource>& resource, D3D12_RESOURCE_STATES& state,
			UINT w, UINT h, DXGI_FORMAT format, const wchar_t* name)
		{
			if (resource) {
				const auto desc = resource->GetDesc();
				if (desc.Width == w && desc.Height == h && desc.Format == format)
					return true;
			}
			resource.Reset();
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = w;
			desc.Height = h;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))))
				return false;
			resource->SetName(name);
			state = D3D12_RESOURCE_STATE_COMMON;
			return true;
		}

		bool ImportShared(const SharedPublish& published)
		{
			if (openedGeneration != published.generation) {
				for (auto& resource : shared12)
					resource.Reset();
				for (auto& resource : sharedDepth12)
					resource.Reset();
				for (auto& resource : sharedMvec12)
					resource.Reset();
				openedSharedFence.Reset();
				openedFenceHandle = nullptr;
				openedGeneration = published.generation;
			}
			const HANDLE fenceHandle = SharedFenceHandle();
			if (!openedSharedFence) {
				if (!fenceHandle || FAILED(device->OpenSharedHandle(fenceHandle, IID_PPV_ARGS(&openedSharedFence))))
					return false;
				openedFenceHandle = fenceHandle;
			}
			if (!shared12[published.slot]) {
				HANDLE texHandle = nullptr;
				if (!CopySharedHandle(published.slot, texHandle) ||
					FAILED(device->OpenSharedHandle(texHandle, IID_PPV_ARGS(&shared12[published.slot]))))
					return false;
			}
			return shared12[published.slot] != nullptr;
		}

		bool ImportGuides(const SharedPublish& published)
		{
			if (!published.hasGuides)
				return false;
			if (!sharedDepth12[published.slot]) {
				HANDLE handle = nullptr;
				if (!CopyGuideHandle(sharedDepthHandle, published.slot, handle) ||
					FAILED(device->OpenSharedHandle(handle, IID_PPV_ARGS(&sharedDepth12[published.slot]))))
					return false;
			}
			if (!sharedMvec12[published.slot]) {
				HANDLE handle = nullptr;
				if (!CopyGuideHandle(sharedMvecHandle, published.slot, handle) ||
					FAILED(device->OpenSharedHandle(handle, IID_PPV_ARGS(&sharedMvec12[published.slot]))))
					return false;
			}
			return sharedDepth12[published.slot] && sharedMvec12[published.slot];
		}

		bool PrepareShared(const SharedPublish& published, bool cropEyes)
		{
			if (!published.fenceValue || !queue || !ImportShared(published) ||
				!EnsureColorFor(published.width, published.height, published.format))
				return false;
			if (!WaitFenceCpu(openedSharedFence.Get(), fenceEvent, published.fenceValue, kFenceWaitMs))
				return false;
			if (!WaitFenceTimeout(fenceValue, kFenceWaitMs))
				return false;
			if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
				return false;
			if (colorState != D3D12_RESOURCE_STATE_COPY_DEST) {
				Transition(list.Get(), color.Get(), colorState, D3D12_RESOURCE_STATE_COPY_DEST);
				colorState = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			list->CopyResource(color.Get(), shared12[published.slot].Get());
			if (FormatAllowsTypedUav(published.format) && EnsureOpaqueCs()) {
				Transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				colorState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
				D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
				uavDesc.Format = UavTypedFormat(published.format);
				uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				device->CreateUnorderedAccessView(color.Get(), nullptr, &uavDesc,
					uavHeap->GetCPUDescriptorHandleForHeapStart());
				ID3D12DescriptorHeap* heaps[]{ uavHeap.Get() };
				list->SetDescriptorHeaps(1, heaps);
				list->SetComputeRootSignature(opaqueRoot.Get());
				list->SetPipelineState(opaquePso.Get());
				list->SetComputeRootDescriptorTable(0, uavHeap->GetGPUDescriptorHandleForHeapStart());
				list->Dispatch((published.width + 7) / 8, (published.height + 7) / 8, 1);
				Transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			} else {
				Transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
			}
			colorState = D3D12_RESOURCE_STATE_COMMON;
			if (cropEyes)
				sbsBounds = published.width >= published.height + (published.height / 2);
			if (cropEyes && sbsBounds) {
				const UINT eyeW = published.width / 2;
				if (!EnsureEyes(eyeW, published.height, published.format))
					sbsBounds = false;
				else {
					Transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
					colorState = D3D12_RESOURCE_STATE_COPY_SOURCE;
					for (int i = 0; i < 2; ++i) {
						Transition(list.Get(), eye[eyeSubmit][i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
						D3D12_TEXTURE_COPY_LOCATION dst{};
						dst.pResource = eye[eyeSubmit][i].Get();
						dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
						D3D12_TEXTURE_COPY_LOCATION src{};
						src.pResource = color.Get();
						src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
						D3D12_BOX box{};
						box.left = i * eyeW;
						box.right = box.left + eyeW;
						box.top = 0;
						box.bottom = published.height;
						box.front = 0;
						box.back = 1;
						list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
						Transition(list.Get(), eye[eyeSubmit][i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
					}
					Transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
					colorState = D3D12_RESOURCE_STATE_COMMON;
				}
			}
			guidesReady = false;
			if (published.hasGuides && ImportGuides(published) &&
				EnsureGuide(depth, depthState, published.guideWidth, published.guideHeight,
					published.depthFormat, L"VrAriseTail::Depth") &&
				EnsureGuide(mvec, mvecState, published.guideWidth, published.guideHeight,
					published.mvecFormat, L"VrAriseTail::Mvec")) {
				if (depthState != D3D12_RESOURCE_STATE_COPY_DEST) {
					Transition(list.Get(), depth.Get(), depthState, D3D12_RESOURCE_STATE_COPY_DEST);
					depthState = D3D12_RESOURCE_STATE_COPY_DEST;
				}
				if (mvecState != D3D12_RESOURCE_STATE_COPY_DEST) {
					Transition(list.Get(), mvec.Get(), mvecState, D3D12_RESOURCE_STATE_COPY_DEST);
					mvecState = D3D12_RESOURCE_STATE_COPY_DEST;
				}
				list->CopyResource(depth.Get(), sharedDepth12[published.slot].Get());
				list->CopyResource(mvec.Get(), sharedMvec12[published.slot].Get());
				Transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
				Transition(list.Get(), mvec.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
				depthState = D3D12_RESOURCE_STATE_COMMON;
				mvecState = D3D12_RESOURCE_STATE_COMMON;
				guideWidth = published.guideWidth;
				guideHeight = published.guideHeight;
				depthFormat = published.depthFormat;
				mvecFormat = published.mvecFormat;
				guidesReady = true;
			}
			if (FAILED(list->Close()))
				return false;
			ID3D12CommandList* lists[]{ list.Get() };
			queue->ExecuteCommandLists(1, lists);
			++fenceValue;
			queue->Signal(fence.Get(), fenceValue);
			prepareFenceValue = fenceValue;
			lastConsumed = published.fenceValue;
			return true;
		}


		bool IngestSbs(ID3D12Resource* src, UINT w, UINT h, DXGI_FORMAT format)
		{
			if (!src || !queue || !EnsureColorFor(w, h, format)) {
				logger::error("[VrAriseTail] X4 IngestSbs refused: src={} {}x{}", src != nullptr, w, h);
				return false;
			}
			if (!WaitFenceTimeout(fenceValue, kFenceWaitMs))
				return false;
			if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) {
				logger::error("[VrAriseTail] X4 IngestSbs command list reset failed");
				return false;
			}
			if (colorState != D3D12_RESOURCE_STATE_COPY_DEST) {
				Transition(list.Get(), color.Get(), colorState, D3D12_RESOURCE_STATE_COPY_DEST);
				colorState = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			list->CopyResource(color.Get(), src);
			if (FormatAllowsTypedUav(format) && EnsureOpaqueCs()) {
				Transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				colorState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
				D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
				uavDesc.Format = UavTypedFormat(format);
				uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				device->CreateUnorderedAccessView(color.Get(), nullptr, &uavDesc,
					uavHeap->GetCPUDescriptorHandleForHeapStart());
				ID3D12DescriptorHeap* heaps[]{ uavHeap.Get() };
				list->SetDescriptorHeaps(1, heaps);
				list->SetComputeRootSignature(opaqueRoot.Get());
				list->SetPipelineState(opaquePso.Get());
				list->SetComputeRootDescriptorTable(0, uavHeap->GetGPUDescriptorHandleForHeapStart());
				list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
				Transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			} else {
				Transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
			}
			colorState = D3D12_RESOURCE_STATE_COMMON;
			sbsBounds = w >= h + (h / 2);
			if (sbsBounds) {
				const UINT eyeW = w / 2;
				if (!EnsureEyes(eyeW, h, format))
					sbsBounds = false;
				else {
					Transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
					colorState = D3D12_RESOURCE_STATE_COPY_SOURCE;
					for (int i = 0; i < 2; ++i) {
						Transition(list.Get(), eye[eyeSubmit][i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
						D3D12_TEXTURE_COPY_LOCATION dst{};
						dst.pResource = eye[eyeSubmit][i].Get();
						dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
						D3D12_TEXTURE_COPY_LOCATION srcLoc{};
						srcLoc.pResource = color.Get();
						srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
						D3D12_BOX box{};
						box.left = i * eyeW;
						box.right = box.left + eyeW;
						box.bottom = h;
						box.back = 1;
						list->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, &box);
						Transition(list.Get(), eye[eyeSubmit][i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
					}
					Transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
					colorState = D3D12_RESOURCE_STATE_COMMON;
				}
			}
			if (FAILED(list->Close()))
				return false;
			ID3D12CommandList* lists[]{ list.Get() };
			queue->ExecuteCommandLists(1, lists);
			++fenceValue;
			queue->Signal(fence.Get(), fenceValue);
			ingestFenceValue = fenceValue;
			return true;
		}

		bool IngestEyes(ID3D12Resource* left, ID3D12Resource* right, UINT w, UINT h, DXGI_FORMAT format,
			ID3D12Fence* srcFence, uint64_t srcValue)
		{
			if (!left || !right || !copyQueue || !ingestAlloc[0] || !ingestList[0] || !EyesMatch(w, h, format)) {
				static bool loggedRefuse = false;
				if (!loggedRefuse) {
					UINT haveW = 0;
					UINT haveH = 0;
					if (HasSubmitEyes()) {
						const auto desc = eye[0][0]->GetDesc();
						haveW = static_cast<UINT>(desc.Width);
						haveH = desc.Height;
					}
					logger::error("[VrAriseTail] X4 IngestEyes refused want={}x{} have={}x{}", w, h, haveW, haveH);
					loggedRefuse = true;
				}
				return false;
			}
			const UINT dest = 1u - eyeSubmit;
			if (dest == eyeSubmit || !SetMatches(dest, w, h, format))
				return false;
			if (srcFence && srcValue)
				copyQueue->Wait(srcFence, srcValue);
			const UINT a = ingestAllocSlot;
			if (!FenceReady(ingestDone[a]))
				return false;
			if (FAILED(ingestAlloc[a]->Reset()) || FAILED(ingestList[a]->Reset(ingestAlloc[a].Get(), nullptr)))
				return false;
			ID3D12Resource* src[2]{ left, right };
			for (int i = 0; i < 2; ++i) {
				Transition(ingestList[a].Get(), eye[dest][i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
				ingestList[a]->CopyResource(eye[dest][i].Get(), src[i]);
				Transition(ingestList[a].Get(), eye[dest][i].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
					D3D12_RESOURCE_STATE_COMMON);
			}
			sbsBounds = true;
			if (FAILED(ingestList[a]->Close()))
				return false;
			ID3D12CommandList* lists[]{ ingestList[a].Get() };
			copyQueue->ExecuteCommandLists(1, lists);
			++fenceValue;
			copyQueue->Signal(fence.Get(), fenceValue);
			ingestDone[a] = fenceValue;
			ingestFenceValue = fenceValue;
			pendingEye = dest;
			pendingEyeFence = fenceValue;
			ingestAllocSlot = 1u - a;
			return true;
		}
	};

	struct DeviceBPath
	{
		ComPtr<IDXGIAdapter> adapter;
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		ComPtr<ID3D11Texture2D> color;
		ComPtr<ID3D11RenderTargetView> rtv;
		std::vector<std::uint8_t> barPixels;
		std::uint32_t width = 0;
		std::uint32_t height = 0;

		bool CreateDevice()
		{
			if (device)
				return true;
			IDXGIAdapter* chosen = QueryGameAdapter(adapter);
			const D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
			const D3D_DRIVER_TYPE type = chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
			const HRESULT hr = D3D11CreateDevice(chosen, type, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
				&device, nullptr, &context);
			if (FAILED(hr)) {
				logger::warn("[VrAriseTail] Device B D3D11CreateDevice failed hr={:08X}", static_cast<unsigned>(hr));
				return false;
			}
			return true;
		}

		bool CreateColor(std::uint32_t w, std::uint32_t h)
		{
			if (color)
				return true;
			width = w;
			height = h;
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = w;
			desc.Height = h;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			if (FAILED(device->CreateTexture2D(&desc, nullptr, &color)))
				return false;
			if (FAILED(device->CreateRenderTargetView(color.Get(), nullptr, &rtv)))
				return false;
			barPixels.assign(static_cast<std::size_t>(16) * h * 4, 255);
			return true;
		}

		void Paint(std::uint32_t frame)
		{
			const float clear[4]{ 0.75f, 0.05f, 0.85f, 1.0f };
			context->ClearRenderTargetView(rtv.Get(), clear);
			const std::uint32_t barW = 16;
			const std::uint32_t barX = width > barW ? ((frame * 8u) % (width - barW)) : 0;
			D3D11_BOX box{};
			box.left = barX;
			box.right = barX + barW;
			box.top = 0;
			box.bottom = height;
			box.front = 0;
			box.back = 1;
			context->UpdateSubresource(color.Get(), 0, &box, barPixels.data(), barW * 4, 0);
			context->Flush();
		}

		void SubmitEyes(vr::IVRCompositor* compositor, vr::EVRCompositorError& errL, vr::EVRCompositorError& errR)
		{
			vr::Texture_t tex{};
			tex.handle = color.Get();
			tex.eType = vr::TextureType_DirectX;
			tex.eColorSpace = vr::ColorSpace_Auto;
			const vr::VRTextureBounds_t bounds{ 0.f, 0.f, 1.f, 1.f };
			errL = compositor->Submit(vr::Eye_Left, &tex, &bounds, vr::Submit_Default);
			errR = compositor->Submit(vr::Eye_Right, &tex, &bounds, vr::Submit_Default);
		}
	};

	constexpr UINT kRingSlots = 4;
	constexpr UINT kPairSlots = 8;
	using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

	struct Doorbell;
	Doorbell* g_doorbell = nullptr;

	struct Doorbell
	{
		Dx12Path* dx12 = nullptr;
		HWND hwnd[2]{};
		ComPtr<IDXGISwapChain3> swapChain[2];
		IDXGISwapChain* nativeSc[2]{};
		ComPtr<ID3D12Resource> bb[2][3];
		ComPtr<ID3D12Resource> ring[2][kRingSlots];
		ComPtr<ID3D12Resource> hudlessKeep[2][2];
		D3D12_RESOURCE_STATES keepState[2][2]{
			{ D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON },
			{ D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON }
		};
		ComPtr<ID3D12Resource> keepDepth[2][2];
		ComPtr<ID3D12Resource> keepMvec[2][2];
		D3D12_RESOURCE_STATES keepDepthState[2][2]{
			{ D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON },
			{ D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON }
		};
		D3D12_RESOURCE_STATES keepMvecState[2][2]{
			{ D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON },
			{ D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON }
		};
		bool keepHasGuides[2][2]{};
		uint64_t keepFenceValue[2]{};
		UINT keepW = 0;
		UINT keepH = 0;
		UINT keepGuideW = 0;
		UINT keepGuideH = 0;
		DXGI_FORMAT keepDepthFmt = DXGI_FORMAT_R32_FLOAT;
		DXGI_FORMAT keepMvecFmt = DXGI_FORMAT_UNKNOWN;
		UINT publishedKeep = 0;
		std::atomic<uint32_t> kickSlot{ 1 };
		ComPtr<ID3D12CommandAllocator> hookAlloc[2];
		ComPtr<ID3D12GraphicsCommandList> hookList[2];
		ComPtr<ID3D12CommandAllocator> assembleAlloc;
		ComPtr<ID3D12GraphicsCommandList> assembleList;
		ComPtr<ID3D12CommandAllocator> kickAlloc;
		ComPtr<ID3D12GraphicsCommandList> kickList;
		ComPtr<ID3D12Fence> kickFence;
		HANDLE kickFenceEvent = nullptr;
		HANDLE snapshotWaitEvent = nullptr;
		uint64_t kickFenceValue = 0;
		std::thread worker;
		std::mutex kickMutex;
		std::condition_variable kickCv;
		std::atomic<bool> stop{ false };
		std::atomic<bool> pendingKick{ false };
		std::atomic<uint64_t> colorFence{ 0 };
		UINT reqW = 0;
		UINT reqH = 0;
		DXGI_FORMAT reqFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
		ComPtr<ID3D12Fence> ringFence;
		HANDLE ringFenceEvent = nullptr;
		std::atomic<uint64_t> ringFenceValue{ 0 };
		uint64_t lastHookFence[2]{};
		uint64_t lastAssembleFence = 0;
		void* clonedVtbl[2][48]{};
		PresentFn origPresent[2]{};
		std::mutex hookMutex;
		uint64_t slotFence[2][kRingSlots]{};
		uint64_t slotSeq[kRingSlots]{};
		UINT writeSlot[2]{};
		UINT holdSlot = kRingSlots;
		std::atomic<uint64_t> produceSeq{ 0 };
		std::atomic<uint64_t> consumeSeq{ 0 };
		uint64_t kickGen = 0;
		static constexpr UINT kPending = 32;
		struct PendingPresent
		{
			uint64_t gen = 0;
			uint32_t remaining = 0;
		};
		PendingPresent pending[2][kPending]{};
		UINT pendR[2]{};
		UINT pendW[2]{};
		static constexpr UINT kGenRecs = 16;
		struct GenRec
		{
			uint64_t gen = 0;
			bool inUse = false;
			bool haveI[2]{};
			bool haveR[2]{};
			UINT slotI[2]{};
			UINT slotR[2]{};
			uint64_t fenceI[2]{};
			uint64_t fenceR[2]{};
		};
		GenRec genRecs[kGenRecs]{};
		struct StagedPair
		{
			ComPtr<ID3D12Resource> eye[2];
			uint64_t gen = 0;
			uint64_t fence = 0;
			bool valid = false;
		};
		StagedPair lastR{};
		StagedPair newestR{};
		StagedPair parkedI{};
		struct PairOut
		{
			ComPtr<ID3D12Resource> eye[2];
			uint64_t seq = 0;
			uint64_t gen = 0;
			uint64_t fence = 0;
			bool interp = false;
			bool occupied = false;
		};
		PairOut pairs[kPairSlots]{};
		uint64_t pairProduce = 0;
		std::atomic<uint64_t> pairConsume{ 0 };
		UINT sbsW = 0;
		UINT width = 0;
		UINT height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
		std::atomic<bool> ready{ false };
		bool failed = false;
		bool firstPresentDone = false;
		bool gOnLogged = false;
		bool gEnabled = false;
		uint32_t nativePresents = 0;
		uint32_t kicks = 0;
		uint32_t kickFrameId = 0;
		LARGE_INTEGER lastKickQpc{};

		~Doorbell()
		{
			stop.store(true, std::memory_order_release);
			kickCv.notify_all();
			if (worker.joinable())
				worker.join();
			if (g_doorbell == this)
				g_doorbell = nullptr;
			if (ringFenceEvent) {
				CloseHandle(ringFenceEvent);
				ringFenceEvent = nullptr;
			}
			if (kickFenceEvent) {
				CloseHandle(kickFenceEvent);
				kickFenceEvent = nullptr;
			}
			if (snapshotWaitEvent) {
				CloseHandle(snapshotWaitEvent);
				snapshotWaitEvent = nullptr;
			}
		}

		bool WaitRing(uint64_t value)
		{
			return WaitFenceCpu(ringFence.Get(), ringFenceEvent, value, INFINITE);
		}

		bool WaitKick()
		{
			return WaitFenceCpu(kickFence.Get(), kickFenceEvent, kickFenceValue, INFINITE);
		}

		ID3D12CommandQueue* Gpu() const
		{
			return dx12 ? dx12->queue.Get() : nullptr;
		}

		bool CreateTex(ComPtr<ID3D12Resource>& out, UINT w, UINT h, DXGI_FORMAT fmt, bool uav,
			const wchar_t* name)
		{
			out.Reset();
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
			if (uav)
				desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			if (FAILED(dx12->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out))))
				return false;
			out->SetName(name);
			return true;
		}

		bool CreateKeep(UINT w, UINT h, DXGI_FORMAT fmt)
		{
			if (hudlessKeep[0][0] && keepW == w && keepH == h &&
				hudlessKeep[0][0]->GetDesc().Format == fmt)
				return true;
			for (int i = 0; i < 2; ++i) {
				if (!CreateTex(hudlessKeep[0][i], w, h, fmt, true, L"VrAriseTail::HudlessKeep"))
					return false;
				keepState[0][i] = D3D12_RESOURCE_STATE_COMMON;
			}
			keepFenceValue[0] = 0;
			keepW = w;
			keepH = h;
			return true;
		}

		bool CreateGuideKeep(UINT w, UINT h, DXGI_FORMAT depthFmt, DXGI_FORMAT mvecFmt)
		{
			if (keepDepth[0][0] && keepGuideW == w && keepGuideH == h &&
				keepDepthFmt == depthFmt && keepMvecFmt == mvecFmt)
				return true;
			for (int i = 0; i < 2; ++i) {
				if (!CreateTex(keepDepth[0][i], w, h, depthFmt, false, L"VrAriseTail::KeepDepth") ||
					!CreateTex(keepMvec[0][i], w, h, mvecFmt, false, L"VrAriseTail::KeepMvec"))
					return false;
				keepDepthState[0][i] = D3D12_RESOURCE_STATE_COMMON;
				keepMvecState[0][i] = D3D12_RESOURCE_STATE_COMMON;
				keepHasGuides[0][i] = false;
			}
			keepGuideW = w;
			keepGuideH = h;
			keepDepthFmt = depthFmt;
			keepMvecFmt = mvecFmt;
			return true;
		}

		static HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain* self, UINT sync, UINT flags)
		{
			if (!g_doorbell || !self)
				return E_FAIL;
			for (int eye = 0; eye < 2; ++eye) {
				if (self == g_doorbell->nativeSc[eye] || self == g_doorbell->swapChain[eye].Get()) {
					g_doorbell->OnNativePresent(eye, self);
					return g_doorbell->origPresent[eye] ?
						g_doorbell->origPresent[eye](self, sync, flags) :
						E_FAIL;
				}
			}
			return E_FAIL;
		}

		bool CreateRing(UINT w, UINT h, DXGI_FORMAT fmt)
		{
			for (UINT i = 0; i < kRingSlots; ++i) {
				if (!CreateTex(ring[0][i], w, h, fmt, false, L"VrAriseTail::GRing"))
					return false;
			}
			return true;
		}

		void HookNative(int eye, IDXGISwapChain* target)
		{
			if (!target || origPresent[eye] || eye < 0 || eye > 1)
				return;
			void*** object = reinterpret_cast<void***>(target);
			void** realVtbl = *object;
			std::memcpy(clonedVtbl[eye], realVtbl, sizeof(clonedVtbl[eye]));
			origPresent[eye] = reinterpret_cast<PresentFn>(clonedVtbl[eye][8]);
			clonedVtbl[eye][8] = reinterpret_cast<void*>(&PresentThunk);
			*object = clonedVtbl[eye];
			nativeSc[eye] = target;
			logger::info("[VrAriseTail] X4 cloned Present vtable eye={}", eye);
		}

		bool Ensure(UINT sbsWidth, UINT sbsHeight, DXGI_FORMAT fmt)
		{
			if (ready.load(std::memory_order_acquire))
				return true;
			if (failed)
				return false;
			auto fail = [&](const char* why) {
				logger::warn("[VrAriseTail] X4 doorbell failed: {}", why);
				failed = true;
				for (int eye = 0; eye < 2; ++eye) {
					if (hwnd[eye]) {
						DestroyWindow(hwnd[eye]);
						hwnd[eye] = nullptr;
					}
				}
				return false;
			};
			auto& sl = globals::features::upscaling.streamlineDX12;
			if (!dx12 || !dx12->device || !dx12->queue || !dx12->adapter)
				return fail("no Tail D3D12");
			if (!sl.initialized || !sl.featureDLSSG || !sl.slUpgradeInterface)
				return fail("DLSS-G not bound");
			if (sbsWidth < 2 || sbsHeight == 0)
				return fail("SBS too small");

			if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
				fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
			else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
				fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
			const DXGI_FORMAT engineFmt = fmt;
			fmt = DXGI_FORMAT_R8G8B8A8_UNORM;

			WNDCLASSW wc{};
			wc.lpfnWndProc = DefWindowProcW;
			wc.hInstance = GetModuleHandleW(nullptr);
			wc.lpszClassName = L"VrAriseDoorbell";
			RegisterClassW(&wc);
			hwnd[0] = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"VrAriseDoorbell", L"OS",
				WS_POPUP | WS_VISIBLE, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
			if (!hwnd[0])
				return fail("CreateWindowExW");
			SetWindowPos(hwnd[0], HWND_BOTTOM, 0, 0, 8, 8, SWP_NOACTIVATE | SWP_SHOWWINDOW);

			ComPtr<IDXGIFactory4> factory;
			if (FAILED(dx12->adapter->GetParent(IID_PPV_ARGS(&factory))))
				return fail("factory GetParent");
			IDXGIFactory4* factoryRaw = factory.Detach();
			if (sl.slUpgradeInterface(reinterpret_cast<void**>(&factoryRaw)) != sl::Result::eOk) {
				factory.Attach(factoryRaw);
				return fail("factory slUpgradeInterface");
			}
			factory.Attach(factoryRaw);

			DXGI_SWAP_CHAIN_DESC1 desc{};
			desc.Width = sbsWidth;
			desc.Height = sbsHeight;
			desc.Format = fmt;
			desc.SampleDesc.Count = 1;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.BufferCount = 3;
			desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
			ComPtr<IDXGISwapChain1> sc1;
			if (FAILED(factory->CreateSwapChainForHwnd(dx12->queue.Get(), hwnd[0], &desc, nullptr, nullptr, &sc1)))
				return fail("CreateSwapChainForHwnd");
			if (FAILED(sc1.As(&swapChain[0])))
				return fail("Query IDXGISwapChain3");
			IDXGISwapChain3* scRaw = swapChain[0].Detach();
			if (sl.slUpgradeInterface(reinterpret_cast<void**>(&scRaw)) != sl::Result::eOk)
				logger::warn("[VrAriseTail] X4 swapchain slUpgradeInterface failed");
			swapChain[0].Attach(scRaw);
			for (UINT i = 0; i < 3; ++i) {
				if (FAILED(swapChain[0]->GetBuffer(i, IID_PPV_ARGS(&bb[0][i]))))
					return fail("GetBuffer");
				bb[0][i]->SetName(L"VrAriseTail::DoorbellBB");
			}
			ringFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			kickFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			snapshotWaitEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (FAILED(dx12->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
					IID_PPV_ARGS(&hookAlloc[0]))) ||
				FAILED(dx12->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, hookAlloc[0].Get(),
					nullptr, IID_PPV_ARGS(&hookList[0]))) ||
				FAILED(hookList[0]->Close()) ||
				FAILED(dx12->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&assembleAlloc))) ||
				FAILED(dx12->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, assembleAlloc.Get(), nullptr,
					IID_PPV_ARGS(&assembleList))) ||
				FAILED(assembleList->Close()) ||
				FAILED(dx12->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&kickAlloc))) ||
				FAILED(dx12->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, kickAlloc.Get(), nullptr,
					IID_PPV_ARGS(&kickList))) ||
				FAILED(kickList->Close()) ||
				FAILED(dx12->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ringFence))) ||
				FAILED(dx12->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&kickFence))) ||
				!ringFenceEvent || !kickFenceEvent || !snapshotWaitEvent || !CreateRing(sbsWidth, sbsHeight, fmt))
				return fail("ring/hook/kick list");

			void* proxy = swapChain[0].Get();
			void* native = nullptr;
			if (sl.slGetNativeInterface &&
				sl.slGetNativeInterface(proxy, &native) == sl::Result::eOk && native && native != proxy) {
				HookNative(0, static_cast<IDXGISwapChain*>(native));
				logger::info("[VrAriseTail] X4 hooked slGetNativeInterface Present SBS");
			} else {
				HookNative(0, swapChain[0].Get());
				logger::warn("[VrAriseTail] X4 native Present hook on SL chain SBS");
			}

			sbsW = sbsWidth;
			width = sbsWidth;
			height = sbsHeight;
			format = fmt;
			g_doorbell = this;
			ready.store(true, std::memory_order_release);
			if (sl.featureReflex && sl.slReflexSetOptions) {
				sl::ReflexOptions reflex{};
				reflex.mode = sl::ReflexMode::eLowLatency;
				reflex.useMarkersToOptimize = sl.featurePCL;
				reflex.frameLimitUs = 0;
				if (sl.slReflexSetOptions(reflex) == sl::Result::eOk)
					logger::info("[VrAriseTail] X4 Reflex eLowLatency on Tail DX12 (Kick paced {} Hz)",
						static_cast<unsigned>(kAppHz));
				else
					logger::warn("[VrAriseTail] X4 slReflexSetOptions failed");
			}
			logger::info("[VrAriseTail] X4 SBS doorbell {}x{} engineFmt={} RGBA8 one G",
				sbsWidth, sbsHeight, static_cast<unsigned>(engineFmt));
			return true;
		}

		bool EnsureStaged(StagedPair& pair)
		{
			if (pair.eye[0] && pair.eye[0]->GetDesc().Width == width && pair.eye[0]->GetDesc().Height == height)
				return true;
			return CreateTex(pair.eye[0], width, height, format, false, L"VrAriseTail::StagedSbs");
		}

		bool CopyRingToStaged(StagedPair& pair, UINT slotL, UINT slotR, uint64_t fence, uint64_t gen)
		{
			(void)slotR;
			if (!EnsureStaged(pair))
				return false;
			if (!WaitRing(lastAssembleFence) || !WaitRing(fence))
				return false;
			if (FAILED(assembleAlloc->Reset()) || FAILED(assembleList->Reset(assembleAlloc.Get(), nullptr)))
				return false;
			ID3D12Resource* src = ring[0][slotL].Get();
			Transition(assembleList.Get(), src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
			Transition(assembleList.Get(), pair.eye[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			assembleList->CopyResource(pair.eye[0].Get(), src);
			Transition(assembleList.Get(), pair.eye[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
			Transition(assembleList.Get(), src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			if (FAILED(assembleList->Close()))
				return false;
			ID3D12CommandList* lists[]{ assembleList.Get() };
			Gpu()->ExecuteCommandLists(1, lists);
			const uint64_t value = ringFenceValue.fetch_add(1, std::memory_order_relaxed) + 1;
			Gpu()->Signal(ringFence.Get(), value);
			lastAssembleFence = value;
			pair.gen = gen;
			pair.fence = value;
			pair.valid = true;
			return true;
		}

		bool EnqueuePair(StagedPair& src, bool interp)
		{
			if (!src.valid || !src.eye[0])
				return false;
			if (!WaitRing(lastAssembleFence) || !WaitRing(src.fence))
				return false;
			const UINT idx = static_cast<UINT>(pairProduce % kPairSlots);
			auto& dst = pairs[idx];
			if (dst.occupied && dst.seq > pairConsume.load(std::memory_order_acquire)) {
				logger::warn("[VrAriseTail] X4 pair ring full; dropping gen={} interp={}", src.gen, interp ? 1 : 0);
				return false;
			}
			if (!dst.eye[0] || dst.eye[0]->GetDesc().Width != width || dst.eye[0]->GetDesc().Height != height) {
				if (!CreateTex(dst.eye[0], width, height, format, false, L"VrAriseTail::PairSbs"))
					return false;
			}
			if (FAILED(assembleAlloc->Reset()) || FAILED(assembleList->Reset(assembleAlloc.Get(), nullptr)))
				return false;
			Transition(assembleList.Get(), src.eye[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
			Transition(assembleList.Get(), dst.eye[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			assembleList->CopyResource(dst.eye[0].Get(), src.eye[0].Get());
			Transition(assembleList.Get(), dst.eye[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
			Transition(assembleList.Get(), src.eye[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			if (FAILED(assembleList->Close()))
				return false;
			ID3D12CommandList* lists[]{ assembleList.Get() };
			Gpu()->ExecuteCommandLists(1, lists);
			const uint64_t value = ringFenceValue.fetch_add(1, std::memory_order_relaxed) + 1;
			Gpu()->Signal(ringFence.Get(), value);
			lastAssembleFence = value;
			{
				std::lock_guard lock(hookMutex);
				dst.seq = ++pairProduce;
				dst.gen = src.gen;
				dst.fence = value;
				dst.interp = interp;
				dst.occupied = true;
				if (dst.seq <= 16 || (dst.seq % 120) == 0)
					logger::info("[VrAriseTail] X4 enqueue SBS seq={} gen={} kind={}",
						dst.seq, dst.gen, interp ? "interp" : "real");
			}
			return true;
		}

		void FlushPairs()
		{
			StagedPair real{};
			StagedPair interp{};
			{
				std::lock_guard lock(hookMutex);
				if (!lastR.valid || !parkedI.valid || parkedI.gen != lastR.gen + 1)
					return;
				const uint64_t inflight = pairProduce - pairConsume.load(std::memory_order_acquire);
				if (inflight + 2 > kPairSlots)
					return;
				real = lastR;
				interp = parkedI;
				lastR = newestR;
				newestR = {};
				parkedI = {};
			}
			if (!EnqueuePair(real, false) || !EnqueuePair(interp, true))
				logger::warn("[VrAriseTail] X4 flush enqueue failed gen={}", real.gen);
		}

		void OnRealPair(UINT slotL, UINT slotR, uint64_t fence, uint64_t gen)
		{
			StagedPair incoming{};
			if (!CopyRingToStaged(incoming, slotL, slotR, fence, gen))
				return;
			StagedPair lone{};
			{
				std::lock_guard lock(hookMutex);
				if (!lastR.valid) {
					lastR = incoming;
					if (!gEnabled)
						lone = incoming;
				} else if (incoming.gen > lastR.gen) {
					newestR = incoming;
					if (!gEnabled) {
						lone = incoming;
						lastR = incoming;
						newestR = {};
					} else if (incoming.gen > lastR.gen + 1) {
						lone = lastR;
						lastR = incoming;
						newestR = {};
					}
				}
			}
			if (lone.valid)
				EnqueuePair(lone, false);
			FlushPairs();
		}

		void OnInterpPair(UINT slotL, UINT slotR, uint64_t fence, uint64_t gen)
		{
			StagedPair incoming{};
			if (!CopyRingToStaged(incoming, slotL, slotR, fence, gen))
				return;
			{
				std::lock_guard lock(hookMutex);
				parkedI = incoming;
			}
			FlushPairs();
		}

		bool PushPending(int eye, uint64_t gen, uint32_t remaining)
		{
			if (pendW[eye] - pendR[eye] >= kPending) {
				++pendR[eye];
				static uint32_t dropped = 0;
				if ((++dropped % 120) == 1)
					logger::warn("[VrAriseTail] X4 pending Present overflow eye={} gen={}", eye, gen);
			}
			pending[eye][pendW[eye] % kPending] = { gen, remaining };
			++pendW[eye];
			return true;
		}

		bool ClassifyPresent(int eye, uint64_t& gen, bool& interp)
		{
			if (pendR[eye] == pendW[eye])
				return false;
			auto& p = pending[eye][pendR[eye] % kPending];
			gen = p.gen;
			if (p.remaining >= 2) {
				interp = true;
				p.remaining = 1;
				return true;
			}
			interp = false;
			++pendR[eye];
			return true;
		}

		GenRec* RecFor(uint64_t gen)
		{
			for (auto& rec : genRecs) {
				if (rec.inUse && rec.gen == gen)
					return &rec;
			}
			for (auto& rec : genRecs) {
				if (!rec.inUse) {
					rec = {};
					rec.gen = gen;
					rec.inUse = true;
					return &rec;
				}
			}
			GenRec* oldest = &genRecs[0];
			for (auto& rec : genRecs) {
				if (rec.gen < oldest->gen)
					oldest = &rec;
			}
			*oldest = {};
			oldest->gen = gen;
			oldest->inUse = true;
			return oldest;
		}

		void AssembleReadyPairs()
		{
			struct Job
			{
				bool interp = false;
				UINT slotL = 0;
				UINT slotR = 0;
				uint64_t fence = 0;
				uint64_t gen = 0;
			};
			Job jobs[kGenRecs * 2]{};
			UINT n = 0;
			{
				std::lock_guard lock(hookMutex);
				for (auto& rec : genRecs) {
					if (!rec.inUse)
						continue;
					if (rec.haveI[0] && n < kGenRecs * 2) {
						jobs[n++] = { true, rec.slotI[0], rec.slotI[0], rec.fenceI[0], rec.gen };
						rec.haveI[0] = rec.haveI[1] = false;
					}
					if (rec.haveR[0] && n < kGenRecs * 2) {
						jobs[n++] = { false, rec.slotR[0], rec.slotR[0], rec.fenceR[0], rec.gen };
						rec.haveR[0] = rec.haveR[1] = false;
					}
					if (!rec.haveI[0] && !rec.haveI[1] && !rec.haveR[0] && !rec.haveR[1])
						rec.inUse = false;
				}
			}
			for (UINT i = 0; i < n; ++i) {
				if (jobs[i].interp)
					OnInterpPair(jobs[i].slotL, jobs[i].slotR, jobs[i].fence, jobs[i].gen);
				else
					OnRealPair(jobs[i].slotL, jobs[i].slotR, jobs[i].fence, jobs[i].gen);
			}
		}

		void OnNativePresent(int eye, IDXGISwapChain* self)
		{
			if (!ready.load(std::memory_order_acquire) || !self || eye != 0)
				return;
			std::lock_guard lock(hookMutex);
			if (!WaitRing(lastHookFence[eye]))
				return;
			ComPtr<IDXGISwapChain3> sc3;
			if (FAILED(self->QueryInterface(IID_PPV_ARGS(&sc3))))
				return;
			const UINT index = sc3->GetCurrentBackBufferIndex();
			ComPtr<ID3D12Resource> src;
			if (FAILED(sc3->GetBuffer(index, IID_PPV_ARGS(&src))) || !src)
				return;
			if (FAILED(hookAlloc[eye]->Reset()) || FAILED(hookList[eye]->Reset(hookAlloc[eye].Get(), nullptr)))
				return;
			UINT slot = writeSlot[eye];
			if (slot == holdSlot)
				slot = (slot + 1) % kRingSlots;
			writeSlot[eye] = (slot + 1) % kRingSlots;
			Transition(hookList[eye].Get(), src.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
			Transition(hookList[eye].Get(), ring[eye][slot].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			hookList[eye]->CopyResource(ring[eye][slot].Get(), src.Get());
			Transition(hookList[eye].Get(), ring[eye][slot].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
			Transition(hookList[eye].Get(), src.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			if (FAILED(hookList[eye]->Close()))
				return;
			ID3D12CommandList* lists[]{ hookList[eye].Get() };
			Gpu()->ExecuteCommandLists(1, lists);
			const uint64_t value = ringFenceValue.fetch_add(1, std::memory_order_relaxed) + 1;
			Gpu()->Signal(ringFence.Get(), value);
			lastHookFence[eye] = value;
			slotFence[eye][slot] = value;
			slotSeq[slot] = ++produceSeq;
			++nativePresents;
			if (nativePresents <= 8 || (nativePresents % 120) == 0)
				logger::info("[VrAriseTail] X4 native Present #{} slot={} seq={}",
					nativePresents, slot, slotSeq[slot]);
		}

		bool Snapshot(ID3D12Resource* color, UINT w, UINT h, DXGI_FORMAT fmt)
		{
			if (!color || !dx12 || !dx12->device || !dx12->guidesReady || !dx12->depth || !dx12->mvec)
				return false;
			if (w < 2 || h == 0 || dx12->guideWidth < 2 || dx12->guideHeight == 0)
				return false;
			constexpr DXGI_FORMAT kDoorbellFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
			if (!CreateKeep(w, h, kDoorbellFmt) ||
				!CreateGuideKeep(dx12->guideWidth, dx12->guideHeight, dx12->depthFormat, dx12->mvecFormat))
				return false;
			if (fmt != kDoorbellFmt)
				return false;
			const UINT dest = 1u - kickSlot.load(std::memory_order_acquire);
			if (!dx12->WaitFenceTimeout(dx12->fenceValue, kFenceWaitMs))
				return false;
			if (FAILED(dx12->allocator->Reset()) || FAILED(dx12->list->Reset(dx12->allocator.Get(), nullptr)))
				return false;
			if (dx12->colorState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
				Transition(dx12->list.Get(), color, dx12->colorState, D3D12_RESOURCE_STATE_COPY_SOURCE);
				dx12->colorState = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			if (keepState[0][dest] != D3D12_RESOURCE_STATE_COPY_DEST) {
				Transition(dx12->list.Get(), hudlessKeep[0][dest].Get(), keepState[0][dest],
					D3D12_RESOURCE_STATE_COPY_DEST);
				keepState[0][dest] = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			dx12->list->CopyResource(hudlessKeep[0][dest].Get(), color);
			keepHasGuides[0][dest] = false;
			if (keepDepthState[0][dest] != D3D12_RESOURCE_STATE_COPY_DEST) {
				Transition(dx12->list.Get(), keepDepth[0][dest].Get(), keepDepthState[0][dest],
					D3D12_RESOURCE_STATE_COPY_DEST);
				keepDepthState[0][dest] = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			if (keepMvecState[0][dest] != D3D12_RESOURCE_STATE_COPY_DEST) {
				Transition(dx12->list.Get(), keepMvec[0][dest].Get(), keepMvecState[0][dest],
					D3D12_RESOURCE_STATE_COPY_DEST);
				keepMvecState[0][dest] = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			if (dx12->depthState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
				Transition(dx12->list.Get(), dx12->depth.Get(), dx12->depthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
				dx12->depthState = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			if (dx12->mvecState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
				Transition(dx12->list.Get(), dx12->mvec.Get(), dx12->mvecState, D3D12_RESOURCE_STATE_COPY_SOURCE);
				dx12->mvecState = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			dx12->list->CopyResource(keepDepth[0][dest].Get(), dx12->depth.Get());
			dx12->list->CopyResource(keepMvec[0][dest].Get(), dx12->mvec.Get());
			Transition(dx12->list.Get(), hudlessKeep[0][dest].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_COMMON);
			keepState[0][dest] = D3D12_RESOURCE_STATE_COMMON;
			Transition(dx12->list.Get(), keepDepth[0][dest].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_COMMON);
			keepDepthState[0][dest] = D3D12_RESOURCE_STATE_COMMON;
			Transition(dx12->list.Get(), keepMvec[0][dest].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_COMMON);
			keepMvecState[0][dest] = D3D12_RESOURCE_STATE_COMMON;
			keepHasGuides[0][dest] = true;
			Transition(dx12->list.Get(), color, dx12->colorState, D3D12_RESOURCE_STATE_COMMON);
			dx12->colorState = D3D12_RESOURCE_STATE_COMMON;
			if (dx12->depthState != D3D12_RESOURCE_STATE_COMMON) {
				Transition(dx12->list.Get(), dx12->depth.Get(), dx12->depthState, D3D12_RESOURCE_STATE_COMMON);
				dx12->depthState = D3D12_RESOURCE_STATE_COMMON;
			}
			if (dx12->mvecState != D3D12_RESOURCE_STATE_COMMON) {
				Transition(dx12->list.Get(), dx12->mvec.Get(), dx12->mvecState, D3D12_RESOURCE_STATE_COMMON);
				dx12->mvecState = D3D12_RESOURCE_STATE_COMMON;
			}
			if (FAILED(dx12->list->Close()))
				return false;
			ID3D12CommandList* lists[]{ dx12->list.Get() };
			dx12->queue->ExecuteCommandLists(1, lists);
			++dx12->fenceValue;
			dx12->queue->Signal(dx12->fence.Get(), dx12->fenceValue);
			keepFenceValue[dest] = dx12->fenceValue;
			publishedKeep = dest;
			return true;
		}

		void WorkerLoop()
		{
			SetThreadDescription(GetCurrentThread(), L"VrAriseDoorbell");
			while (!stop.load(std::memory_order_acquire)) {
				MSG msg{};
				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
					DispatchMessageW(&msg);
				if (!ready.load(std::memory_order_acquire) && !failed && pendingKick.load(std::memory_order_acquire))
					Ensure(reqW, reqH, reqFmt);
				if (ready.load(std::memory_order_acquire) && pendingKick.load(std::memory_order_acquire)) {
					Kick();
					pendingKick.store(false, std::memory_order_release);
				}
				std::unique_lock lock(kickMutex);
				kickCv.wait_for(lock, std::chrono::milliseconds(2), [&] {
					return stop.load(std::memory_order_acquire) || pendingKick.load(std::memory_order_acquire);
				});
			}
			{
				std::lock_guard hookLock(hookMutex);
				for (int eye = 0; eye < 2; ++eye) {
					swapChain[eye].Reset();
					nativeSc[eye] = nullptr;
					for (auto& buffer : bb[eye])
						buffer.Reset();
				}
			}
			for (int eye = 0; eye < 2; ++eye) {
				if (hwnd[eye]) {
					DestroyWindow(hwnd[eye]);
					hwnd[eye] = nullptr;
				}
			}
		}

		void OfferKick(ID3D12Resource* color, UINT w, UINT h, DXGI_FORMAT fmt)
		{
			reqW = w;
			reqH = h;
			reqFmt = fmt;
			if (!Snapshot(color, w, h, fmt))
				return;
			pendingKick.store(true, std::memory_order_release);
			if (!worker.joinable()) {
				worker = std::thread([this] { WorkerLoop(); });
				logger::info("[VrAriseTail] X4 doorbell worker started");
			}
			kickCv.notify_one();
		}

		bool Kick()
		{
			if (!ready.load(std::memory_order_acquire) || !swapChain[0] || !dx12)
				return false;
			auto& sl = globals::features::upscaling.streamlineDX12;
			sl::Constants constants{};
			{
				std::lock_guard lock(sharedMutex);
				if (!sharedConstantsValid) {
					static bool logged = false;
					if (!logged) {
						logger::error("[VrAriseTail] X4 Kick refused: no game-thread constants");
						logged = true;
					}
					return false;
				}
				constants = sharedConstants[0];
			}
			sl::FrameToken* token = nullptr;
			if (!sl.AcquireFrameToken(++kickFrameId, token) || !token) {
				static bool logged = false;
				if (!logged) {
					logger::error("[VrAriseTail] X4 Kick refused: slGetNewFrameToken failed");
					logged = true;
				}
				return false;
			}
			if (sl.slReflexSleep)
				sl.slReflexSleep(*token);
			if (lastKickQpc.QuadPart)
				PaceUntil(lastKickQpc, kAppHz);
			QueryPerformanceCounter(&lastKickQpc);
			const sl::ViewportHandle vp = sl.viewport;
			const bool enableG = kicks >= 1;
			gEnabled = enableG;
			const UINT slot = publishedKeep;
			if (!hudlessKeep[0][slot] || !keepHasGuides[0][slot] || !keepDepth[0][slot] || !keepMvec[0][slot])
				return false;
			if (!WaitKick())
				return false;
			kickSlot.store(slot, std::memory_order_release);
			if (keepFenceValue[slot] &&
				!WaitFenceCpu(dx12->fence.Get(), snapshotWaitEvent, keepFenceValue[slot], kFenceWaitMs))
				return false;
			if (FAILED(kickAlloc->Reset()) || FAILED(kickList->Reset(kickAlloc.Get(), nullptr)))
				return false;
			if (sl.featurePCL)
				sl.EmitPCLMarker(sl::PCLMarker::eSimulationStart, *token);
			if (!sl.SetConstants(*token, constants, vp)) {
				static bool logged = false;
				if (!logged) {
					logger::error("[VrAriseTail] X4 Kick refused: slSetConstants SBS");
					logged = true;
				}
				kickList->Close();
				return false;
			}
			const UINT index = swapChain[0]->GetCurrentBackBufferIndex();
			if (!bb[0][index])
				return false;
			ID3D12Resource* hudless = hudlessKeep[0][slot].Get();
			if (keepState[0][slot] != D3D12_RESOURCE_STATE_COPY_SOURCE) {
				Transition(kickList.Get(), hudless, keepState[0][slot], D3D12_RESOURCE_STATE_COPY_SOURCE);
				keepState[0][slot] = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			Transition(kickList.Get(), bb[0][index].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			kickList->CopyResource(bb[0][index].Get(), hudless);
			Transition(kickList.Get(), bb[0][index].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
			Transition(kickList.Get(), hudless, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			keepState[0][slot] = D3D12_RESOURCE_STATE_COMMON;
			if (!sl.TagDX12Resources(*token, kickList.Get(), keepDepth[0][slot].Get(),
					keepMvec[0][slot].Get(), hudless, nullptr, keepGuideW, keepGuideH, width, height,
					vp)) {
				static bool logged = false;
				if (!logged) {
					logger::error("[VrAriseTail] X4 Kick refused: TagDX12Resources SBS");
					logged = true;
				}
				kickList->Close();
				return false;
			}
			sl.ConfigureDLSSG(vp, enableG, keepGuideW, keepGuideH, width, height,
				keepDepthFmt, keepMvecFmt, format, DXGI_FORMAT_UNKNOWN, 3);
			if (sl.featurePCL) {
				sl.EmitPCLMarker(sl::PCLMarker::eSimulationEnd, *token);
				sl.EmitPCLMarker(sl::PCLMarker::eRenderSubmitStart, *token);
			}
			if (FAILED(kickList->Close()))
				return false;
			ID3D12CommandList* lists[]{ kickList.Get() };
			Gpu()->ExecuteCommandLists(1, lists);
			++kickFenceValue;
			Gpu()->Signal(kickFence.Get(), kickFenceValue);
			if (!WaitKick())
				return false;
			if (sl.featurePCL) {
				sl.EmitPCLMarker(sl::PCLMarker::eRenderSubmitEnd, *token);
				sl.EmitPCLMarker(sl::PCLMarker::ePresentStart, *token);
			}
			const HRESULT hr = swapChain[0]->Present(0, DXGI_PRESENT_ALLOW_TEARING);
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
				logger::warn("[VrAriseTail] X4 doorbell Present failed hr={:08X}", static_cast<unsigned>(hr));
				return false;
			}
			++kicks;
			if (!firstPresentDone) {
				firstPresentDone = true;
				logger::info("[VrAriseTail] X4 first SBS Present (G off)");
			} else if (enableG && !gOnLogged) {
				gOnLogged = true;
				logger::info("[VrAriseTail] X4 G on after warmup kick={} guide {}x{} color {}x{}",
					kicks, keepGuideW, keepGuideH, width, height);
			}
			if (kicks <= 8 || (kicks % 120) == 0)
				logger::info("[VrAriseTail] X4 G kick={} status={} presented={} produce={}",
					kicks, magic_enum::enum_name(sl.lastDLSSGStatus), sl.lastDLSSGFramesPresented,
					produceSeq.load(std::memory_order_acquire));
			return true;
		}

		bool PeekNext(ID3D12Resource*& out, uint64_t& seq)
		{
			out = nullptr;
			seq = 0;
			if (!ready.load(std::memory_order_acquire) || !ringFence)
				return false;
			std::lock_guard lock(hookMutex);
			const uint64_t completed = ringFence->GetCompletedValue();
			const uint64_t next = consumeSeq.load(std::memory_order_acquire) + 1;
			uint64_t oldest = UINT64_MAX;
			UINT oldestSlot = kRingSlots;
			for (UINT i = 0; i < kRingSlots; ++i) {
				if (slotSeq[i] < next || completed < slotFence[0][i])
					continue;
				if (slotSeq[i] < oldest) {
					oldest = slotSeq[i];
					oldestSlot = i;
				}
			}
			if (oldestSlot >= kRingSlots)
				return false;
			out = ring[0][oldestSlot].Get();
			seq = oldest;
			holdSlot = oldestSlot;
			return out != nullptr;
		}

		void Consume(uint64_t seq)
		{
			std::lock_guard lock(hookMutex);
			consumeSeq.store(seq, std::memory_order_release);
			holdSlot = kRingSlots;
		}

		void ReleaseHold()
		{
			std::lock_guard lock(hookMutex);
			holdSlot = kRingSlots;
		}

		bool PeekNextPair(ID3D12Resource*& sbs, uint64_t& seq, uint64_t& gen,
			bool& interp, uint64_t& fence)
		{
			sbs = nullptr;
			seq = 0;
			gen = 0;
			interp = false;
			fence = 0;
			if (!ready.load(std::memory_order_acquire) || !ringFence)
				return false;
			std::lock_guard lock(hookMutex);
			const uint64_t completed = ringFence->GetCompletedValue();
			const uint64_t next = pairConsume.load(std::memory_order_acquire) + 1;
			for (auto& pair : pairs) {
				if (!pair.occupied || pair.seq != next || completed < pair.fence)
					continue;
				sbs = pair.eye[0].Get();
				seq = pair.seq;
				gen = pair.gen;
				interp = pair.interp;
				fence = pair.fence;
				return sbs != nullptr;
			}
			return false;
		}

		ID3D12Fence* RingFence() { return ringFence.Get(); }

		void ConsumePair(uint64_t seq)
		{
			std::lock_guard lock(hookMutex);
			for (auto& pair : pairs) {
				if (pair.occupied && pair.seq == seq)
					pair.occupied = false;
			}
			pairConsume.store(seq, std::memory_order_release);
		}
	};

	void ResolveSize(std::uint32_t& width, std::uint32_t& height)
	{
		width = 1512;
		height = 1512;
	}

	void TailLoop()
	{
		SetThreadDescription(GetCurrentThread(), L"VrAriseTail");
		timeBeginPeriod(1);
		tailThreadId.store(GetCurrentThreadId(), std::memory_order_release);
		Dx12Path dx12;
		DeviceBPath deviceB;
		Doorbell doorbell;
		doorbell.dx12 = &dx12;
		enum class Path
		{
			kNone,
			kDx12,
			kDeviceB
		};
		Path path = Path::kNone;
		bool loggedPath = false;
		bool loggedWaitOk = false;
		bool submitRing = false;
		std::uint32_t frame = 0;
		std::uint32_t waitCount = 0;
		std::uint32_t ingestCount = 0;
		VrAriseTail::Status local{};
		local.active = true;
		local.path = "pending";

		while (running.load(std::memory_order_acquire) && !abandoned.load(std::memory_order_acquire)) {
			if (path == Path::kNone) {
				if (dx12.CreateDevice() && dx12.device) {
					path = Path::kDx12;
					local.path = "dx12";
				} else if (deviceB.CreateDevice()) {
					path = Path::kDeviceB;
					local.path = "deviceB";
					logger::warn("[VrAriseTail] DX12 device failed; using Device B");
				} else {
					Sleep(50);
					continue;
				}
			}

			if ((path == Path::kDx12 && !dx12.color) || (path == Path::kDeviceB && !deviceB.color)) {
				std::uint32_t w = 0;
				std::uint32_t h = 0;
				ResolveSize(w, h);
				const bool ok = path == Path::kDx12 ? dx12.CreateColor(w, h) : deviceB.CreateColor(w, h);
				if (!ok) {
					logger::warn("[VrAriseTail] color target {}x{} failed on {}", w, h, local.path);
					Sleep(50);
					continue;
				}
				logger::info("[VrAriseTail] color target {}x{} path={}", w, h, local.path);
			}

			auto* compositor = RE::BSOpenVR::GetIVRCompositor();
			if (!compositor) {
				Sleep(100);
				continue;
			}
			HookCompositorPoses(compositor);

			vr::TrackedDevicePose_t renderPoses[vr::k_unMaxTrackedDeviceCount]{};
			vr::TrackedDevicePose_t gamePoses[vr::k_unMaxTrackedDeviceCount]{};
			LARGE_INTEGER start{};
			LARGE_INTEGER end{};
			LARGE_INTEGER freq{};
			QueryPerformanceFrequency(&freq);
			vr::EVRCompositorError waitErr = vr::VRCompositorError_None;
			{
				std::lock_guard lock(vrLoop);
				if (!tailOwnsLoop.load(std::memory_order_acquire))
					tailOwnsLoop.store(true, std::memory_order_release);
				QueryPerformanceCounter(&start);
				waitErr = compositor->WaitGetPoses(renderPoses, vr::k_unMaxTrackedDeviceCount,
					gamePoses, vr::k_unMaxTrackedDeviceCount);
				QueryPerformanceCounter(&end);
			}
			CachePoses(renderPoses, gamePoses);
			if (waitErr == vr::VRCompositorError_None &&
				renderPoses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
				poseRing.Push(renderPoses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking,
					waitCount, end.QuadPart);
			Util::DisableConsoleQuickEdit();
			if (waitErr == vr::VRCompositorError_None) {
				++hmdCycles;
				if ((hmdCycles % 2) == 0)
					ReleaseEngineFrame();
			}
			++waitCount;
			LARGE_INTEGER paced{};
			QueryPerformanceCounter(&paced);
			const double waitMs = freq.QuadPart ?
				(static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)) :
				0.0;
			const double pacedMs = freq.QuadPart ?
				(static_cast<double>(paced.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)) :
				0.0;
			local.lastWaitMs = waitMs;
			local.lastWaitErr = static_cast<int>(waitErr);
			if (waitErr != vr::VRCompositorError_None) {
				logger::error("[VrAriseTail] WaitGetPoses err={}", static_cast<int>(waitErr));
				if (path == Path::kDx12 && dx12.device) {
					const HRESULT removed = dx12.device->GetDeviceRemovedReason();
					if (FAILED(removed))
						logger::error("[VrAriseTail] X4 device removed hr={:08X}", static_cast<unsigned>(removed));
				}
				Sleep(10);
				PublishStatus(local);
				continue;
			}
			if (!loggedWaitOk) {
				logger::info("[VrAriseTail] Tail WaitGetPoses ok");
				loggedWaitOk = true;
			}
			if (waitErr == vr::VRCompositorError_None && (hmdCycles % 2) == 0) {
				static bool loggedThrottle = false;
				if (!loggedThrottle) {
					logger::info("[VrAriseTail] X3 release engine after {} HMD cycles", hmdCycles);
					loggedThrottle = true;
				}
			} else if (waitCount <= 8 || (waitCount % 120) == 0)
				logger::info("[VrAriseTail] WaitGetPoses {:.2f} ms paced={:.2f} err={} n={} path={}",
					waitMs, pacedMs, static_cast<int>(waitErr), waitCount, local.path);

			if (path == Path::kDx12) {
				const SharedPublish published = SnapshotShared();
				bool prepared = false;
				if (published.fenceValue > dx12.lastConsumed) {
					if (dx12.PrepareShared(published, !submitRing)) {
						if (!submitRing)
							local.path = "dx12-ktemp";
						prepared = true;
						static bool loggedShared = false;
						if (!loggedShared) {
							logger::info("[VrAriseTail] X2 shared Submit {}x{} eye={}x{} fence={} slot={} guides={} {}x{}",
								published.width, published.height, published.width / 2, published.height,
								published.fenceValue, published.slot, dx12.guidesReady ? 1 : 0,
								dx12.guideWidth, dx12.guideHeight);
							loggedShared = true;
						}
					} else {
						static bool loggedSharedFail = false;
						if (!loggedSharedFail) {
							logger::error("[VrAriseTail] X2 PrepareShared failed");
							loggedSharedFail = true;
						}
					}
				}
				if (!dx12.HasSubmitEyes()) {
					dx12.sbsBounds = false;
					dx12.Paint(frame);
				}
				if (kFrameGenerationDoorbell && loggedPath && prepared && dx12.color && dx12.guidesReady)
					doorbell.OfferKick(dx12.color.Get(), published.width, published.height, published.format);
				if (kFrameGenerationDoorbell && loggedPath && doorbell.ready.load(std::memory_order_acquire)) {
					ID3D12Resource* generated = nullptr;
					uint64_t seq = 0;
					if (doorbell.PeekNext(generated, seq)) {
						if (dx12.IngestSbs(generated, doorbell.width, doorbell.height, doorbell.format)) {
							const uint64_t expected = doorbell.consumeSeq.load(std::memory_order_acquire) + 1;
							if (seq > expected)
								logger::warn("[VrAriseTail] X4 ring dropped {} presents ({} -> {})",
									seq - expected, expected, seq);
							doorbell.Consume(seq);
							submitRing = true;
							local.path = "dx12-g";
							++ingestCount;
							if (ingestCount == 1)
								logger::info("[VrAriseTail] X4 IngestSbs ring seq={}", seq);
							if (ingestCount <= 16 || (ingestCount % 120) == 0)
								logger::info("[VrAriseTail] X4 ingest SBS seq={} ingest={}", seq, ingestCount);
						} else
							doorbell.ReleaseHold();
					}
				}
			} else
				deviceB.Paint(frame);
			++frame;

			vr::EVRCompositorError errL = vr::VRCompositorError_None;
			vr::EVRCompositorError errR = vr::VRCompositorError_None;
			LARGE_INTEGER submitStart{};
			LARGE_INTEGER submitEnd{};
			QueryPerformanceCounter(&submitStart);
			if (path == Path::kDx12)
				dx12.SubmitEyes(compositor, errL, errR);
			else
				deviceB.SubmitEyes(compositor, errL, errR);
			QueryPerformanceCounter(&submitEnd);

			local.lastSubmitL = static_cast<int>(errL);
			local.lastSubmitR = static_cast<int>(errR);
			if (errL == vr::VRCompositorError_None && errR == vr::VRCompositorError_None)
				++local.submits;
			if (!loggedPath) {
				if (path == Path::kDx12) {
					logger::info("[VrAriseTail] Tail first Submit dx12 err={} ({}) R={} ({})",
						static_cast<int>(errL), CompositorErrorName(errL),
						static_cast<int>(errR), CompositorErrorName(errR));
				} else {
					logger::info("[VrAriseTail] Tail first Submit {} err={} ({}) R={} ({})",
						local.path,
						static_cast<int>(errL), CompositorErrorName(errL),
						static_cast<int>(errR), CompositorErrorName(errR));
				}
				const double submitMs = freq.QuadPart ?
					(static_cast<double>(submitEnd.QuadPart - submitStart.QuadPart) * 1000.0 /
						static_cast<double>(freq.QuadPart)) :
					0.0;
				logger::info("[VrAriseTail] first Submit blocked {:.0f} ms", submitMs);
				loggedPath = true;
				SignalFirstOwnedSubmit();
			}
			if (path == Path::kDx12 &&
				(errL == vr::VRCompositorError_InvalidTexture || errR == vr::VRCompositorError_InvalidTexture) &&
				deviceB.CreateDevice() && deviceB.CreateColor(dx12.width, dx12.height)) {
				logger::warn("[VrAriseTail] Device A InvalidTexture; switching to Device B");
				path = Path::kDeviceB;
				local.path = "deviceB";
			}
			QueryPerformanceCounter(&paced);
			local.lastWaitMs = waitMs;
			if (waitCount <= 8 || (waitCount % 120) == 0)
				logger::info("[VrAriseTail] beat wait={:.2f} ms (need WaitGetPoses, not sleep) n={} path={} hmdHz={:.0f}",
					waitMs, waitCount, local.path, kHmdHz);
			PublishStatus(local);
		}
		if (!firstOwnedSubmit.load(std::memory_order_acquire))
			AbandonTakeover();
		timeEndPeriod(1);
	}

	void StartWorker()
	{
		if (running.exchange(true))
			return;
		active.store(true, std::memory_order_release);
		VrAriseTail::Status pending{};
		pending.active = true;
		pending.path = "pending";
		PublishStatus(pending);
		logger::info("[VrAriseTail] Tail thread started");
		worker = std::thread(TailLoop);
		worker.detach();
	}
#endif
}

void VrAriseTail::Install()
{
#ifdef ENABLE_SKYRIM_VR
	if (!globals::game::isVR)
		return;
	if (!installed.exchange(true))
		InstallUpdatePosesHook();
#endif
}

bool VrAriseTail::IsActive()
{
#ifdef ENABLE_SKYRIM_VR
	return !abandoned.load(std::memory_order_acquire) &&
		(active.load(std::memory_order_acquire) || BootLatchedArise());
#else
	return false;
#endif
}

void VrAriseTail::NoteEngineSubmit()
{
#ifdef ENABLE_SKYRIM_VR
	if (!BootLatchedArise() || abandoned.load(std::memory_order_acquire))
		return;
	if (vrReady.exchange(true, std::memory_order_acq_rel))
		return;
	logger::info("[VrAriseTail] vrReady on first engine Submit");
	StartWorker();
#endif
}

bool VrAriseTail::ShouldSwallowEngineSubmit()
{
#ifdef ENABLE_SKYRIM_VR
	if (!BootLatchedArise() || abandoned.load(std::memory_order_acquire))
		return false;
	NoteEngineSubmit();
	if (firstOwnedSubmit.load(std::memory_order_acquire))
		return true;
	std::unique_lock lock(firstSubmitMutex);
	while (!firstOwnedSubmit.load(std::memory_order_acquire) &&
		!abandoned.load(std::memory_order_acquire)) {
		if (!running.load(std::memory_order_acquire)) {
			logger::error("[VrAriseTail] Tail exited before first Submit; abandoning takeover");
			AbandonTakeover();
			return false;
		}
		if (!firstSubmitCv.wait_for(lock, std::chrono::seconds(5), [] {
				return firstOwnedSubmit.load(std::memory_order_acquire) ||
					abandoned.load(std::memory_order_acquire);
			})) {
			logger::warn("[VrAriseTail] still waiting for Tail first Submit (OC session bind)");
		}
	}
	return firstOwnedSubmit.load(std::memory_order_acquire) &&
		!abandoned.load(std::memory_order_acquire);
#else
	return false;
#endif
}

void VrAriseTail::PublishKTemp(ID3D11Texture2D* kTemp)
{
#ifdef ENABLE_SKYRIM_VR
	if (!kTemp || !BootLatchedArise() || abandoned.load(std::memory_order_acquire))
		return;
	if (globals::state &&
		(globals::state->isLoadingMenuOpen ||
			(globals::state->IsMainOrLoadingMenuOpen() && !globals::state->worldRenderedThisFrame))) {
		if (!publishPausedForLoad.exchange(true))
			logger::info("[VrAriseTail] X4 publish paused (loading/no world)");
		publishResumeQpc.store(0, std::memory_order_release);
		return;
	}
	D3D11_TEXTURE2D_DESC desc{};
	kTemp->GetDesc(&desc);
	if (!desc.Width || !desc.Height)
		return;

	ComPtr<ID3D11Device5> device5;
	ComPtr<ID3D11DeviceContext4> context4;
	if (!globals::d3d::device || !globals::d3d::context ||
		FAILED(globals::d3d::device->QueryInterface(IID_PPV_ARGS(&device5))) ||
		FAILED(globals::d3d::context->QueryInterface(IID_PPV_ARGS(&context4)))) {
		if (!loggedPublishFail.exchange(true))
			logger::warn("[VrAriseTail] X2 D3D11 fence interfaces unavailable");
		return;
	}

	CS_GPU_PASS("VrAriseTail::PublishKTemp");

	UINT guideW = 0;
	UINT guideH = 0;
	DXGI_FORMAT mvecFmt = DXGI_FORMAT_UNKNOWN;
	ID3D11Texture2D* mvecSrc = nullptr;
	ID3D11ShaderResourceView* depthSrv = nullptr;
	if (globals::game::renderer) {
		auto& depth = globals::game::renderer->GetDepthStencilData()
						  .depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto& motion = globals::game::renderer->GetRuntimeData()
						   .renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
		ComPtr<ID3D11Texture2D> depthTex;
		ComPtr<ID3D11Texture2D> mvecTex;
		if (depth.texture)
			depth.texture->QueryInterface(IID_PPV_ARGS(&depthTex));
		if (motion.texture)
			motion.texture->QueryInterface(IID_PPV_ARGS(&mvecTex));
		D3D11_TEXTURE2D_DESC depthDesc{};
		D3D11_TEXTURE2D_DESC mvecDesc{};
		if (depthTex && mvecTex && depth.depthSRV) {
			depthTex->GetDesc(&depthDesc);
			mvecTex->GetDesc(&mvecDesc);
			if (depthDesc.Width && depthDesc.Height &&
				depthDesc.Width == mvecDesc.Width && depthDesc.Height == mvecDesc.Height) {
				guideW = depthDesc.Width;
				guideH = depthDesc.Height;
				mvecFmt = mvecDesc.Format;
				mvecSrc = mvecTex.Get();
				depthSrv = depth.depthSRV;
			}
		}
	}
	if (!guideW || !guideH || !mvecSrc || !depthSrv) {
		if (!loggedPublishFail.exchange(true))
			logger::error("[VrAriseTail] X4 publish refused: missing real depth/mvec");
		return;
	}
	const bool resumeReset = publishPausedForLoad.exchange(false);
	if (resumeReset || publishResumeQpc.load(std::memory_order_acquire) == 0) {
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		publishResumeQpc.store(now.QuadPart, std::memory_order_release);
	}
	if (resumeReset)
		logger::info("[VrAriseTail] X4 publish resumed; G reset");
	sl::Constants constants[2]{};
	for (uint32_t eye = 0; eye < 2; ++eye) {
		if (!globals::features::upscaling.streamlineDX12.FillConstants(constants[eye], eye, resumeReset, false)) {
			if (!loggedPublishFail.exchange(true))
				logger::error("[VrAriseTail] X4 publish refused: FillConstants eye={}", eye);
			return;
		}
		constants[eye].mvecScale = { 1.0f, 1.0f };
	}
	if (globals::state && globals::state->screenSize.y > 0.0f)
		constants[0].cameraAspectRatio = globals::state->screenSize.x / globals::state->screenSize.y;
	static bool loggedGConstants = false;
	if (!loggedGConstants) {
		loggedGConstants = true;
		logger::info("[VrAriseTail] X4 SBS G constants jitter=({:.3f},{:.3f}) color={}x{} aspect={:.3f} fmt={} ({}) guide={}x{}",
			constants[0].jitterOffset.x, constants[0].jitterOffset.y,
			desc.Width, desc.Height, constants[0].cameraAspectRatio,
			static_cast<unsigned>(desc.Format),
			desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? "RGBA8" :
				desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? "RGB10A2" : "other",
			guideW, guideH);
	}

	const UINT64 frame = globals::state ? globals::state->frameCount : 0;
	if (frame && publishedThisFrame.exchange(frame) == frame)
		return;

	std::lock_guard lock(sharedMutex);
	if (!EnsureSharedSlots(device5.Get(), desc.Width, desc.Height, desc.Format, guideW, guideH, mvecFmt)) {
		if (!loggedPublishFail.exchange(true))
			logger::warn("[VrAriseTail] X2 shared slot create failed {}x{} fmt={}",
				desc.Width, desc.Height, static_cast<unsigned>(desc.Format));
		return;
	}
	const UINT slot = static_cast<UINT>(sharedFenceValue % kSharedSlots);
	if (!sharedHasGuides || !sharedDepthRtv[slot] || !sharedMvec11[slot]) {
		if (!loggedPublishFail.exchange(true))
			logger::error("[VrAriseTail] X4 publish refused: shared guides not created");
		return;
	}
	context4->CopyResource(sharedTex11[slot].Get(), kTemp);
	context4->CopyResource(sharedMvec11[slot].Get(), mvecSrc);
	auto& upscaling = globals::features::upscaling;
	auto* vs = upscaling.GetUpscaleVS();
	auto* ps = upscaling.copyDepthToSharedBufferPS.Get(
		L"Data\\Shaders\\Upscaling\\CopyDepthToSharedBufferPS.hlsl", { { "PSHADER", "" } }, "ps_5_0");
	if (!vs || !ps) {
		if (!loggedPublishFail.exchange(true))
			logger::error("[VrAriseTail] X4 publish refused: depth blit shaders missing");
		return;
	}
	{
		Util::FullscreenPassScope stateScope(globals::d3d::context);
		D3D11_VIEWPORT viewport{};
		viewport.Width = static_cast<float>(guideW);
		viewport.Height = static_cast<float>(guideH);
		viewport.MaxDepth = 1.0f;
		globals::d3d::context->RSSetViewports(1, &viewport);
		globals::d3d::context->IASetInputLayout(nullptr);
		globals::d3d::context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		globals::d3d::context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		globals::d3d::context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		globals::d3d::context->VSSetShader(vs, nullptr, 0);
		globals::d3d::context->PSSetShader(ps, nullptr, 0);
		if (upscaling.upscaleRasterizerState)
			globals::d3d::context->RSSetState(upscaling.upscaleRasterizerState.get());
		if (upscaling.upscaleBlendState)
			globals::d3d::context->OMSetBlendState(upscaling.upscaleBlendState.get(), nullptr, 0xffffffff);
		ID3D11ShaderResourceView* views[]{ depthSrv };
		globals::d3d::context->PSSetShaderResources(0, 1, views);
		ID3D11RenderTargetView* rtvs[]{ sharedDepthRtv[slot].Get() };
		globals::d3d::context->OMSetRenderTargets(1, rtvs, nullptr);
		globals::d3d::context->Draw(3, 0);
	}
	++sharedFenceValue;
	if (FAILED(context4->Signal(sharedFence11.Get(), sharedFenceValue))) {
		if (!loggedPublishFail.exchange(true))
			logger::warn("[VrAriseTail] X2 fence Signal failed");
		return;
	}
	sharedPublished.fenceValue = sharedFenceValue;
	sharedPublished.slot = slot;
	sharedPublished.generation = sharedGeneration;
	sharedPublished.width = desc.Width;
	sharedPublished.height = desc.Height;
	sharedPublished.format = desc.Format;
	sharedPublished.hasGuides = true;
	sharedPublished.guideWidth = guideW;
	sharedPublished.guideHeight = guideH;
	sharedPublished.depthFormat = DXGI_FORMAT_R32_FLOAT;
	sharedPublished.mvecFormat = mvecFmt;
	sharedConstants[0] = constants[0];
	sharedConstants[1] = constants[1];
	sharedConstantsValid = true;
	if (!loggedPublish.exchange(true))
		logger::info("[VrAriseTail] X4 published kTEMP {}x{} + guides {}x{} mvecFmt={} slot={}",
			desc.Width, desc.Height, guideW, guideH, static_cast<unsigned>(mvecFmt), slot);
#else
	(void)kTemp;
#endif
}

VrAriseTail::Status VrAriseTail::GetStatus()
{
	std::scoped_lock lock(statusMutex);
	auto copy = status;
	copy.active = IsActive();
	if (!copy.active)
		copy.path = "off";
	return copy;
}
