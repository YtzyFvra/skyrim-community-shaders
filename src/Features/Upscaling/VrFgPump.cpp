#include "VrFgPump.h"

#include "../../Globals.h"
#include "../../GpuPass.h"
#include "../../State.h"
#include "../Upscaling.h"
#include "DX12SwapChain.h"
#include "FoveatedRender/Ops.h"
#include "Streamline.h"
#include "VrSubmitDlssg.h"
#include "Utils/D3D.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace
{
	constexpr wchar_t kWindowClass[] = L"OpenShadersVrFgPump";
	constexpr UINT kBackBufferCount = 3;
	constexpr UINT kMaxSwapchainBuffers = 8;
	constexpr UINT kSubmitRing = 8;
	constexpr UINT kWindowWidth = 8;
	constexpr UINT kWindowHeight = 8;
	constexpr UINT kPresentVtable = 8;
	constexpr UINT kPresent1Vtable = 22;
	constexpr UINT kVtableCopy = 64;

	using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

	struct HookCopy
	{
		WrappedResource* slot = nullptr;
		UINT64 fence = 0;
		UINT index = 0;
	};

	struct PumpState
	{
		std::mutex mutex;
		std::mutex submitMutex;
		std::mutex fenceWaitMutex;
		bool active = false;
		bool failed = false;
		bool loggedPresent = false;
		bool allowTearing = false;
		uint32_t appPresents = 0;
		std::atomic<uint32_t> hookSubmits{ 0 };
		std::atomic<uint32_t> submitRingWrite{ 0 };
		UINT swapBufferCount = kBackBufferCount;
		uint32_t width = 0;
		uint32_t height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;

		HWND hwnd = nullptr;
		HANDLE fenceEvent = nullptr;
		HANDLE presentEvent = nullptr;
		std::thread presentThread;
		std::unique_ptr<void*[]> clonedVtable;
		std::atomic<bool> presentBusy{ false };
		std::atomic<bool> workerStop{ false };
		std::atomic<uint32_t> traceSeq{ 0 };
		std::atomic<const char*> lastStep{ "" };
		std::atomic<int64_t> kickQpc{ 0 };
		std::atomic<uint32_t> lastHr{ 0 };
		std::thread watchdogThread;
		bool loggedForeignPresent = false;
		std::atomic<UINT64> presentFence{ 0 };
		std::atomic<bool> workerEnableG{ false };
		std::atomic<bool> workerConstantsOk{ false };
		std::atomic<UINT> workerListSlot{ 0 };
		winrt::com_ptr<ID3D11Device5> device11;
		winrt::com_ptr<ID3D11DeviceContext4> context11;
		winrt::com_ptr<ID3D12Device> device12;
		winrt::com_ptr<ID3D12CommandQueue> queue;
		winrt::com_ptr<ID3D12CommandAllocator> allocators[kBackBufferCount];
		winrt::com_ptr<ID3D12GraphicsCommandList> lists[kBackBufferCount];
		winrt::com_ptr<ID3D12CommandAllocator> hookAllocators[2];
		winrt::com_ptr<ID3D12GraphicsCommandList> hookLists[2];
		std::atomic<uint32_t> hookListWrite{ 0 };
		winrt::com_ptr<IDXGISwapChain4> slSwapChain;
		winrt::com_ptr<IDXGISwapChain> nativeSwapChain;
		winrt::com_ptr<IDXGISwapChain1> nativeSwap1;
		winrt::com_ptr<IDXGISwapChain3> nativeSwap3;
		winrt::com_ptr<ID3D12Resource> backBuffers[kMaxSwapchainBuffers];
		winrt::com_ptr<ID3D11Fence> fence11;
		winrt::com_ptr<ID3D12Fence> fence12;
		std::atomic<UINT64> fenceValue{ 0 };
		UINT frameIndex = 0;

		std::unique_ptr<WrappedResource> hudLess;
		std::unique_ptr<WrappedResource> depth;
		std::unique_ptr<WrappedResource> motion;
		std::unique_ptr<WrappedResource> ui;
		std::unique_ptr<WrappedResource> submitRing[kSubmitRing];
		winrt::com_ptr<ID3D11Texture2D> submitPresent[2];
		winrt::com_ptr<ID3D11Multithread> mt11;
		uint32_t submitPresentWrite = 0;

		PresentFn originalPresent = nullptr;
		Present1Fn originalPresent1 = nullptr;
	};

	PumpState g_pump;

	D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource,
		D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = resource;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = after;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		return barrier;
	}

	UINT64 NextFence()
	{
		return g_pump.fenceValue.fetch_add(1, std::memory_order_relaxed) + 1;
	}

	int64_t NowQpc()
	{
		LARGE_INTEGER qpc{};
		QueryPerformanceCounter(&qpc);
		return qpc.QuadPart;
	}

	uint32_t BusyMs()
	{
		const int64_t kick = g_pump.kickQpc.load(std::memory_order_relaxed);
		if (!kick || !g_pump.presentBusy.load(std::memory_order_relaxed))
			return 0;
		LARGE_INTEGER freq{};
		QueryPerformanceFrequency(&freq);
		const double ms = (NowQpc() - kick) * 1000.0 / static_cast<double>(freq.QuadPart);
		return ms > 0.0 ? static_cast<uint32_t>(ms) : 0;
	}

	const char* HrName(HRESULT hr)
	{
		if (hr == E_ABORT)
			return "E_ABORT";
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
			return "WAS_STILL_DRAWING";
		if (hr == DXGI_ERROR_DEVICE_REMOVED)
			return "DEVICE_REMOVED";
		if (hr == DXGI_ERROR_DEVICE_HUNG)
			return "DEVICE_HUNG";
		if (hr == DXGI_ERROR_DEVICE_RESET)
			return "DEVICE_RESET";
		return "";
	}

	void Trace(const char* step, UINT a = 0, UINT b = 0, HRESULT hr = S_OK)
	{
		g_pump.lastStep.store(step, std::memory_order_release);
		g_pump.lastHr.store(static_cast<uint32_t>(hr), std::memory_order_relaxed);
		const uint32_t n = g_pump.traceSeq.fetch_add(1, std::memory_order_relaxed) + 1;
		const bool verbose = globals::features::upscaling.settings.vrFgPumpTrace;
		const bool always = n <= 80 || FAILED(hr) || std::strncmp(step, "watchdog", 8) == 0;
		if (!always && !verbose && (n % 60) != 0)
			return;
		logger::info("[VrFgPump] step={} n={} tid={} a={} b={} hr=0x{:08X} {} busy={} hook={} app={} vis={} busyMs={}",
			step, n, GetCurrentThreadId(), a, b, static_cast<uint32_t>(hr), HrName(hr),
			g_pump.presentBusy.load(std::memory_order_relaxed),
			g_pump.hookSubmits.load(std::memory_order_relaxed),
			g_pump.appPresents,
			(g_pump.hwnd && IsWindowVisible(g_pump.hwnd)) ? 1 : 0,
			BusyMs());
	}

	bool PumpTearingEnabled()
	{
		return g_pump.allowTearing && !globals::features::upscaling.settings.vrFgPumpDisableTearing;
	}

	bool WaitFenceCpu(UINT64 value)
	{
		if (!g_pump.fence12 || !g_pump.fenceEvent)
			return false;
		std::lock_guard lock(g_pump.fenceWaitMutex);
		if (g_pump.fence12->GetCompletedValue() >= value)
			return true;
		if (FAILED(g_pump.fence12->SetEventOnCompletion(value, g_pump.fenceEvent)))
			return false;
		return WaitForSingleObject(g_pump.fenceEvent, 1000) == WAIT_OBJECT_0;
	}

#ifdef ENABLE_SKYRIM_VR
	void PumpWindowMessages(PumpState& pump)
	{
		MSG msg{};
		while (pump.hwnd && PeekMessageW(&msg, pump.hwnd, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	void WorkerDoorbell(PumpState& pump);

	UINT CurrentNativeBackbuffer()
	{
		if (g_pump.nativeSwap3)
			return g_pump.nativeSwap3->GetCurrentBackBufferIndex();
		return 0;
	}

	HookCopy CopyBackbufferOnHook(PumpState& pump, UINT index)
	{
		if (index >= pump.swapBufferCount || !pump.backBuffers[index])
			return {};
		const UINT listSlot = pump.hookListWrite.fetch_add(1, std::memory_order_relaxed) % 2;
		auto* allocator = pump.hookAllocators[listSlot].get();
		auto* list = pump.hookLists[listSlot].get();
		auto* dest = pump.submitRing[pump.submitRingWrite.fetch_add(1, std::memory_order_relaxed) % kSubmitRing].get();
		if (!allocator || !list || !dest || !dest->resource) {
			Trace("hook-copy-fail", index);
			return {};
		}
		if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr))) {
			Trace("hook-copy-reset", index);
			return {};
		}

		auto* src = pump.backBuffers[index].get();
		auto* dst = dest->resource.get();
		D3D12_RESOURCE_BARRIER barriers[2]{
			Transition(src, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
			Transition(dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
		};
		list->ResourceBarrier(2, barriers);
		list->CopyResource(dst, src);
		barriers[0] = Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
		barriers[1] = Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		list->ResourceBarrier(2, barriers);
		if (FAILED(list->Close()))
			return {};

		ID3D12CommandList* lists[]{ list };
		pump.queue->ExecuteCommandLists(1, lists);
		const UINT64 fence = NextFence();
		if (FAILED(pump.queue->Signal(pump.fence12.get(), fence))) {
			Trace("hook-copy-signal", index);
			return {};
		}
		Trace("hook-copy-ok", index, dest ? 1 : 0);
		return { dest, fence, index };
	}

	bool SubmitHooked(WrappedResource* slot, PumpState& pump)
	{
		if (!slot || !slot->resource11 || !pump.context11)
			return false;
		std::lock_guard lock(pump.submitMutex);
		const UINT dst = pump.submitPresentWrite++ & 1u;
		auto* present = pump.submitPresent[dst].get();
		if (!present)
			return false;
		if (pump.mt11)
			pump.mt11->Enter();
		pump.context11->CopyResource(present, slot->resource11);
		pump.context11->Flush();
		const bool ok = VrSubmitDlssg::SubmitOwnedSbs(present, pump.width, pump.height);
		if (pump.mt11)
			pump.mt11->Leave();
		return ok;
	}

	void LogHookPresent(UINT index)
	{
		const uint32_t n = g_pump.hookSubmits.fetch_add(1, std::memory_order_relaxed) + 1;
		if (n > 8 && (n % 120) != 0)
			return;
		LARGE_INTEGER qpc{};
		QueryPerformanceCounter(&qpc);
		logger::info("[VrFgPump] Submit qpc={} index={} tid={} n={}",
			qpc.QuadPart, index, GetCurrentThreadId(), n);
	}

	void SubmitAfterPresent(const HookCopy& copy, HRESULT hr)
	{
		if (FAILED(hr) || !copy.slot || !copy.fence) {
			Trace("hook-skip-submit", copy.index, copy.slot ? 1 : 0, hr);
			return;
		}
		Trace("hook-wait-fence", copy.index);
		if (!WaitFenceCpu(copy.fence)) {
			Trace("hook-wait-timeout", copy.index);
			return;
		}
		if (globals::features::upscaling.settings.vrFgPumpSkipSubmit) {
			Trace("hook-submit-skipped", copy.index);
			return;
		}
		Trace("hook-submit", copy.index);
		if (SubmitHooked(copy.slot, g_pump))
			LogHookPresent(copy.index);
		else
			Trace("hook-submit-fail", copy.index);
	}

	bool IsPumpChain(IUnknown* self)
	{
		return self && (self == g_pump.nativeSwapChain.get() ||
			self == static_cast<IUnknown*>(g_pump.nativeSwap1.get()) ||
			self == static_cast<IUnknown*>(g_pump.slSwapChain.get()));
	}

	HRESULT ForwardForeignPresent(IDXGISwapChain* self, UINT sync, UINT flags)
	{
		if (!g_pump.loggedForeignPresent) {
			g_pump.loggedForeignPresent = true;
			logger::warn("[VrFgPump] Foreign Present this={} tid={} — DXGI vtable is shared; forwarding",
				static_cast<void*>(self), GetCurrentThreadId());
		}
		return g_pump.originalPresent(self, sync, flags);
	}

	HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* self, UINT sync, UINT flags)
	{
		if (!IsPumpChain(self))
			return ForwardForeignPresent(self, sync, flags);
		const bool test = (flags & DXGI_PRESENT_TEST) != 0;
		const UINT index = CurrentNativeBackbuffer();
		Trace("hook-enter", index, test ? 1 : 0);
		HookCopy copy{};
		if (!test)
			copy = CopyBackbufferOnHook(g_pump, index);
		UINT presentSync = sync;
		if (!test && PumpTearingEnabled()) {
			flags |= DXGI_PRESENT_ALLOW_TEARING;
			presentSync = 0;
		}
		Trace("hook-orig-present", index, presentSync);
		const HRESULT hr = g_pump.originalPresent(self, presentSync, flags);
		Trace("hook-orig-done", index, 0, hr);
		if (!test)
			SubmitAfterPresent(copy, hr);
		return hr;
	}

	HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* self, UINT sync, UINT flags,
		const DXGI_PRESENT_PARAMETERS* params)
	{
		if (!g_pump.originalPresent1)
			return g_pump.originalPresent(self, sync, flags);
		if (!IsPumpChain(self)) {
			if (!g_pump.loggedForeignPresent) {
				g_pump.loggedForeignPresent = true;
				logger::warn("[VrFgPump] Foreign Present1 this={} tid={} — forwarding",
					static_cast<void*>(self), GetCurrentThreadId());
			}
			return g_pump.originalPresent1(self, sync, flags, params);
		}
		const bool test = (flags & DXGI_PRESENT_TEST) != 0;
		const UINT index = CurrentNativeBackbuffer();
		Trace("hook1-enter", index, test ? 1 : 0);
		HookCopy copy{};
		if (!test)
			copy = CopyBackbufferOnHook(g_pump, index);
		UINT presentSync = sync;
		if (!test && PumpTearingEnabled()) {
			flags |= DXGI_PRESENT_ALLOW_TEARING;
			presentSync = 0;
		}
		Trace("hook1-orig-present", index, presentSync);
		const HRESULT hr = g_pump.originalPresent1(self, presentSync, flags, params);
		Trace("hook1-orig-done", index, 0, hr);
		if (!test)
			SubmitAfterPresent(copy, hr);
		return hr;
	}

	DXGI_FORMAT UiFormat(const PumpState& pump)
	{
		if (!pump.ui || !pump.ui->resource11)
			return DXGI_FORMAT_UNKNOWN;
		D3D11_TEXTURE2D_DESC desc{};
		pump.ui->resource11->GetDesc(&desc);
		return desc.Format;
	}

	bool SignalGuides(PumpState& pump, UINT64& outFence)
	{
		outFence = NextFence();
		if (FAILED(pump.context11->Signal(pump.fence11.get(), outFence)))
			return false;
		return SUCCEEDED(pump.queue->Wait(pump.fence12.get(), outFence));
	}

	bool CloneVtableAndHook(void* instance)
	{
		if (!instance)
			return false;
		auto*** vptr = reinterpret_cast<void***>(instance);
		void** oldVt = *vptr;
		if (g_pump.clonedVtable && oldVt == g_pump.clonedVtable.get())
			return g_pump.originalPresent != nullptr;
		auto clone = std::make_unique<void*[]>(kVtableCopy);
		std::memcpy(clone.get(), oldVt, kVtableCopy * sizeof(void*));
		g_pump.originalPresent = reinterpret_cast<PresentFn>(clone[kPresentVtable]);
		clone[kPresentVtable] = reinterpret_cast<void*>(&HookedPresent);
		if (clone[kPresent1Vtable]) {
			g_pump.originalPresent1 = reinterpret_cast<Present1Fn>(clone[kPresent1Vtable]);
			clone[kPresent1Vtable] = reinterpret_cast<void*>(&HookedPresent1);
		}
		*vptr = clone.get();
		g_pump.clonedVtable = std::move(clone);
		return g_pump.originalPresent != nullptr;
	}

	bool InstallNativePresentHook(PumpState& pump)
	{
		if (!pump.nativeSwapChain)
			return false;
		pump.nativeSwapChain->QueryInterface(IID_PPV_ARGS(pump.nativeSwap1.put()));
		pump.nativeSwapChain->QueryInterface(IID_PPV_ARGS(pump.nativeSwap3.put()));
		if (!CloneVtableAndHook(pump.nativeSwapChain.get())) {
			logger::warn("[VrFgPump] Native Present clone-hook failed");
			return false;
		}
		logger::info("[VrFgPump] Native Present hooked on cloned vtable native={}",
			static_cast<void*>(pump.nativeSwapChain.get()));
		return true;
	}

	HWND CreatePumpWindow()
	{
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = kWindowClass;
		RegisterClassExW(&wc);
		HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
			kWindowClass, L"OpenShaders VR FG", WS_POPUP, 0, 0,
			static_cast<int>(kWindowWidth), static_cast<int>(kWindowHeight),
			nullptr, nullptr, wc.hInstance, nullptr);
		if (hwnd) {
			ShowWindow(hwnd, SW_SHOWNOACTIVATE);
			SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, kWindowWidth, kWindowHeight,
				SWP_NOACTIVATE | SWP_SHOWWINDOW);
			UpdateWindow(hwnd);
			logger::info("[VrFgPump] hwnd={} vis={} 8x8",
				static_cast<void*>(hwnd), IsWindowVisible(hwnd) ? 1 : 0);
		}
		return hwnd;
	}

	bool BindStreamlineDevice(IDXGIAdapter* adapter, PumpState& pump)
	{
		auto& sl = globals::features::upscaling.streamlineDX12;
		// Flat DLSS-G also upgrades the device in place, then rebuilds the queue
		// (Upscaling.cpp). Do not skip this — a native-only device leaves FG SL-invisible.
		sl.SetD3DDevice12(pump.device12.get());
		sl.CheckFeatures(adapter);
		sl.PostDevice();
		if (!sl.featureDLSSG || !sl.slUpgradeInterface || !sl.slDLSSGSetOptions)
			return false;
		ID3D12Device* raw = pump.device12.detach();
		const sl::Result upgradeResult = sl.slUpgradeInterface(reinterpret_cast<void**>(&raw));
		if (upgradeResult != sl::Result::eOk || !raw) {
			if (raw)
				pump.device12.attach(raw);
			logger::error("[VrFgPump] slUpgradeInterface(device) failed: {}", magic_enum::enum_name(upgradeResult));
			return false;
		}
		pump.device12.attach(raw);
		pump.queue = nullptr;
		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(pump.device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(pump.queue.put()))))
			return false;
		pump.queue->SetName(L"VrFgPump::Queue");
		return true;
	}

	bool CreateSharedFence(PumpState& pump)
	{
		if (FAILED(pump.device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(pump.fence12.put()))))
			return false;
		HANDLE handle = nullptr;
		if (FAILED(pump.device12->CreateSharedHandle(pump.fence12.get(), nullptr, GENERIC_ALL, nullptr, &handle)))
			return false;
		const HRESULT opened = pump.device11->OpenSharedFence(handle, IID_PPV_ARGS(pump.fence11.put()));
		CloseHandle(handle);
		pump.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		return SUCCEEDED(opened) && pump.fenceEvent;
	}

	std::unique_ptr<WrappedResource> MakeWrapped(uint32_t width, uint32_t height, DXGI_FORMAT format,
		UINT bind, const char* name, PumpState& pump)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = bind;
		return std::make_unique<WrappedResource>(desc, pump.device11.get(), pump.device12.get(), name);
	}

	bool CreateSubmitPresent(PumpState& pump)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = pump.width;
		desc.Height = pump.height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = pump.format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		for (UINT i = 0; i < 2; ++i) {
			if (FAILED(pump.device11->CreateTexture2D(&desc, nullptr, pump.submitPresent[i].put())))
				return false;
			Util::SetResourceName(pump.submitPresent[i].get(), "VrFgPump::SubmitPresent[%u]", i);
		}
		if (SUCCEEDED(pump.context11->QueryInterface(IID_PPV_ARGS(pump.mt11.put()))) && pump.mt11)
			pump.mt11->SetMultithreadProtected(TRUE);
		return true;
	}

	bool CreateSwapChainForStreamline(IDXGIAdapter* adapter, PumpState& pump)
	{
		auto& sl = globals::features::upscaling.streamlineDX12;
		if (!sl.slUpgradeInterface)
			return false;

		// Same order as Flat ARISE: upgrade the factory, then create. A bare
		// CreateDXGIFactory2 leaves NvPresent64 with the SL queue proxy.
		IDXGIFactory4* factoryRaw = nullptr;
		if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factoryRaw))))
			return false;
		const sl::Result factoryResult = sl.slUpgradeInterface(reinterpret_cast<void**>(&factoryRaw));
		if (factoryResult != sl::Result::eOk || !factoryRaw) {
			logger::error("[VrFgPump] slUpgradeInterface(factory) failed: {}", magic_enum::enum_name(factoryResult));
			if (factoryRaw)
				factoryRaw->Release();
			return false;
		}
		winrt::com_ptr<IDXGIFactory4> factory;
		factory.attach(factoryRaw);

		BOOL tearing = FALSE;
		winrt::com_ptr<IDXGIFactory5> factory5;
		if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(factory5.put()))))
			factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
		pump.allowTearing = tearing == TRUE;

		DXGI_SWAP_CHAIN_DESC1 desc{};
		desc.Width = pump.width;
		desc.Height = pump.height;
		desc.Format = pump.format;
		desc.SampleDesc.Count = 1;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = kBackBufferCount;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		desc.Scaling = DXGI_SCALING_STRETCH;
		if (pump.allowTearing)
			desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

		winrt::com_ptr<IDXGISwapChain1> swap1;
		if (FAILED(factory->CreateSwapChainForHwnd(pump.queue.get(), pump.hwnd, &desc, nullptr, nullptr, swap1.put())))
			return false;
		if (FAILED(swap1->QueryInterface(IID_PPV_ARGS(pump.slSwapChain.put()))))
			return false;

		if (sl.slGetNativeInterface) {
			void* native = nullptr;
			if (sl.slGetNativeInterface(pump.slSwapChain.get(), &native) == sl::Result::eOk && native)
				pump.nativeSwapChain.attach(static_cast<IDXGISwapChain*>(native));
		}
		if (!pump.nativeSwapChain)
			pump.slSwapChain->QueryInterface(IID_PPV_ARGS(pump.nativeSwapChain.put()));
		InstallNativePresentHook(pump);

		IDXGISwapChain* bufferChain = pump.nativeSwapChain ? pump.nativeSwapChain.get() :
			static_cast<IDXGISwapChain*>(pump.slSwapChain.get());
		DXGI_SWAP_CHAIN_DESC liveDesc{};
		bufferChain->GetDesc(&liveDesc);
		pump.swapBufferCount = (std::min)(liveDesc.BufferCount, kMaxSwapchainBuffers);
		for (UINT i = 0; i < pump.swapBufferCount; ++i) {
			if (FAILED(bufferChain->GetBuffer(i, IID_PPV_ARGS(pump.backBuffers[i].put())))) {
				pump.swapBufferCount = i;
				break;
			}
			pump.backBuffers[i]->SetName((std::wstring(L"VrFgPump::BackBuffer[") + std::to_wstring(i) + L"]").c_str());
		}
		if (!pump.swapBufferCount)
			return false;
		pump.frameIndex = pump.slSwapChain->GetCurrentBackBufferIndex();
		logger::info("[VrFgPump] Factory-created SL swapchain sl={} native={} buffers={} tearing={}",
			static_cast<void*>(pump.slSwapChain.get()), static_cast<void*>(pump.nativeSwapChain.get()),
			pump.swapBufferCount, pump.allowTearing);
		return true;
	}

	bool EnsurePump(uint32_t width, uint32_t height, DXGI_FORMAT format)
	{
		auto& pump = g_pump;
		if (pump.failed)
			return false;
		if (pump.active && pump.width == width && pump.height == height && pump.format == format)
			return true;
		if (pump.active) {
			logger::warn("[VrFgPump] Size/format changed {}x{} -> {}x{}; keeping existing pump",
				pump.width, pump.height, width, height);
			return pump.width == width && pump.height == height;
		}

		if (!globals::d3d::device || !globals::d3d::context)
			return false;
		if (FAILED(globals::d3d::device->QueryInterface(IID_PPV_ARGS(pump.device11.put()))) ||
			FAILED(globals::d3d::context->QueryInterface(IID_PPV_ARGS(pump.context11.put()))))
			return false;

		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		if (FAILED(globals::d3d::device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()))))
			return false;
		winrt::com_ptr<IDXGIAdapter> adapter;
		if (FAILED(dxgiDevice->GetAdapter(adapter.put())))
			return false;
		DXGI_ADAPTER_DESC adapterDesc{};
		adapter->GetDesc(&adapterDesc);
		if (adapterDesc.VendorId != Streamline::kNvidiaVendorId) {
			logger::warn("[VrFgPump] Adapter is not NVIDIA; pump disabled");
			pump.failed = true;
			return false;
		}

		if (FAILED(D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(pump.device12.put()))))
			return false;
		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(pump.device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(pump.queue.put()))))
			return false;

		pump.width = width;
		pump.height = height;
		if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
			format = DXGI_FORMAT_R8G8B8A8_UNORM;
		else if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
			format = DXGI_FORMAT_B8G8R8A8_UNORM;
		pump.format = format;

		if (!BindStreamlineDevice(adapter.get(), pump)) {
			pump.failed = true;
			return false;
		}

		for (UINT i = 0; i < kBackBufferCount; ++i) {
			if (FAILED(pump.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
					IID_PPV_ARGS(pump.allocators[i].put()))) ||
				FAILED(pump.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
					pump.allocators[i].get(), nullptr, IID_PPV_ARGS(pump.lists[i].put()))))
				return false;
			pump.lists[i]->Close();
		}
		for (UINT i = 0; i < 2; ++i) {
			if (FAILED(pump.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
					IID_PPV_ARGS(pump.hookAllocators[i].put()))) ||
				FAILED(pump.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
					pump.hookAllocators[i].get(), nullptr, IID_PPV_ARGS(pump.hookLists[i].put()))))
				return false;
			pump.hookLists[i]->Close();
		}

		if (!CreateSharedFence(pump))
			return false;

		pump.hwnd = CreatePumpWindow();
		if (!pump.hwnd)
			return false;
		if (!CreateSwapChainForStreamline(adapter.get(), pump)) {
			pump.failed = true;
			return false;
		}

		const UINT colorBind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
		const UINT depthBind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
		pump.hudLess = MakeWrapped(width, height, format, colorBind, "VrFgPump::HUDLess", pump);
		pump.ui = MakeWrapped(width, height, format, colorBind, "VrFgPump::UI", pump);
		for (UINT i = 0; i < kSubmitRing; ++i) {
			pump.submitRing[i] = MakeWrapped(width, height, format, colorBind,
				("VrFgPump::SubmitRing[" + std::to_string(i) + "]").c_str(), pump);
		}
		if (!CreateSubmitPresent(pump))
			return false;
		pump.depth = MakeWrapped(width, height, DXGI_FORMAT_R32_FLOAT, depthBind, "VrFgPump::Depth", pump);
		if (pump.ui && pump.ui->rtv) {
			const float clearUi[4]{};
			globals::d3d::context->ClearRenderTargetView(pump.ui->rtv, clearUi);
		}

		auto* renderer = globals::game::renderer;
		DXGI_FORMAT motionFormat = DXGI_FORMAT_R16G16_FLOAT;
		if (renderer) {
			D3D11_TEXTURE2D_DESC motionDesc{};
			renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].texture->GetDesc(&motionDesc);
			motionFormat = motionDesc.Format;
		}
		pump.motion = MakeWrapped(width, height, motionFormat, D3D11_BIND_SHADER_RESOURCE, "VrFgPump::Motion", pump);

		pump.presentEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!pump.presentEvent)
			return false;
		pump.presentThread = std::thread([] {
			for (;;) {
				WaitForSingleObject(g_pump.presentEvent, INFINITE);
				if (g_pump.workerStop.load(std::memory_order_relaxed))
					break;
				WorkerDoorbell(g_pump);
			}
		});
		pump.watchdogThread = std::thread([] {
			uint32_t fires = 0;
			for (;;) {
				Sleep(200);
				if (g_pump.workerStop.load(std::memory_order_relaxed))
					break;
				if (!g_pump.active || !g_pump.presentBusy.load(std::memory_order_relaxed))
					continue;
				const uint32_t ms = BusyMs();
				if (ms < 200)
					continue;
				++fires;
				if (fires <= 8 || (fires % 10) == 0)
					Trace("watchdog-busy", ms, fires);
			}
		});

		pump.active = true;
		auto& upscaling = globals::features::upscaling;
		logger::info("[VrFgPump] Private SL Present pump ready {}x{} fmt={} frameGenerationMode={} forceEnable={}",
			width, height, static_cast<int>(format), upscaling.settings.frameGenerationMode,
			upscaling.settings.frameGenerationForceEnable);
		return true;
	}

	bool CopyGuides(PumpState& pump, ID3D11Texture2D* hudLessSbs, UINT64& outFence)
	{
		auto* context = globals::d3d::context;
		auto* renderer = globals::game::renderer;
		outFence = 0;
		if (!context || !renderer || !pump.hudLess || !pump.depth || !pump.motion)
			return false;

		CS_GPU_PASS("VrFgPump::CopyGuides");
		context->CopyResource(pump.hudLess->resource11, hudLessSbs);
		auto& motion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
		if (motion.texture)
			context->CopyResource(pump.motion->resource11, motion.texture);
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		if (depth.depthSRV && pump.depth->uav)
			FoveatedRenderImpl::Ops::CopyDepthSubrect(depth.depthSRV, pump.depth->uav, 0, 0, 0, 0, pump.width, pump.height);
		return SignalGuides(pump, outFence);
	}

	bool RecordHudLessCopy(PumpState& pump, ID3D12GraphicsCommandList* list)
	{
		if (pump.frameIndex >= pump.swapBufferCount || !pump.backBuffers[pump.frameIndex] || !pump.hudLess)
			return false;
		auto* src = pump.hudLess->resource.get();
		auto* dst = pump.backBuffers[pump.frameIndex].get();
		D3D12_RESOURCE_BARRIER barriers[2]{
			Transition(src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
			Transition(dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
		};
		list->ResourceBarrier(2, barriers);
		list->CopyResource(dst, src);
		barriers[0] = Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
		barriers[1] = Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
		list->ResourceBarrier(2, barriers);
		return true;
	}

	DXGI_FORMAT MotionFormat(const PumpState& pump)
	{
		if (!pump.motion || !pump.motion->resource11)
			return DXGI_FORMAT_R16G16_FLOAT;
		D3D11_TEXTURE2D_DESC desc{};
		pump.motion->resource11->GetDesc(&desc);
		return desc.Format;
	}

	void WorkerDoorbell(PumpState& pump)
	{
		Trace("worker-enter", pump.appPresents);
		WaitFenceCpu(pump.presentFence.load(std::memory_order_relaxed));
		Trace("worker-guides-waited", pump.appPresents);
		auto& sl = globals::features::upscaling.streamlineDX12;
		sl.EnsureFrameToken();
		sl.EmitPCLMarker(sl::PCLMarker::eSimulationEnd);
		sl.EmitPCLMarker(sl::PCLMarker::eRenderSubmitStart);

		pump.frameIndex = CurrentNativeBackbuffer();
		const UINT listSlot = pump.workerListSlot.load(std::memory_order_relaxed);
		auto* allocator = pump.allocators[listSlot].get();
		auto* list = pump.lists[listSlot].get();
		if (!allocator || !list || FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr))) {
			Trace("worker-list-reset");
			pump.presentBusy.store(false, std::memory_order_release);
			return;
		}
		if (!RecordHudLessCopy(pump, list)) {
			Trace("worker-hudless-copy");
			list->Close();
			pump.presentBusy.store(false, std::memory_order_release);
			return;
		}

		const bool constantsOk = pump.workerConstantsOk.load(std::memory_order_relaxed);
		const bool enableG = pump.workerEnableG.load(std::memory_order_relaxed);
		if (constantsOk && sl.frameToken) {
			sl.TagDX12Resources(*sl.frameToken, list,
				pump.depth ? pump.depth->resource.get() : nullptr,
				pump.motion ? pump.motion->resource.get() : nullptr,
				pump.hudLess ? pump.hudLess->resource.get() : nullptr,
				pump.ui ? pump.ui->resource.get() : nullptr,
				pump.width, pump.height, pump.width, pump.height);
			sl.ConfigureDLSSG(enableG,
				pump.width, pump.height, pump.width, pump.height,
				DXGI_FORMAT_R32_FLOAT, MotionFormat(pump), pump.format, UiFormat(pump),
				pump.swapBufferCount);
		} else {
			sl.ConfigureDLSSG(false);
		}

		if (FAILED(list->Close())) {
			Trace("worker-list-close");
			pump.presentBusy.store(false, std::memory_order_release);
			return;
		}
		ID3D12CommandList* lists[]{ list };
		pump.queue->ExecuteCommandLists(1, lists);
		sl.EmitPCLMarker(sl::PCLMarker::eRenderSubmitEnd);
		sl.EmitPCLMarker(sl::PCLMarker::ePresentStart);
		const UINT presentFlags = PumpTearingEnabled() ? DXGI_PRESENT_ALLOW_TEARING : 0;
		Trace("worker-present", pump.frameIndex, presentFlags);
		const HRESULT presented = pump.slSwapChain->Present(0, presentFlags);
		Trace("worker-present-done", pump.frameIndex, presentFlags, presented);
		sl.EmitPCLMarker(sl::PCLMarker::ePresentEnd);
		if (FAILED(presented)) {
			logger::warn("[VrFgPump] Worker Present failed HRESULT 0x{:08X} {}",
				static_cast<uint32_t>(presented), HrName(presented));
		}
		PumpWindowMessages(pump);
		pump.presentBusy.store(false, std::memory_order_release);
		Trace("worker-idle", pump.appPresents, 0, presented);
	}

	bool KickDoorbell(PumpState& pump)
	{
		auto& sl = globals::features::upscaling.streamlineDX12;
		auto& upscaling = globals::features::upscaling;
		pump.workerConstantsOk.store(sl.CheckFrameConstants(sl.viewport), std::memory_order_relaxed);
		pump.workerEnableG.store(pump.appPresents > 0 &&
				upscaling.ShouldUseFrameGenerationThisFrame() && sl.featureDLSSG,
			std::memory_order_relaxed);
		pump.workerListSlot.store(pump.appPresents % kBackBufferCount, std::memory_order_relaxed);
		pump.kickQpc.store(NowQpc(), std::memory_order_relaxed);
		pump.presentBusy.store(true, std::memory_order_release);
		Trace("kick", pump.appPresents, pump.workerEnableG.load(std::memory_order_relaxed) ? 1 : 0);
		if (!SetEvent(pump.presentEvent)) {
			pump.presentBusy.store(false, std::memory_order_release);
			Trace("kick-fail", pump.appPresents);
			logger::warn("[VrFgPump] Failed to wake Present worker");
			return false;
		}
		++pump.appPresents;
		if (!pump.loggedPresent) {
			logger::info("[VrFgPump] Inject kicked worker (hook Submits, game thread does not)");
			pump.loggedPresent = true;
		}
		if ((pump.appPresents % 120) == 1) {
			logger::info("[VrFgPump] G doorbell hook={} app={} (not VDXR fps) dlssgPresented={} status={}",
				pump.hookSubmits.load(std::memory_order_relaxed), pump.appPresents,
				sl.lastDLSSGFramesPresented, magic_enum::enum_name(sl.lastDLSSGStatus));
		}
		return true;
	}
#endif
}

bool VrFgPump::WantsPump()
{
#ifdef ENABLE_SKYRIM_VR
	if (!globals::game::isVR)
		return false;
	auto& upscaling = globals::features::upscaling;
	if (!upscaling.settings.frameGenerationMode)
		return false;
	return upscaling.streamlineDX12.initialized && upscaling.streamlineDX12.slUpgradeInterface;
#else
	return false;
#endif
}

bool VrFgPump::IsActive()
{
#ifdef ENABLE_SKYRIM_VR
	return g_pump.active && !g_pump.failed;
#else
	return false;
#endif
}

const char* VrFgPump::LastStep()
{
	return Snapshot().lastStep;
}

VrFgPump::DebugSnapshot VrFgPump::Snapshot()
{
	DebugSnapshot snap{};
#ifdef ENABLE_SKYRIM_VR
	auto* step = g_pump.lastStep.load(std::memory_order_acquire);
	snap.lastStep = step ? step : "";
	snap.active = g_pump.active && !g_pump.failed;
	snap.busy = g_pump.presentBusy.load(std::memory_order_relaxed);
	snap.windowVisible = g_pump.hwnd && IsWindowVisible(g_pump.hwnd);
	snap.appPresents = g_pump.appPresents;
	snap.hookSubmits = g_pump.hookSubmits.load(std::memory_order_relaxed);
	snap.lastHr = g_pump.lastHr.load(std::memory_order_relaxed);
	snap.busyMs = BusyMs();
#endif
	return snap;
}

bool VrFgPump::PresentOwnedSbs(ID3D11Texture2D* hudLessSbs, std::uint32_t width, std::uint32_t height)
{
#ifdef ENABLE_SKYRIM_VR
	if (!WantsPump() || !hudLessSbs)
		return false;
	if (!globals::features::upscaling.ShouldUseFrameGenerationThisFrame())
		return false;
	(void)width;
	(void)height;
	D3D11_TEXTURE2D_DESC desc{};
	hudLessSbs->GetDesc(&desc);
	if (!desc.Width || !desc.Height)
		return false;
	std::scoped_lock lock(g_pump.mutex);
	if (!EnsurePump(desc.Width, desc.Height, desc.Format))
		return false;
	CS_GPU_PASS("VrFgPump::Inject");
	// A still-running doorbell must not swallow: the engine Submit keeps this frame on the HMD.
	if (g_pump.presentBusy.load(std::memory_order_acquire)) {
		Trace("inject-busy", g_pump.appPresents, BusyMs());
		if ((g_pump.appPresents % 120) == 1) {
			logger::info("[VrFgPump] Present worker busy; leaving engine Submit live hook={} app={}",
				g_pump.hookSubmits.load(std::memory_order_relaxed), g_pump.appPresents);
		}
		return false;
	}
	Trace("inject-copy", desc.Width, desc.Height);
	UINT64 guidesFence = 0;
	if (!CopyGuides(g_pump, hudLessSbs, guidesFence)) {
		Trace("inject-copy-fail", desc.Width, desc.Height);
		return false;
	}
	g_pump.presentFence.store(guidesFence, std::memory_order_relaxed);
	if (!KickDoorbell(g_pump))
		return false;
	VrSubmitDlssg::ArmWorldSwallow();
	return true;
#else
	(void)hudLessSbs;
	(void)width;
	(void)height;
	return false;
#endif
}
