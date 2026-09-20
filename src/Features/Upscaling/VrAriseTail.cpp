#include "PCH.h"
#include "VrAriseTail.h"

#include "../../Globals.h"
#include "../../State.h"
#include "../../Utils/D3D.h"
#include "../../Utils/WinApi.h"
#include "../Upscaling.h"
#include "GpuPass.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#ifdef ENABLE_SKYRIM_VR
#	include "RE/B/BSOpenVR.h"
#	include "VrBeat/Metronome.h"
#	include "VrBridge/DbEnd.h"
#	include "VrCount/PreAfter.h"
#	include "VrGWrap/GPresent.h"
#	include "VrPose/Absolute.h"
#	include "VrSimGate/PresentGate.h"
#	include "VrSwitch/Master.h"
#endif

using Microsoft::WRL::ComPtr;

namespace
{
	std::atomic<bool> installed{ false };
	std::atomic<bool> running{ false };
	std::atomic<bool> active{ false };
	std::atomic<bool> vrReady{ false };
	std::atomic<bool> abandoned{ false };
	std::thread worker;

	// Diagnostic: PublishKTemp's pose capture (CopyEngineDrawPose) assumes
	// exactly one UpdatePoses call happened since the last Submit -- i.e. the
	// snooped pose is still "this frame's" and hasn't been overwritten by a
	// later UpdatePoses that belongs to a *different* rendered frame. Every
	// numeric diagnostic tried so far (pose continuity, GPU fence waits,
	// UpdatePoses(true/false) mode, overlay swallow) has come back clean
	// across repeated menu-open judder repros, so this checks the one
	// remaining place a stale-vs-fresh mismatch could hide without showing
	// up as a bad *value* -- only as a bad *pairing*. Both increment/reset
	// happen on the game thread (UpdatePosesHook::thunk and PublishKTemp are
	// both called from the engine's own render thread), so this is read-reset
	// without a lock; a plain (non-atomic) counter would already be safe,
	// atomic only guards against ever being read from the wrong thread later.
	std::atomic<uint32_t> updatePosesSincePublish{ 0 };

	std::mutex statusMutex;
	VrAriseTail::Status status{};

	constexpr UINT kSharedSlots = 2;
	constexpr DWORD kFenceWaitMs = 16;

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
		// Captured at the same instant as the color (in PublishKTemp, on the
		// game thread, right after this frame's UpdatePoses). Rides along
		// with the fenceValue so Tail/Doorbell can be arbitrarily late
		// without pairing this frame's pixels with a later frame's pose.
		VrPose::Body pose{};
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
	void AbandonTakeover()
	{
		abandoned.store(true, std::memory_order_release);
		running.store(false, std::memory_order_release);
		active.store(false, std::memory_order_release);
		VrBeat::Stop();
	}

	struct UpdatePosesHook
	{
		static void thunk(RE::BSOpenVR* openVR, bool useLivePoses)
		{
			if (!BootLatchedArise() || abandoned.load(std::memory_order_acquire)) {
				func(openVR, useLivePoses);
				return;
			}
			VrBeat::HookCompositorPoses();
			// EXPERIMENT (09-20 01:13): route the engine's own Wait through
			// GetLastPoses (non-blocking snapshot, documented safe for a
			// second thread -- doesn't open a period, doesn't compete for
			// anything) instead of the real blocking WaitGetPoses. The
			// metronome is now the *sole* caller of the real Wait,
			// continuously, as fast as the runtime returns it -- so there
			// is only ever one real blocking caller, eliminating the
			// two-callers-racing-the-same-mutex contention that crashed
			// 72fps to 5-8fps tonight when the metronome's own extra Wait
			// for G's interpolated frame collided with the engine's.
			func(openVR, false);
			updatePosesSincePublish.fetch_add(1, std::memory_order_relaxed);
			VrPoseAbs::OnEngineDrawStart();
			static bool logged = false;
			if (!logged) {
				logger::info("[VrAriseTail] Engine UpdatePoses; Abs from engine's own OpenVR pose snoop");
				logged = true;
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

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
		// PrepareShared's own list/allocator (single-buffered, unrelated to
		// Doorbell's double-buffered copy below -- they run sequentially on
		// the same thread, never concurrently, so this stays single).
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		// Double-buffered so Doorbell::Snapshot's Reset() of slot N never has
		// to wait on slot N's own GPU work from last call -- it waits on N-2
		// calls ago instead, which the GPU has a full extra Snapshot interval
		// to clear. A logged session showed that wait alone tripling (0.4ms
		// -> 1.1ms mean) right when judder was reported, with throughput
		// unaffected; this removes that self-imposed serialization without
		// touching Queue12's topology (still the single shared queue).
		ComPtr<ID3D12CommandAllocator> snapAllocator[2];
		ComPtr<ID3D12GraphicsCommandList> snapList[2];
		UINT64 prepareFenceValue = 0;
		ComPtr<ID3D12DescriptorHeap> uavHeap;
		ComPtr<ID3D12RootSignature> opaqueRoot;
		ComPtr<ID3D12PipelineState> opaquePso;
		ComPtr<ID3D12Resource> color;
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

		// Diagnostics only: PrepareShared has ~6 distinct early-return sites
		// and the call site only ever logged "failed" once for the whole
		// session, so a real occurrence couldn't be told apart from a
		// one-off hitch. lastFailReason names which check tripped; failCount
		// tracks whether it happened once or is stuck failing every cycle.
		const char* lastFailReason = "";
		std::uint64_t failCount = 0;

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
			if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
				return false;
			if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
				return false;
			list->Close();
			for (int i = 0; i < 2; ++i) {
				if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&snapAllocator[i]))))
					return false;
				if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, snapAllocator[i].Get(), nullptr, IID_PPV_ARGS(&snapList[i]))))
					return false;
				snapList[i]->Close();
			}
			if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
				return false;
			fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			device->SetName(L"VrAriseTail::Device12");
			queue->SetName(L"VrAriseTail::Queue12");
			return fenceEvent != nullptr;
		}

		bool WaitFenceTimeout(UINT64 value, DWORD ms = kFenceWaitMs)
		{
			return WaitFenceCpu(fence.Get(), fenceEvent, value, ms);
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

		bool EnsureColorFor(UINT w, UINT h, DXGI_FORMAT format)
		{
			if (color && width == w && height == h && color->GetDesc().Format == format)
				return true;
			color.Reset();
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

		bool PrepareShared(const SharedPublish& published)
		{
			auto fail = [&](const char* why) {
				lastFailReason = why;
				++failCount;
				return false;
			};
			if (!published.fenceValue || !queue)
				return fail("no fence/queue");
			if (!ImportShared(published))
				return fail("ImportShared");
			if (!EnsureColorFor(published.width, published.height, published.format))
				return fail("EnsureColorFor");
			if (!WaitFenceCpu(openedSharedFence.Get(), fenceEvent, published.fenceValue, kFenceWaitMs))
				return fail("wait D3D11 publish fence (game-thread hitch)");
			if (!WaitFenceTimeout(fenceValue, kFenceWaitMs))
				return fail("wait own prior-copy fence (Tail-side hitch)");
			if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
				return fail("allocator/list Reset");
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
			if (FAILED(list->Close())) {
				// The command list built above transitioned colorState (and
				// depthState/mvecState if guides ran) back to COMMON inline,
				// but those commands were never submitted -- the actual GPU
				// resources are still wherever the last successful call left
				// them (COMMON, by this function's own invariant), so the
				// tracked state matches reality here. Nothing to resync.
				return fail("list Close");
			}
			ID3D12CommandList* lists[]{ list.Get() };
			queue->ExecuteCommandLists(1, lists);
			++fenceValue;
			queue->Signal(fence.Get(), fenceValue);
			prepareFenceValue = fenceValue;
			lastConsumed = published.fenceValue;
			return true;
		}
	};

	struct Doorbell
	{
		Dx12Path* dx12 = nullptr;
		ComPtr<ID3D12Resource> hudlessKeep[2];
		D3D12_RESOURCE_STATES keepState[2]{
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON
		};
		ComPtr<ID3D12Resource> keepDepth[2];
		ComPtr<ID3D12Resource> keepMvec[2];
		D3D12_RESOURCE_STATES keepDepthState[2]{
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON
		};
		D3D12_RESOURCE_STATES keepMvecState[2]{
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON
		};
		bool keepHasGuides[2]{};
		uint64_t keepFenceValue[2]{};
		UINT keepW = 0;
		UINT keepH = 0;
		UINT keepGuideW = 0;
		UINT keepGuideH = 0;
		DXGI_FORMAT keepDepthFmt = DXGI_FORMAT_R32_FLOAT;
		DXGI_FORMAT keepMvecFmt = DXGI_FORMAT_UNKNOWN;
		UINT publishedKeep = 0;
		std::atomic<uint32_t> kickSlot{ 1 };
		HANDLE snapshotWaitEvent = nullptr;
		std::thread worker;
		std::mutex kickMutex;
		std::condition_variable kickCv;
		std::atomic<bool> stop{ false };
		UINT reqW = 0;
		UINT reqH = 0;
		DXGI_FORMAT reqFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
		UINT width = 0;
		UINT height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
		std::atomic<bool> ready{ false };
		bool failed = false;
		bool gOnLogged = false;
		uint32_t kicks = 0;
		std::atomic<uint64_t> inputGen{ 0 };
		uint64_t takenGen = 0;

		// Diagnostics only: Tail thread writes these, Doorbell worker thread
		// reads/writes them too, so plain atomics rather than the mutex the
		// hot path can't afford. Correlate lastSnapshotWaitMs/lastKickWaitMs
		// spikes with "when did it start juddering" in the log.
		std::atomic<uint32_t> lastSlot{ 0 };
		std::atomic<double> lastSnapshotWaitMs{ 0.0 };
		std::atomic<double> lastKickWaitMs{ 0.0 };
		std::atomic<uint64_t> droppedGens{ 0 };
		const char* lastSnapshotFailReason = "";
		uint64_t snapshotFailCount = 0;

		~Doorbell()
		{
			stop.store(true, std::memory_order_release);
			kickCv.notify_all();
			if (worker.joinable())
				worker.join();
			if (snapshotWaitEvent) {
				CloseHandle(snapshotWaitEvent);
				snapshotWaitEvent = nullptr;
			}
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
			if (hudlessKeep[0] && keepW == w && keepH == h &&
				hudlessKeep[0]->GetDesc().Format == fmt)
				return true;
			for (int i = 0; i < 2; ++i) {
				if (!CreateTex(hudlessKeep[i], w, h, fmt, true, L"VrAriseTail::HudlessKeep"))
					return false;
				keepState[i] = D3D12_RESOURCE_STATE_COMMON;
				keepFenceValue[i] = 0;
			}
			keepW = w;
			keepH = h;
			return true;
		}

		bool CreateGuideKeep(UINT w, UINT h, DXGI_FORMAT depthFmt, DXGI_FORMAT mvecFmt)
		{
			if (keepDepth[0] && keepGuideW == w && keepGuideH == h &&
				keepDepthFmt == depthFmt && keepMvecFmt == mvecFmt)
				return true;
			for (int i = 0; i < 2; ++i) {
				if (!CreateTex(keepDepth[i], w, h, depthFmt, false, L"VrAriseTail::KeepDepth") ||
					!CreateTex(keepMvec[i], w, h, mvecFmt, false, L"VrAriseTail::KeepMvec"))
					return false;
				keepDepthState[i] = D3D12_RESOURCE_STATE_COMMON;
				keepMvecState[i] = D3D12_RESOURCE_STATE_COMMON;
				keepHasGuides[i] = false;
			}
			keepGuideW = w;
			keepGuideH = h;
			keepDepthFmt = depthFmt;
			keepMvecFmt = mvecFmt;
			return true;
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
				return false;
			};
			auto& sl = globals::features::upscaling.streamlineDX12;
			if (!dx12 || !dx12->device || !dx12->queue || !dx12->adapter)
				return fail("no Tail D3D12");
			if (sbsWidth < 2 || sbsHeight == 0)
				return fail("SBS too small");

			if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
				fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
			else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
				fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
			const DXGI_FORMAT engineFmt = fmt;
			fmt = DXGI_FORMAT_R8G8B8A8_UNORM;

			VrBeat::BindGpu(dx12->device.Get(), dx12->queue.Get());
			if (!snapshotWaitEvent)
				snapshotWaitEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!snapshotWaitEvent)
				return fail("snapshot event");

			width = sbsWidth;
			height = sbsHeight;
			format = fmt;
			ready.store(true, std::memory_order_release);
			if (sl.featureReflex && sl.slReflexSetOptions) {
				sl::ReflexOptions reflex{};
				reflex.mode = sl::ReflexMode::eLowLatency;
				reflex.useMarkersToOptimize = sl.featurePCL;
				reflex.frameLimitUs = 0;
				if (sl.slReflexSetOptions(reflex) == sl::Result::eOk)
					logger::info("[VrAriseTail] X4 Reflex eLowLatency on Tail DX12 (frameLimitUs=0)");
				else
					logger::warn("[VrAriseTail] X4 slReflexSetOptions failed");
			}
			logger::info("[VrAriseTail] X4 SBS doorbell {}x{} engineFmt={} RGBA8",
				sbsWidth, sbsHeight, static_cast<unsigned>(engineFmt));
			return true;
		}

		bool EnsureGPath()
		{
			if (VrGWrap::Ready())
				return true;
			if (!dx12 || !dx12->device || !dx12->queue || !dx12->adapter)
				return false;
			auto& sl = globals::features::upscaling.streamlineDX12;
			if (!sl.initialized || !sl.featureDLSSG || !sl.slUpgradeInterface) {
				logger::warn("[VrAriseTail] G path: DLSS-G not bound");
				return false;
			}
			if (!VrGWrap::Ensure(dx12->device.Get(), dx12->queue.Get(), dx12->adapter.Get(),
					width, height, format))
				return false;
			if (!VrSimGate::Ensure(dx12->device.Get(), dx12->queue.Get(),
					width, height, width, height, format,
					DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT))
				return false;
			return true;
		}

		bool Snapshot(ID3D12Resource* color, UINT w, UINT h, DXGI_FORMAT fmt)
		{
			auto fail = [&](const char* why) {
				lastSnapshotFailReason = why;
				++snapshotFailCount;
				return false;
			};
			if (!color || !dx12 || !dx12->device || !dx12->guidesReady || !dx12->depth || !dx12->mvec)
				return fail("no color/device/guides yet");
			if (w < 2 || h == 0 || dx12->guideWidth < 2 || dx12->guideHeight == 0)
				return fail("size too small");
			constexpr DXGI_FORMAT kDoorbellFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
			if (!CreateKeep(w, h, kDoorbellFmt) ||
				!CreateGuideKeep(dx12->guideWidth, dx12->guideHeight, dx12->depthFormat, dx12->mvecFormat))
				return fail("CreateKeep/CreateGuideKeep");
			if (fmt != kDoorbellFmt)
				return fail("format mismatch");
			const UINT dest = 1u - kickSlot.load(std::memory_order_acquire);
			lastSlot.store(dest, std::memory_order_relaxed);
			// Wait on slot `dest`'s own last-signaled value (from the Snapshot
			// call before last -- the other slot ran in between), not the
			// global latest fenceValue. Reusing allocator[dest]/list[dest] only
			// needs that specific submission retired.
			const UINT64 destPriorFence = keepFenceValue[dest];
			{
				LARGE_INTEGER freq{}, start{}, end{};
				QueryPerformanceFrequency(&freq);
				QueryPerformanceCounter(&start);
				const bool waited = dx12->WaitFenceTimeout(destPriorFence, kFenceWaitMs);
				QueryPerformanceCounter(&end);
				const double ms = freq.QuadPart ?
					(static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)) :
					0.0;
				lastSnapshotWaitMs.store(ms, std::memory_order_relaxed);
				if (ms > 3.0)
					logger::warn("[VrAriseTail] Snapshot fence wait {:.2f} ms (hitch candidate) dest={}", ms, dest);
				if (!waited)
					return fail("wait own prior-copy fence (Tail-side hitch)");
			}
			ID3D12CommandAllocator* alloc = dx12->snapAllocator[dest].Get();
			ID3D12GraphicsCommandList* cmd = dx12->snapList[dest].Get();
			if (FAILED(alloc->Reset()) || FAILED(cmd->Reset(alloc, nullptr)))
				return fail("allocator/list Reset");
			if (dx12->colorState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
				Transition(cmd, color, dx12->colorState, D3D12_RESOURCE_STATE_COPY_SOURCE);
				dx12->colorState = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			if (keepState[dest] != D3D12_RESOURCE_STATE_COPY_DEST) {
				Transition(cmd, hudlessKeep[dest].Get(), keepState[dest],
					D3D12_RESOURCE_STATE_COPY_DEST);
				keepState[dest] = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			cmd->CopyResource(hudlessKeep[dest].Get(), color);
			keepHasGuides[dest] = false;
			if (keepDepthState[dest] != D3D12_RESOURCE_STATE_COPY_DEST) {
				Transition(cmd, keepDepth[dest].Get(), keepDepthState[dest],
					D3D12_RESOURCE_STATE_COPY_DEST);
				keepDepthState[dest] = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			if (keepMvecState[dest] != D3D12_RESOURCE_STATE_COPY_DEST) {
				Transition(cmd, keepMvec[dest].Get(), keepMvecState[dest],
					D3D12_RESOURCE_STATE_COPY_DEST);
				keepMvecState[dest] = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			if (dx12->depthState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
				Transition(cmd, dx12->depth.Get(), dx12->depthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
				dx12->depthState = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			if (dx12->mvecState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
				Transition(cmd, dx12->mvec.Get(), dx12->mvecState, D3D12_RESOURCE_STATE_COPY_SOURCE);
				dx12->mvecState = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			cmd->CopyResource(keepDepth[dest].Get(), dx12->depth.Get());
			cmd->CopyResource(keepMvec[dest].Get(), dx12->mvec.Get());
			Transition(cmd, hudlessKeep[dest].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_COMMON);
			keepState[dest] = D3D12_RESOURCE_STATE_COMMON;
			Transition(cmd, keepDepth[dest].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_COMMON);
			keepDepthState[dest] = D3D12_RESOURCE_STATE_COMMON;
			Transition(cmd, keepMvec[dest].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_COMMON);
			keepMvecState[dest] = D3D12_RESOURCE_STATE_COMMON;
			keepHasGuides[dest] = true;
			Transition(cmd, color, dx12->colorState, D3D12_RESOURCE_STATE_COMMON);
			dx12->colorState = D3D12_RESOURCE_STATE_COMMON;
			if (dx12->depthState != D3D12_RESOURCE_STATE_COMMON) {
				Transition(cmd, dx12->depth.Get(), dx12->depthState, D3D12_RESOURCE_STATE_COMMON);
				dx12->depthState = D3D12_RESOURCE_STATE_COMMON;
			}
			if (dx12->mvecState != D3D12_RESOURCE_STATE_COMMON) {
				Transition(cmd, dx12->mvec.Get(), dx12->mvecState, D3D12_RESOURCE_STATE_COMMON);
				dx12->mvecState = D3D12_RESOURCE_STATE_COMMON;
			}
			if (FAILED(cmd->Close()))
				return fail("list Close");
			ID3D12CommandList* lists[]{ cmd };
			dx12->queue->ExecuteCommandLists(1, lists);
			++dx12->fenceValue;
			dx12->queue->Signal(dx12->fence.Get(), dx12->fenceValue);
			keepFenceValue[dest] = dx12->fenceValue;
			publishedKeep = dest;
			return true;
		}

		/**
		 * Diagnostic-only: 09-19's crash left no crash-*.log/WER dump despite
		 * Crash Logger being installed, and Windows' own Event Log showed
		 * 0xc0000409 (fail-fast, not a normal AV) — consistent with an
		 * uncaught exception reaching std::terminate on one of Tail's threads.
		 * This cannot make that safe; it only names the exception first so
		 * the next occurrence doesn't require guessing again.
		 */
		void WorkerLoopImpl()
		{
			SetThreadDescription(GetCurrentThread(), L"VrAriseDoorbell");
			while (!stop.load(std::memory_order_acquire)) {
				MSG msg{};
				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
					DispatchMessageW(&msg);
				if (!ready.load(std::memory_order_acquire) && !failed &&
					inputGen.load(std::memory_order_acquire) > 0)
					Ensure(reqW, reqH, reqFmt);
				VrSwitch::Tick();
				if (ready.load(std::memory_order_acquire) && VrSwitch::On())
					EnsureGPath();
				if (VrSwitch::On())
					VrGWrap::Pump();
				if (ready.load(std::memory_order_acquire)) {
					const uint64_t gen = inputGen.load(std::memory_order_acquire);
					// gen advances once per successful OfferKick/Snapshot; if it
					// jumped by more than 1 since the last Kick, one or more
					// snapshotted frames were silently overwritten before this
					// worker ever read them (this thread fell behind Tail's).
					if (gen > takenGen + 1) {
						const uint64_t dropped = gen - takenGen - 1;
						droppedGens.fetch_add(dropped, std::memory_order_relaxed);
						logger::warn("[VrAriseTail] Doorbell fell behind: {} snapshot(s) overwritten before Kick (gen {} -> {})",
							dropped, takenGen, gen);
					}
					if (gen != takenGen && Kick())
						takenGen = gen;
					else {
						std::unique_lock lock(kickMutex);
						kickCv.wait_for(lock, std::chrono::milliseconds(2), [&] {
							return stop.load(std::memory_order_acquire) ||
								inputGen.load(std::memory_order_acquire) != takenGen;
						});
					}
				} else {
					std::unique_lock lock(kickMutex);
					kickCv.wait_for(lock, std::chrono::milliseconds(2), [&] {
						return stop.load(std::memory_order_acquire) ||
							inputGen.load(std::memory_order_acquire) > 0;
					});
				}
			}
			VrSimGate::Shutdown();
			VrGWrap::Shutdown();
		}

		void WorkerLoop()
		{
			try {
				WorkerLoopImpl();
			} catch (const std::exception& e) {
				logger::critical("[VrAriseTail] Doorbell WorkerLoop uncaught exception: {}", e.what());
				throw;
			} catch (...) {
				logger::critical("[VrAriseTail] Doorbell WorkerLoop uncaught non-standard exception");
				throw;
			}
		}

		void OfferKick(ID3D12Resource* color, UINT w, UINT h, DXGI_FORMAT fmt, const VrPose::Body& pose)
		{
			reqW = w;
			reqH = h;
			reqFmt = fmt;
			if (!Snapshot(color, w, h, fmt)) {
				// Every occurrence: see the matching PrepareShared comment.
				if (snapshotFailCount <= 8 || (snapshotFailCount % 60) == 0)
					logger::error("[VrAriseTail] Snapshot failed #{}: {}", snapshotFailCount, lastSnapshotFailReason);
				return;
			}
			VrPoseAbs::LatchDraw(pose);
			inputGen.fetch_add(1, std::memory_order_release);
			if (!worker.joinable()) {
				worker = std::thread([this] { WorkerLoop(); });
				logger::info("[VrAriseTail] Snapshot+Kick; G or bridge at Kick");
			}
			kickCv.notify_one();
		}

		bool Kick()
		{
			if (!ready.load(std::memory_order_acquire) || !dx12)
				return false;
			const UINT slot = publishedKeep;
			if (!hudlessKeep[slot])
				return false;
			if (keepFenceValue[slot]) {
				LARGE_INTEGER freq{}, start{}, end{};
				QueryPerformanceFrequency(&freq);
				QueryPerformanceCounter(&start);
				const bool waited = WaitFenceCpu(dx12->fence.Get(), snapshotWaitEvent, keepFenceValue[slot], kFenceWaitMs);
				QueryPerformanceCounter(&end);
				const double ms = freq.QuadPart ?
					(static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)) :
					0.0;
				lastKickWaitMs.store(ms, std::memory_order_relaxed);
				if (ms > 3.0)
					logger::warn("[VrAriseTail] Kick fence wait {:.2f} ms (hitch candidate) slot={}", ms, slot);
				if (!waited)
					return false;
			}

			// Same double-buffer flip as the G path below: without this the
			// bridge path never advances kickSlot, Snapshot()'s `dest` always
			// resolves to the same slot, and the "double" buffer degenerates
			// into a single buffer Snapshot can overwrite while this Kick is
			// still reading it -- no hazard tracking catches that case.
			kickSlot.store(slot, std::memory_order_release);
			if (!VrSwitch::On()) {
				if (!VrBridge::OnDbFinalColor(hudlessKeep[slot].Get(), width, height, format))
					return false;
				++kicks;
				if (kicks <= 8 || (kicks % 120) == 0)
					logger::info("[VrAriseTail] bridge kick={} slot={}", kicks, slot);
				return true;
			}

			if (!VrGWrap::SwapChain())
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
			if (!keepHasGuides[slot] || !keepDepth[slot] || !keepMvec[slot])
				return false;
			VrSimGate::DbFrame db{};
			db.color = hudlessKeep[slot].Get();
			db.depth = keepDepth[slot].Get();
			db.mvec = keepMvec[slot].Get();
			db.constants = constants;
			db.colorW = width;
			db.colorH = height;
			db.guideW = keepGuideW;
			db.guideH = keepGuideH;
			db.colorFmt = format;
			db.depthFmt = keepDepthFmt;
			db.mvecFmt = keepMvecFmt;
			db.enableG = true;
			if (!VrSimGate::OnDbFinalColor(db))
				return false;
			++kicks;
			if (!gOnLogged) {
				gOnLogged = true;
				logger::info("[VrAriseTail] X4 G on P={} A={} kick={} guide {}x{} color {}x{}",
					VrCount::Pre(), VrCount::After(),
					kicks, keepGuideW, keepGuideH, width, height);
			}
			if (kicks <= 8 || (kicks % 120) == 0)
				logger::info("[VrAriseTail] X4 G kick={} status={} presented={} P={} A={}",
					kicks, magic_enum::enum_name(sl.lastDLSSGStatus), sl.lastDLSSGFramesPresented,
					VrCount::Pre(), VrCount::After());
			return true;
		}
	};

	void ResolveSize(std::uint32_t& width, std::uint32_t& height)
	{
		width = 1512;
		height = 1512;
	}

	void TailLoopImpl()
	{
		SetThreadDescription(GetCurrentThread(), L"VrAriseTail");
		timeBeginPeriod(1);
		Dx12Path dx12;
		Doorbell doorbell;
		doorbell.dx12 = &dx12;
		bool haveDevice = false;
		bool loggedPath = false;
		bool beatStarted = false;
		VrAriseTail::Status local{};
		local.active = true;
		local.path = "pending";

		while (running.load(std::memory_order_acquire) && !abandoned.load(std::memory_order_acquire)) {
			if (!haveDevice) {
				if (dx12.CreateDevice() && dx12.device) {
					haveDevice = true;
					local.path = "dx12";
				} else {
					Sleep(50);
					continue;
				}
			}

			if (!dx12.color) {
				std::uint32_t w = 0;
				std::uint32_t h = 0;
				ResolveSize(w, h);
				if (!dx12.EnsureColorFor(w, h, DXGI_FORMAT_R8G8B8A8_UNORM)) {
					logger::warn("[VrAriseTail] color target {}x{} failed", w, h);
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
			bool prepared = false;
			SharedPublish published{};

			published = SnapshotShared();
			if (published.fenceValue > dx12.lastConsumed) {
				if (dx12.PrepareShared(published)) {
					prepared = true;
					if (!VrBeat::HasGpuMail())
						local.path = "dx12-ktemp";
					static bool loggedShared = false;
					if (!loggedShared) {
						logger::info("[VrAriseTail] X2 shared Submit {}x{} eye={}x{} fence={} slot={} guides={} {}x{}",
							published.width, published.height, published.width / 2, published.height,
							published.fenceValue, published.slot, dx12.guidesReady ? 1 : 0,
							dx12.guideWidth, dx12.guideHeight);
						loggedShared = true;
					}
				} else {
					// Every occurrence, not just the first: a one-off hitch
					// self-corrects next cycle (lastConsumed didn't advance,
					// so this same fenceValue is retried); a failCount that
					// keeps climbing every cycle means something stayed
					// broken instead of recovering.
					if (dx12.failCount <= 8 || (dx12.failCount % 60) == 0)
						logger::error("[VrAriseTail] X2 PrepareShared failed #{}: {}",
							dx12.failCount, dx12.lastFailReason);
				}
			}
			if (VrBeat::HasFirstSubmit() && prepared && dx12.color && dx12.guidesReady)
				doorbell.OfferKick(dx12.color.Get(), published.width, published.height, published.format, published.pose);

			if (!dx12.color) {
				Sleep(1);
				PublishStatus(local);
				continue;
			}

			if (!VrBeat::HasGpuMail()) {
				VrBeat::Frame beat{};
				beat.queue12 = dx12.queue.Get();
				beat.left12 = dx12.color.Get();
				beat.right12 = dx12.color.Get();
				VrBeat::Publish(beat);
			}
			if (!beatStarted) {
				VrBeat::Start();
				beatStarted = true;
			}

			const auto beatStatus = VrBeat::GetStatus();
			local.lastWaitMs = beatStatus.lastWaitMs;
			local.lastWaitErr = beatStatus.lastWaitErr;
			local.lastSubmitL = beatStatus.lastSubmitL;
			local.lastSubmitR = beatStatus.lastSubmitR;
			local.submits = beatStatus.submits;
			local.lastKeepSlot = doorbell.lastSlot.load(std::memory_order_relaxed);
			local.lastSnapshotWaitMs = doorbell.lastSnapshotWaitMs.load(std::memory_order_relaxed);
			local.lastKickWaitMs = doorbell.lastKickWaitMs.load(std::memory_order_relaxed);
			local.droppedGens = doorbell.droppedGens.load(std::memory_order_relaxed);
			local.sharedPrepareFails = dx12.failCount;
			local.lastSharedFailReason = dx12.lastFailReason;
			local.snapshotFails = doorbell.snapshotFailCount;
			local.lastSnapshotFailReason = doorbell.lastSnapshotFailReason;
			if (!loggedPath && beatStatus.firstSubmit) {
				logger::info("[VrAriseTail] metronome first Submit path={}", local.path);
				loggedPath = true;
			}
			PublishStatus(local);
			Sleep(1);
		}
		if (!VrBeat::HasFirstSubmit())
			AbandonTakeover();
		else
			VrBeat::Stop();
		timeEndPeriod(1);
	}

	/**
	 * Diagnostic-only wrapper: see WorkerLoopImpl's comment. Names the
	 * exception if this thread is where a future 0xc0000409 comes from.
	 */
	void TailLoop()
	{
		try {
			TailLoopImpl();
		} catch (const std::exception& e) {
			logger::critical("[VrAriseTail] TailLoop uncaught exception: {}", e.what());
			throw;
		} catch (...) {
			logger::critical("[VrAriseTail] TailLoop uncaught non-standard exception");
			throw;
		}
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
	if (VrBeat::HasFirstSubmit())
		return true;
	while (!VrBeat::HasFirstSubmit() &&
		!abandoned.load(std::memory_order_acquire)) {
		if (!running.load(std::memory_order_acquire)) {
			logger::error("[VrAriseTail] Tail exited before first Submit; abandoning takeover");
			AbandonTakeover();
			return false;
		}
		if (!VrBeat::WaitForFirstSubmitMs(5000) && !VrBeat::HasFirstSubmit())
			logger::warn("[VrAriseTail] still waiting for metronome first Submit (OC session bind)");
	}
	return VrBeat::HasFirstSubmit() &&
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
	if (globals::state && globals::state->isLoadingMenuOpen) {
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
		logger::info("[VrAriseTail] X4 publish resumed");
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
	// Same instant as the color above, on this same (game) thread: this is
	// still exactly this frame's pose, not whatever UpdatePoses last wrote --
	// *if* exactly one UpdatePoses happened since the last Submit. Diagnostic:
	// name it when that assumption breaks (0 = reusing a pose already spent
	// on a prior kTEMP; >1 = this kTEMP is paired with a later frame's pose
	// than the one it was actually drawn with) rather than assuming 1:1.
	sharedPublished.pose.valid = VrBeat::CopyEngineDrawPose(sharedPublished.pose.m);
	const uint32_t updatePosesThisPublish = updatePosesSincePublish.exchange(0, std::memory_order_relaxed);
	if (updatePosesThisPublish != 1) {
		static std::uint32_t mismatchCount = 0;
		++mismatchCount;
		if (mismatchCount <= 16 || (mismatchCount % 60) == 0)
			logger::warn("[VrAriseTail] X4 publish/UpdatePoses mismatch #{}: {} UpdatePoses call(s) since last publish (expected 1)",
				mismatchCount, updatePosesThisPublish);
	}
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
