#include "PCH.h"
#include "GPresent.h"

#ifdef ENABLE_SKYRIM_VR

#	include "../../../Globals.h"
#	include "../../../GpuPass.h"
#	include "../../Upscaling.h"
#	include "../VrBeat/Metronome.h"
#	include "../VrCount/PreAfter.h"
#	include "../VrPose/Absolute.h"
#	include "../VrSwitch/Master.h"

#	include <array>
#	include <atomic>
#	include <cmath>
#	include <cstring>
#	include <mutex>

#	include <Windows.h>
#	include <dxgi1_5.h>
#	include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
	std::atomic<bool> ready{ false };
	std::atomic<bool> failed{ false };

	ID3D12Device* device = nullptr;
	ID3D12CommandQueue* queue = nullptr;
	HWND hwnd = nullptr;
	ComPtr<IDXGISwapChain3> swapChain;
	ComPtr<ID3D12Resource> bb[3];
	IDXGISwapChain* hooked = nullptr;
	void* clonedVtbl[48]{};
	using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	PresentFn origPresent = nullptr;

	UINT width = 0;
	UINT height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;

	// Per-Kick native-fire tracking for GPresent.h's BeginKickPresent contract.
	std::mutex kickPoseMutex;
	VrPose::Body prevKickPose{};
	VrPose::Body curKickPose{};
	std::atomic<int> fireIndexThisKick{ 0 };

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

	void OnPresent(IDXGISwapChain* self)
	{
		if (!VrSwitch::On() || !ready.load(std::memory_order_acquire) || !self)
			return;
		ComPtr<IDXGISwapChain3> sc3;
		if (FAILED(self->QueryInterface(IID_PPV_ARGS(&sc3))))
			return;
		const UINT index = sc3->GetCurrentBackBufferIndex();
		ComPtr<ID3D12Resource> src;
		if (FAILED(sc3->GetBuffer(index, IID_PPV_ARGS(&src))) || !src)
			return;
		// No CS_GPU_PASS: Profiler's internal vectors have no lock and are
		// written every frame by the render thread's own instrumentation.
		// This fires off the Doorbell worker thread; a concurrent push/
		// reallocate there crashed inside std::vector<Profiler::TimerResult>.
		//
		// AddAfter() here is diagnostic-only (feeds X4 G kick= P=/A=).
		// Classification no longer relies on Pre/After index math (that
		// silently assumed After(0)=Pre(0) and was several seconds stale by
		// the time DLSS-G actually settled into 2:1 -- the giant-screen bug).
		// Instead: BeginKickPresent (called from PresentGate right before
		// this Kick's sc->Present()) reset fireIndexThisKick to 0 and rolled
		// curKickPose into prevKickPose. The 1st native fire under one
		// Kick's Present is, per the "generated then real" order the DLSS-G
		// Programming Guide documents, the interpolated one -- give it the
		// midpoint of the previous and this Kick's real pose and its own
		// dedicated Submit period (OfferGInterpolated) instead of letting it
		// share the reactive mailbox the 2nd (real) fire below also writes
		// (single buffer, no ping-pong -- it would just be overwritten
		// moments later and never reach the compositor, which is exactly why
		// G was pure overhead with no realized fps gain before this).
		VrCount::AddAfter();
		VrPose::Body prev{}, cur{};
		{
			std::lock_guard lock(kickPoseMutex);
			prev = prevKickPose;
			cur = curKickPose;
		}
		const int fireIndex = fireIndexThisKick.fetch_add(1, std::memory_order_acq_rel) + 1;
		if (fireIndex == 1 && prev.valid && cur.valid) {
			vr::HmdMatrix34_t mid = InterpolatePose(prev.m, cur.m, 0.5f);
			VrBeat::OfferGInterpolated(src.Get(), width, height, format, mid);
			// Also keep the reactive mailbox fed in case no 2nd fire follows
			// this Kick (warm-up / skipped insertion, delta==1) -- then this
			// was actually the only (real) native for this Kick and needs to
			// reach the compositor through the normal engine-Wait path too.
			VrBeat::CopySbs(src.Get(), width, height, format, cur.m, cur.valid);
			return;
		}
		if (cur.valid) {
			if (!VrBeat::CopySbs(src.Get(), width, height, format, cur.m, true))
				return;
		} else if (!VrBeat::CopySbs(src.Get(), width, height, format))
			return;
	}

	HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain* self, UINT sync, UINT flags)
	{
		if (!self || self != hooked)
			return E_FAIL;
		OnPresent(self);
		return origPresent ? origPresent(self, sync, flags) : E_FAIL;
	}

	void HookNative(IDXGISwapChain* target)
	{
		if (!target || origPresent)
			return;
		void*** object = reinterpret_cast<void***>(target);
		void** realVtbl = *object;
		std::memcpy(clonedVtbl, realVtbl, sizeof(clonedVtbl));
		origPresent = reinterpret_cast<PresentFn>(clonedVtbl[8]);
		clonedVtbl[8] = reinterpret_cast<void*>(&PresentThunk);
		*object = clonedVtbl;
		hooked = target;
		logger::info("[VrGWrap] Present hooked");
	}
}

bool VrGWrap::Ensure(ID3D12Device* inDevice, ID3D12CommandQueue* inQueue, IDXGIAdapter* adapter,
	UINT w, UINT h, DXGI_FORMAT fmt)
{
	if (ready.load(std::memory_order_acquire))
		return true;
	if (failed.load(std::memory_order_acquire))
		return false;
	auto fail = [&](const char* why) {
		logger::warn("[VrGWrap] failed: {}", why);
		failed.store(true, std::memory_order_release);
		Shutdown();
		return false;
	};
	auto& sl = globals::features::upscaling.streamlineDX12;
	if (!inDevice || !inQueue || !adapter)
		return fail("no D3D12");
	if (!sl.initialized || !sl.featureDLSSG || !sl.slUpgradeInterface)
		return fail("DLSS-G not bound");
	if (w < 2 || h == 0)
		return fail("SBS too small");
	if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
		fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
	else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
		fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
	fmt = DXGI_FORMAT_R8G8B8A8_UNORM;

	device = inDevice;
	queue = inQueue;
	VrBeat::BindGpu(inDevice, inQueue);

	WNDCLASSW wc{};
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"VrGWrapPresent";
	RegisterClassW(&wc);
	hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"VrGWrapPresent", L"OS",
		WS_POPUP | WS_VISIBLE, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
	if (!hwnd)
		return fail("CreateWindowExW");
	SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 8, 8, SWP_NOACTIVATE | SWP_SHOWWINDOW);

	ComPtr<IDXGIFactory4> factory;
	if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
		return fail("factory GetParent");
	IDXGIFactory4* factoryRaw = factory.Detach();
	if (sl.slUpgradeInterface(reinterpret_cast<void**>(&factoryRaw)) != sl::Result::eOk) {
		factory.Attach(factoryRaw);
		return fail("factory slUpgradeInterface");
	}
	factory.Attach(factoryRaw);

	DXGI_SWAP_CHAIN_DESC1 desc{};
	desc.Width = w;
	desc.Height = h;
	desc.Format = fmt;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 3;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
	ComPtr<IDXGISwapChain1> sc1;
	if (FAILED(factory->CreateSwapChainForHwnd(inQueue, hwnd, &desc, nullptr, nullptr, &sc1)))
		return fail("CreateSwapChainForHwnd");
	if (FAILED(sc1.As(&swapChain)))
		return fail("Query IDXGISwapChain3");
	IDXGISwapChain3* scRaw = swapChain.Detach();
	if (sl.slUpgradeInterface(reinterpret_cast<void**>(&scRaw)) != sl::Result::eOk)
		logger::warn("[VrGWrap] swapchain slUpgradeInterface failed");
	swapChain.Attach(scRaw);
	for (UINT i = 0; i < 3; ++i) {
		if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&bb[i]))))
			return fail("GetBuffer");
		bb[i]->SetName(L"VrGWrap::BB");
	}

	void* proxy = swapChain.Get();
	void* native = nullptr;
	if (sl.slGetNativeInterface &&
		sl.slGetNativeInterface(proxy, &native) == sl::Result::eOk && native && native != proxy) {
		HookNative(static_cast<IDXGISwapChain*>(native));
		logger::info("[VrGWrap] hooked slGetNativeInterface Present");
	} else {
		HookNative(swapChain.Get());
		logger::warn("[VrGWrap] Present hook on SL chain");
	}

	width = w;
	height = h;
	format = fmt;
	ready.store(true, std::memory_order_release);
	logger::info("[VrGWrap] fake Present {}x{}", w, h);
	return true;
}

IDXGISwapChain3* VrGWrap::SwapChain()
{
	return swapChain.Get();
}

ID3D12Resource* VrGWrap::BackBuffer(UINT index)
{
	return index < 3 ? bb[index].Get() : nullptr;
}

bool VrGWrap::Ready()
{
	return ready.load(std::memory_order_acquire);
}

void VrGWrap::BeginKickPresent(const VrPose::Body& pose)
{
	{
		std::lock_guard lock(kickPoseMutex);
		prevKickPose = curKickPose;
		curKickPose = pose;
	}
	fireIndexThisKick.store(0, std::memory_order_release);
}

void VrGWrap::Pump()
{
	MSG msg{};
	while (PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE))
		DispatchMessageW(&msg);
}

void VrGWrap::Shutdown()
{
	ready.store(false, std::memory_order_release);
	origPresent = nullptr;
	hooked = nullptr;
	for (auto& buffer : bb)
		buffer.Reset();
	swapChain.Reset();
	if (hwnd) {
		DestroyWindow(hwnd);
		hwnd = nullptr;
	}
	device = nullptr;
	queue = nullptr;
}

#endif
