#include "VrSubmitDlssg.h"

#include "../../Globals.h"
#include "../Upscaling.h"
#include "DX12SwapChain.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>

namespace
{
	bool installed = false;
	bool ready = false;
	bool failed = false;
	HWND hiddenWindow = nullptr;
	bool logged = false;

	LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	HWND CreateHiddenWindow(uint32_t width, uint32_t height)
	{
		static bool registered = false;
		if (!registered) {
			WNDCLASSEXW wc{};
			wc.cbSize = sizeof(wc);
			wc.lpfnWndProc = HiddenWndProc;
			wc.hInstance = GetModuleHandleW(nullptr);
			wc.lpszClassName = L"OpenShadersVrDlssg";
			if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
				return nullptr;
			registered = true;
		}
		return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"OpenShadersVrDlssg", L"",
			WS_POPUP, 0, 0, static_cast<int>(width), static_cast<int>(height),
			nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	}

	IDXGIAdapter* GetDeviceAdapter(ID3D11Device* device)
	{
		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		if (!device || FAILED(device->QueryInterface(dxgiDevice.put())))
			return nullptr;
		IDXGIAdapter* adapter = nullptr;
		if (FAILED(dxgiDevice->GetAdapter(&adapter)))
			return nullptr;
		return adapter;
	}

	DXGI_FORMAT StripSrgb(DXGI_FORMAT format)
	{
		if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
			return DXGI_FORMAT_R8G8B8A8_UNORM;
		if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
			return DXGI_FORMAT_B8G8R8A8_UNORM;
		return format;
	}

	bool RecreateUpgradedQueue(DX12SwapChain& sc)
	{
		if (!sc.d3d12Device)
			return false;
		sc.commandQueue = nullptr;
		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
		return SUCCEEDED(sc.d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(sc.commandQueue.put())));
	}

	bool EnsureChain()
	{
		if (ready)
			return true;
		if (failed || !globals::game::isVR)
			return false;
		auto& upscaling = globals::features::upscaling;
		if (upscaling.settings.frameGenerationMode == 0)
			return false;
		if (!upscaling.streamlineDX12.initialized || !globals::d3d::device || !globals::d3d::context)
			return false;
		auto* renderer = globals::game::renderer;
		if (!renderer)
			return false;
		auto& total = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTOTAL];
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		auto& motion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
		if (!total.texture || !main.texture || !motion.texture)
			return false;

		D3D11_TEXTURE2D_DESC totalDesc{};
		total.texture->GetDesc(&totalDesc);
		const uint32_t width = totalDesc.Width;
		const uint32_t height = totalDesc.Height;
		if (!width || !height)
			return false;

		winrt::com_ptr<IDXGIAdapter> adapter;
		adapter.attach(GetDeviceAdapter(globals::d3d::device));
		if (!adapter) {
			failed = true;
			logger::error("[VrSubmitDlssg] No DXGI adapter");
			return false;
		}

		DXGI_ADAPTER_DESC adapterDesc{};
		adapter->GetDesc(&adapterDesc);
		if (adapterDesc.VendorId != Streamline::kNvidiaVendorId) {
			failed = true;
			logger::error("[VrSubmitDlssg] DLSS-G requires NVIDIA");
			return false;
		}

		if (!hiddenWindow)
			hiddenWindow = CreateHiddenWindow(width, height);
		if (!hiddenWindow) {
			failed = true;
			logger::error("[VrSubmitDlssg] Hidden window failed");
			return false;
		}

		auto& sc = upscaling.dx12SwapChain;
		try {
			sc.CreateD3D12Device(adapter.get());
			sc.SetD3D11Device(globals::d3d::device);
			sc.SetD3D11DeviceContext(globals::d3d::context);
			upscaling.streamlineDX12.SetD3DDevice12(sc.d3d12Device.get());
			upscaling.streamlineDX12.CheckFeatures(adapter.get());
			upscaling.streamlineDX12.PostDevice();
			if (!upscaling.streamlineDX12.featureDLSSG || !upscaling.streamlineDX12.slUpgradeInterface) {
				failed = true;
				logger::error("[VrSubmitDlssg] DLSS-G unavailable on DX12 Streamline");
				return false;
			}
			upscaling.streamlineDX12.slUpgradeInterface(reinterpret_cast<void**>(&sc.d3d12Device));
			if (!RecreateUpgradedQueue(sc)) {
				failed = true;
				logger::error("[VrSubmitDlssg] Upgraded command queue failed");
				return false;
			}
			upscaling.streamlineDX12.SetD3DDevice12(sc.d3d12Device.get());

			DXGI_SWAP_CHAIN_DESC desc{};
			desc.BufferDesc.Width = width;
			desc.BufferDesc.Height = height;
			desc.BufferDesc.Format = StripSrgb(totalDesc.Format);
			desc.BufferDesc.RefreshRate.Numerator = 90;
			desc.BufferDesc.RefreshRate.Denominator = 1;
			desc.SampleDesc.Count = 1;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.BufferCount = 3;
			desc.OutputWindow = hiddenWindow;
			desc.Windowed = TRUE;
			desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			sc.CreateSwapChainDirect(adapter.get(), desc);
			sc.CreateInterop();
			sc.CreateSharedResources();
		} catch (...) {
			sc.useDLSSG = false;
			failed = true;
			logger::error("[VrSubmitDlssg] Hidden swapchain create failed");
			return false;
		}

		ready = sc.useDLSSG && sc.swapChain && sc.swapChainBufferWrapped &&
			sc.swapChainBufferWrapped->resource11;
		if (!ready) {
			sc.useDLSSG = false;
			failed = true;
			logger::error("[VrSubmitDlssg] Hidden swapchain incomplete");
			return false;
		}
		logger::info("[VrSubmitDlssg] Single-thread full-eye DLSS-G chain {}x{}", width, height);
		return true;
	}

	bool SameExtent(ID3D11Resource* a, ID3D11Resource* b)
	{
		winrt::com_ptr<ID3D11Texture2D> ta, tb;
		if (!a || !b || FAILED(a->QueryInterface(ta.put())) || FAILED(b->QueryInterface(tb.put())))
			return false;
		D3D11_TEXTURE2D_DESC da{}, db{};
		ta->GetDesc(&da);
		tb->GetDesc(&db);
		return da.Width == db.Width && da.Height == db.Height && da.Format == db.Format;
	}

	void CopyGuides(Upscaling& upscaling)
	{
		auto& sc = upscaling.dx12SwapChain;
		if (!sc.depthBufferShared12 || !sc.motionVectorBufferShared12)
			return;
		upscaling.CopySharedD3D12Resources();
	}
}

void VrSubmitDlssg::Install()
{
	installed = true;
}

bool VrSubmitDlssg::IsReady()
{
	return ready;
}

#ifdef ENABLE_SKYRIM_VR
bool VrSubmitDlssg::TrySubmit(RE::BSOpenVR* openVR, const vr::Texture_t* texture,
	const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags,
	const REL::Relocation<SubmitFn>& original, vr::EVRCompositorError& outError)
{
	outError = vr::VRCompositorError_None;
	auto& upscaling = globals::features::upscaling;
	if (!globals::game::isVR || !texture || !texture->handle)
		return false;
	if (upscaling.settings.frameGenerationMode == 0)
		return false;
	if (!EnsureChain())
		return false;
	if (!upscaling.ShouldUseFrameGenerationThisFrame())
		return false;

	auto& sc = upscaling.dx12SwapChain;
	auto* src = static_cast<ID3D11Resource*>(texture->handle);
	if (!sc.swapChainBufferWrapped || !sc.swapChainBufferWrapped->resource11 ||
		!SameExtent(src, sc.swapChainBufferWrapped->resource11))
		return false;
	globals::d3d::context->CopyResource(sc.swapChainBufferWrapped->resource11, src);
	CopyGuides(upscaling);
	if (FAILED(sc.Present(0, 0))) {
		outError = original(openVR, texture, bounds, flags);
		return true;
	}

	vr::Texture_t generated = *texture;
	generated.handle = sc.swapChainBufferWrapped->resource11;
	outError = original(openVR, &generated, bounds, flags);
	if (!logged) {
		logger::info("[VrSubmitDlssg] Submit owned; DLSS-G Present is full-eye");
		logged = true;
	}
	return true;
}
#endif
