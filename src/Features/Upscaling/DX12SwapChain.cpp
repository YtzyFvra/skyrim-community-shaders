#include "DX12SwapChain.h"

#include <FidelityFX/api/include/dx12/ffx_api_dx12.hpp>
#include <algorithm>
#include <cmath>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include "../HDRDisplay.h"
#include "../RenderDoc.h"
#include "../Upscaling.h"
#include "FidelityFX.h"
#include "NeuralRendering/ArisePipeline.h"
#include "NeuralRendering/Renderer.h"
#include "Streamline.h"
#include "Utils/D3D.h"

namespace
{
	UINT AlignEvenExtent(UINT value, UINT maximum)
	{
		value = std::clamp(value, 1u, maximum);
		if (value > 1u && (value & 1u))
			--value;
		return value;
	}

	bool IsAriseDeviceFatal(HRESULT result)
	{
		return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
		       result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
	}

	void MapLogicalExtentToPhysical(const DX12SwapChain& owner, UINT& width, UINT& height)
	{
		if (!owner.LogicalWidth() || !owner.LogicalHeight())
			return;
		const double widthScale = static_cast<double>(owner.PhysicalWidth()) / owner.LogicalWidth();
		const double heightScale = static_cast<double>(owner.PhysicalHeight()) / owner.LogicalHeight();
		width = std::max(1u, static_cast<UINT>(std::lround(width * widthScale)));
		height = std::max(1u, static_cast<UINT>(std::lround(height * heightScale)));
	}

	bool IsProxySwapChainInterface(REFIID riid)
	{
		return riid == __uuidof(IUnknown) ||
		       riid == __uuidof(IDXGIObject) ||
		       riid == __uuidof(IDXGIDeviceSubObject) ||
		       riid == __uuidof(IDXGISwapChain) ||
		       riid == __uuidof(IDXGISwapChain1) ||
		       riid == __uuidof(IDXGISwapChain2) ||
		       riid == __uuidof(IDXGISwapChain3) ||
		       riid == __uuidof(IDXGISwapChain4);
	}

	constexpr char kModel3UICompositeShader[] = R"(
Texture2D<float4> uiTexture : register(t0);
Texture2D<float4> sceneTexture : register(t1);
SamplerState linearSampler : register(s0);

struct VertexOutput
{
	float4 position : SV_Position;
	float2 texCoord : TEXCOORD0;
};

VertexOutput VSMain(uint vertexId : SV_VertexID)
{
	VertexOutput output;
	output.texCoord = float2((vertexId << 1) & 2, vertexId & 2);
	output.position = float4(output.texCoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return output;
}

float4 PSMain(VertexOutput input) : SV_Target
{
	float4 scene = sceneTexture.Sample(linearSampler, input.texCoord);
	float4 ui = uiTexture.Sample(linearSampler, input.texCoord);
	return float4(ui.rgb + scene.rgb * (1.0 - ui.a), 1.0);
}

float4 PSUIOnly(VertexOutput input) : SV_Target
{
	return uiTexture.Sample(linearSampler, input.texCoord);
}
)";
}

void DX12SwapChain::CreateD3D12Device(IDXGIAdapter* a_adapter)
{
	// Idempotent: the DLSS-G probe binds Streamline to this device; a second call from
	// the FSR path must not replace it out from under that binding.
	if (d3d12Device)
		return;

	DX::ThrowIfFailed(D3D12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d12Device)));

	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	queueDesc.NodeMask = 0;

	DX::ThrowIfFailed(d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue)));
	commandQueue->SetName(L"DX12SwapChain::CommandQueue");

	for (UINT i = 0; i < kMaxBackBuffers; i++) {
		DX::ThrowIfFailed(d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators[i])));
		DX::ThrowIfFailed(d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[i].get(), nullptr, IID_PPV_ARGS(&commandLists[i])));
		commandLists[i]->Close();
	}
}

void DX12SwapChain::CreateSwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC a_swapChainDesc)
{
	CreateD3D12Device(adapter);

	IDXGIFactory4* dxgiFactory;
	DX::ThrowIfFailed(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory)));

	// Runtime format negotiation for swap chain
	DXGI_FORMAT attemptedFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
	DXGI_FORMAT negotiatedFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
	bool isVR = globals::game::isVR;
	bool fallbackUsed = false;

	// Test R10G10B10A2 support (applies to both VR and non-VR for HDR capability)
	D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_FORMAT_SUPPORT1_RENDER_TARGET, D3D12_FORMAT_SUPPORT2_NONE };
	if (SUCCEEDED(d3d12Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport)))) {
		if ((formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) == 0) {
			logger::warn("[DX12SwapChain] R10G10B10A2_UNORM not supported as render target, falling back to R8G8B8A8_UNORM");
			negotiatedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			fallbackUsed = true;
		} else if (isVR) {
			logger::info("[DX12SwapChain] VR detected with R10G10B10A2_UNORM support, attempting HDR");
		}
	} else {
		logger::warn("[DX12SwapChain] CheckFeatureSupport failed for R10G10B10A2_UNORM, falling back to R8G8B8A8_UNORM");
		negotiatedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		fallbackUsed = true;
	}

	logger::info("[DX12SwapChain] Swap chain format negotiation: attempted={}, negotiated={}, VR={}, fallback={}",
		static_cast<uint32_t>(attemptedFormat),
		static_cast<uint32_t>(negotiatedFormat),
		isVR ? "true" : "false",
		fallbackUsed ? "true" : "false");

	swapChainDesc = {};
	swapChainDesc.Width = a_swapChainDesc.BufferDesc.Width;
	swapChainDesc.Height = a_swapChainDesc.BufferDesc.Height;
	swapChainDesc.Format = negotiatedFormat;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	backBufferCount = 2;
	swapChainDesc.BufferCount = 2;
	swapChainDesc.SwapEffect = a_swapChainDesc.SwapEffect;
	swapChainDesc.Flags = a_swapChainDesc.Flags;

	ffx::CreateContextDescFrameGenerationSwapChainForHwndDX12 ffxSwapChainDesc{};

	ffxSwapChainDesc.desc = &swapChainDesc;
	ffxSwapChainDesc.dxgiFactory = dxgiFactory;
	ffxSwapChainDesc.fullscreenDesc = nullptr;
	ffxSwapChainDesc.gameQueue = commandQueue.get();
	ffxSwapChainDesc.hwnd = a_swapChainDesc.OutputWindow;
	ffxSwapChainDesc.swapchain = &swapChain;

	auto& fidelityFX = globals::features::upscaling.fidelityFX;

	if (ffx::CreateContext(fidelityFX.swapChainContext, nullptr, ffxSwapChainDesc) != ffx::ReturnCode::Ok) {
		logger::critical("[FidelityFX] Failed to create swap chain context!");
	}

	DX::ThrowIfFailed(swapChain->GetBuffer(0, IID_PPV_ARGS(&swapChainBuffers[0])));
	DX::ThrowIfFailed(swapChain->GetBuffer(1, IID_PPV_ARGS(&swapChainBuffers[1])));

	frameIndex = swapChain->GetCurrentBackBufferIndex();

	// Set color space based on HDR Display feature state and negotiated format
	auto* hdr = globals::features::hdrDisplay.loaded ? &globals::features::hdrDisplay : nullptr;
	bool enableHDR = hdr && hdr->settings.enableHDR;
	// Only set HDR color space if not falling back to SDR format
	SetColorSpace(enableHDR && !fallbackUsed);

	fidelityFX.SetupFrameGeneration();
}

void DX12SwapChain::CreateSwapChainDirect(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC a_swapChainDesc)
{
	CreateD3D12Device(adapter);

	IDXGIFactory4* factoryRaw{};
	DX::ThrowIfFailed(adapter->GetParent(IID_PPV_ARGS(&factoryRaw)));

	// CreateSwapChainForHwnd must go through the SL-upgraded factory (a mandatory manual
	// hook), or Streamline never recognizes the swap chain as its own.
	auto& streamlineDX12 = globals::features::upscaling.streamlineDX12;
	if (streamlineDX12.slUpgradeInterface)
		streamlineDX12.slUpgradeInterface((void**)&factoryRaw);
	winrt::com_ptr<IDXGIFactory4> dxgiFactory;
	dxgiFactory.attach(factoryRaw);

	DXGI_FORMAT attemptedFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
	DXGI_FORMAT negotiatedFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
	bool fallbackUsed = false;

	D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_FORMAT_SUPPORT1_RENDER_TARGET, D3D12_FORMAT_SUPPORT2_NONE };
	if (SUCCEEDED(d3d12Device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport)))) {
		if ((formatSupport.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) == 0) {
			logger::warn("[DX12SwapChain] R10G10B10A2_UNORM not supported as render target, falling back to R8G8B8A8_UNORM");
			negotiatedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			fallbackUsed = true;
		}
	} else {
		logger::warn("[DX12SwapChain] CheckFeatureSupport failed for R10G10B10A2_UNORM, falling back to R8G8B8A8_UNORM");
		negotiatedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		fallbackUsed = true;
	}

	logger::info("[DX12SwapChain] Direct swap chain format negotiation: attempted={}, negotiated={}, fallback={}",
		static_cast<uint32_t>(attemptedFormat),
		static_cast<uint32_t>(negotiatedFormat),
		fallbackUsed ? "true" : "false");

	swapChainDesc = {};
	swapChainDesc.Width = a_swapChainDesc.BufferDesc.Width;
	swapChainDesc.Height = a_swapChainDesc.BufferDesc.Height;
	swapChainDesc.Format = negotiatedFormat;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	// The SL pacer holds one backbuffer for composition while flipping generated frames; a
	// two-buffer chain leaves it no slack. Sized from the hardware multiplier, not the live
	// setting, because this runs before user settings are loaded.
	backBufferCount = std::clamp<UINT>(streamlineDX12.dlssgMaxFramesToGenerate + 2, 3, kMaxBackBuffers);
	swapChainDesc.BufferCount = backBufferCount;
	swapChainDesc.SwapEffect = a_swapChainDesc.SwapEffect;
	// No FRAME_LATENCY_WAITABLE_OBJECT: waiting on it serializes presents to one in
	// flight, which makes the SL pacer drop every interpolated frame.
	swapChainDesc.Flags = a_swapChainDesc.Flags & ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

	winrt::com_ptr<IDXGISwapChain1> swapChain1;
	DX::ThrowIfFailed(dxgiFactory->CreateSwapChainForHwnd(
		commandQueue.get(),
		a_swapChainDesc.OutputWindow,
		&swapChainDesc,
		nullptr,
		nullptr,
		swapChain1.put()));

	DX::ThrowIfFailed(swapChain1->QueryInterface(IID_PPV_ARGS(&swapChain)));

	for (UINT i = 0; i < backBufferCount; i++) {
		DX::ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&swapChainBuffers[i])));
		const std::wstring bufferName = L"DX12SwapChain::DirectBackBuffer[" + std::to_wstring(i) + L"]";
		swapChainBuffers[i]->SetName(bufferName.c_str());
	}

	frameIndex = swapChain->GetCurrentBackBufferIndex();

	auto* hdr = globals::features::hdrDisplay.loaded ? &globals::features::hdrDisplay : nullptr;
	bool enableHDR = hdr && hdr->settings.enableHDR;
	SetColorSpace(enableHDR && !fallbackUsed);

	useDLSSG = true;
	logger::info("[DX12SwapChain] Created direct swap chain for DLSS-G ({}x{})", swapChainDesc.Width, swapChainDesc.Height);
}

void DX12SwapChain::CreateSwapChainArise(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC a_swapChainDesc,
	bool asyncTail, bool enableDLSSG)
{
	CreateD3D12Device(adapter);
	ariseAsyncTail = asyncTail;
	ariseRenderDocCompatibility = GetModuleHandleW(L"renderdoc.dll") != nullptr;

	IDXGIFactory4* factoryRaw{};
	DX::ThrowIfFailed(adapter->GetParent(IID_PPV_ARGS(&factoryRaw)));
	bool streamlineFactoryReady = false;
	if (enableDLSSG && globals::features::upscaling.streamlineDX12.slUpgradeInterface) {
		const sl::Result upgradeResult = globals::features::upscaling.streamlineDX12.slUpgradeInterface(
			reinterpret_cast<void**>(&factoryRaw));
		streamlineFactoryReady = upgradeResult == sl::Result::eOk;
		if (!streamlineFactoryReady)
			logger::error("[ARISE] Streamline failed to upgrade the DXGI factory: {}; DLSS-G disabled",
				magic_enum::enum_name(upgradeResult));
	}
	winrt::com_ptr<IDXGIFactory4> dxgiFactory;
	dxgiFactory.attach(factoryRaw);

	swapChainDesc = {};
	swapChainDesc.Width = a_swapChainDesc.BufferDesc.Width;
	swapChainDesc.Height = a_swapChainDesc.BufferDesc.Height;
	swapChainDesc.Format = a_swapChainDesc.BufferDesc.Format;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = 3;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.Flags = a_swapChainDesc.Flags & ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
	UpdateLogicalSwapChainDesc();

	winrt::com_ptr<IDXGISwapChain1> swapChain1;
	DX::ThrowIfFailed(dxgiFactory->CreateSwapChainForHwnd(
		commandQueue.get(), a_swapChainDesc.OutputWindow, &swapChainDesc, nullptr, nullptr, swapChain1.put()));
	DX::ThrowIfFailed(swapChain1->QueryInterface(IID_PPV_ARGS(&swapChain)));

	for (UINT index = 0; index < swapChainDesc.BufferCount; ++index) {
		DX::ThrowIfFailed(swapChain->GetBuffer(index, IID_PPV_ARGS(swapChainBuffers[index].put())));
		const std::wstring bufferName = L"ARISE::PhysicalBackBuffer[" + std::to_wstring(index) + L"]";
		swapChainBuffers[index]->SetName(bufferName.c_str());
	}
	frameIndex = swapChain->GetCurrentBackBufferIndex();

	SetColorSpace(false);
	useDLSSG = enableDLSSG && streamlineFactoryReady;
	if (enableDLSSG && !globals::features::upscaling.streamlineDX12.slUpgradeInterface) {
		logger::warn("[ARISE] DLSS-G requested but Streamline interface upgrade is unavailable; using normal physical Present");
	}
	if (useDLSSG)
		logger::info("[ARISE] Streamline owns the physical swap chain; DLSS-G Present path enabled");
	useArise = true;
	logger::info("[ARISE] Created provider-independent swap chain physical D={}x{} logical R={}x{} ratio=({:.4f},{:.4f}) buffers={} execution={}",
		swapChainDesc.Width, swapChainDesc.Height, logicalSwapChainDesc.Width, logicalSwapChainDesc.Height,
		static_cast<float>(logicalSwapChainDesc.Width) / swapChainDesc.Width,
		static_cast<float>(logicalSwapChainDesc.Height) / swapChainDesc.Height, swapChainDesc.BufferCount,
		ariseAsyncTail ? "async-tail" : "single-thread");
}

void DX12SwapChain::UpdateLogicalSwapChainDesc()
{
	logicalSwapChainDesc = swapChainDesc;
	const auto& foveated = globals::features::upscaling.foveatedRender;
	const auto unified = foveated.ActiveUnifiedPipelineSettings();
	const bool srss = unified.act1Srss;
	const uint quality = std::min(srss ? unified.act1SrssQuality :
		foveated.bootSnapshot.Boot(&FoveatedRender::Settings::flatPipelineAct1Quality), 4u);
	const float guidanceScale = srss ? unified.guidanceScale : 1.0f;
	const float ratio = Upscaling::GetQualityModeRatio(quality);
	const UINT guidanceWidth = AlignEvenExtent(
		static_cast<UINT>(std::lround(swapChainDesc.Width * guidanceScale)), D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);
	const UINT guidanceHeight = AlignEvenExtent(
		static_cast<UINT>(std::lround(swapChainDesc.Height * guidanceScale)), D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);
	UINT logicalWidth = AlignEvenExtent(static_cast<UINT>(std::lround(guidanceWidth / ratio)), guidanceWidth);
	UINT logicalHeight = AlignEvenExtent(static_cast<UINT>(std::lround(guidanceHeight / ratio)), guidanceHeight);
	if (globals::features::upscaling.streamline.slDLSSGetOptimalSettings) {
		globals::features::upscaling.streamline.ClampToDLSSRenderRange(
			quality, guidanceWidth, guidanceHeight, logicalWidth, logicalHeight);
		logicalWidth = AlignEvenExtent(logicalWidth, guidanceWidth);
		logicalHeight = AlignEvenExtent(logicalHeight, guidanceHeight);
	}
	logicalSwapChainDesc.Width = logicalWidth;
	logicalSwapChainDesc.Height = logicalHeight;
	logger::info("[PerfMode] Logical contract R={}x{} -> G={}x{} -> D={}x{} mode={} G/D={:.3f}x",
		logicalWidth, logicalHeight, guidanceWidth, guidanceHeight,
		swapChainDesc.Width, swapChainDesc.Height, quality, guidanceScale);
}

void DX12SwapChain::CreateInterop()
{
	interopFence.Create(d3d12Device.get(), d3d11Device.get(), "DX12SwapChain::InteropFence");

	swapChainProxy = new DXGISwapChainProxy(swapChain);

	RecreateWrappedResources(swapChainDesc);
	if (useArise) {
		CreateArisePresentResources();
		NeuralRendering::ARISE::Pipeline::Instance().SetModel3Enabled(ariseAsyncTail && !ariseRenderDocCompatibility);
	}
}

void DX12SwapChain::CreateArisePresentResources()
{
	auto cleanup = [this] {
		ariseReconstructionGraph.Shutdown();
		ariseGraphAvailable.store(false, std::memory_order_release);
		for (auto& slot : ariseSlots) {
			slot.sealedScene.reset();
			slot.sealedUI.reset();
			slot.displayUI.reset();
			slot.sealedDepth.reset();
			slot.sealedMotionVectors.reset();
			slot.allocator = nullptr;
			slot.commandList = nullptr;
			slot.state = AriseSlotState::kWritable;
			slot.engineReady = 0;
			slot.tailDone = 0;
			slot.frameId = 0;
		}
		ariseCompositeRootSignature = nullptr;
		ariseCompositePipelineState = nullptr;
		ariseUiScalePipelineState = nullptr;
		ariseCompositeSrvHeap = nullptr;
		ariseCompositeRtvHeap = nullptr;
		ariseEngineFence11 = nullptr;
		ariseEngineFence12 = nullptr;
		ariseTailFence12 = nullptr;
	};
	try {
		ariseTailNrAvailable.store(false, std::memory_order_release);
		HANDLE sharedFenceHandle = nullptr;
		DX::ThrowIfFailed(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(ariseEngineFence12.put())));
		DX::ThrowIfFailed(d3d12Device->CreateSharedHandle(
			ariseEngineFence12.get(), nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle));
		const HRESULT openFenceResult = d3d11Device->OpenSharedFence(sharedFenceHandle, IID_PPV_ARGS(ariseEngineFence11.put()));
		CloseHandle(sharedFenceHandle);
		DX::ThrowIfFailed(openFenceResult);
		DX::ThrowIfFailed(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(ariseTailFence12.put())));
		ariseEngineValue = 0;
		ariseTailValue = 0;

		D3D11_TEXTURE2D_DESC slotDesc{};
		slotDesc.Width = logicalSwapChainDesc.Width;
		slotDesc.Height = logicalSwapChainDesc.Height;
		slotDesc.MipLevels = 1;
		slotDesc.ArraySize = 1;
		slotDesc.Format = swapChainDesc.Format;
		slotDesc.SampleDesc.Count = 1;
		slotDesc.Usage = D3D11_USAGE_DEFAULT;
		D3D11_TEXTURE2D_DESC uiSlotDesc = slotDesc;
		uiSlotDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		D3D11_TEXTURE2D_DESC displayUiDesc = uiSlotDesc;
		displayUiDesc.Width = swapChainDesc.Width;
		displayUiDesc.Height = swapChainDesc.Height;
		displayUiDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		for (std::size_t index = 0; index < kAriseSlotCount; ++index) {
			auto& slot = ariseSlots[index];
			slot.sealedScene = std::make_unique<WrappedResource>(slotDesc, d3d11Device.get(), d3d12Device.get(),
				"ARISE::PresentSlotScene[" + std::to_string(index) + "]");
			slot.sealedUI = std::make_unique<WrappedResource>(uiSlotDesc, d3d11Device.get(), d3d12Device.get(),
				"ARISE::PresentSlotUI[" + std::to_string(index) + "]");
			slot.displayUI = std::make_unique<WrappedResource>(displayUiDesc, d3d11Device.get(), d3d12Device.get(),
				"ARISE::PresentSlotDisplayUI[" + std::to_string(index) + "]");
			DX::ThrowIfFailed(d3d12Device->CreateCommandAllocator(
				D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.allocator.put())));
			DX::ThrowIfFailed(d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				slot.allocator.get(), nullptr, IID_PPV_ARGS(slot.commandList.put())));
			DX::ThrowIfFailed(slot.commandList->Close());
			slot.state = AriseSlotState::kWritable;
			slot.engineReady = 0;
			slot.tailDone = 0;
		}
		DX::ThrowIfFailed(CreateAriseCompositeResources());
		const bool graphReady = ariseReconstructionGraph.Initialize(
			d3d12Device.get(), swapChainDesc.Format, kAriseSlotCount,
			ariseRenderDocCompatibility ? nullptr : &globals::features::upscaling.streamlineDX12);
		ariseGraphAvailable.store(graphReady, std::memory_order_release);
		const bool runtimeReady = !ariseRenderDocCompatibility && graphReady && NeuralRendering::Renderer::Instance().InitializeModel3(
			d3d12Device.get(), [this] { return DrainModel3Tail(); });
		ariseTailNrAvailable.store(runtimeReady, std::memory_order_release);
		if (ariseRenderDocCompatibility)
			logger::info("[ARISE] RenderDoc compatibility tail ready: DLSS/NR replaced by visible resample/copy passes");
		else if (!graphReady)
			logger::warn("[ARISE] Reconstruction graph initialization failed; scene passthrough remains active");
		else if (!runtimeReady)
			logger::warn("[ARISE] Feature 18 runtime unavailable; scene passthrough remains active");
		ariseProducerCursor = 0;
		logger::info("[ARISE] Model 3 present tail ready with {} shared slots, reconstruction graph={}",
			kAriseSlotCount, runtimeReady ? "ready" : "fallback");
	} catch (...) {
		cleanup();
		throw;
	}
}

HRESULT DX12SwapChain::CreateAriseCompositeResources()
{
	auto fail = [this](HRESULT result) {
		ariseCompositeRootSignature = nullptr;
		ariseCompositePipelineState = nullptr;
		ariseCompositeSrvHeap = nullptr;
		ariseCompositeRtvHeap = nullptr;
		return FAILED(result) ? result : E_FAIL;
	};
	D3D12_DESCRIPTOR_RANGE srvRange{};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = 2;
	srvRange.BaseShaderRegister = 0;
	srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	D3D12_ROOT_PARAMETER rootParameter{};
	rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParameter.DescriptorTable.NumDescriptorRanges = 1;
	rootParameter.DescriptorTable.pDescriptorRanges = &srvRange;
	rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxAnisotropy = 1;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	sampler.ShaderRegister = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	D3D12_ROOT_SIGNATURE_DESC rootDesc{};
	rootDesc.NumParameters = 1;
	rootDesc.pParameters = &rootParameter;
	rootDesc.NumStaticSamplers = 1;
	rootDesc.pStaticSamplers = &sampler;
	rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	winrt::com_ptr<ID3DBlob> rootBlob;
	winrt::com_ptr<ID3DBlob> errorBlob;
	HRESULT result = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		rootBlob.put(), errorBlob.put());
	Util::LogShaderCompileWarnings(errorBlob.get(), "Model3UIComposite root signature");
	if (FAILED(result))
		return fail(result);
	result = d3d12Device->CreateRootSignature(0, rootBlob->GetBufferPointer(),
		rootBlob->GetBufferSize(), IID_PPV_ARGS(ariseCompositeRootSignature.put()));
	if (FAILED(result))
		return fail(result);
	ariseCompositeRootSignature->SetName(L"ARISE::Model3UICompositeRootSignature");

	winrt::com_ptr<ID3DBlob> vertexShader;
	winrt::com_ptr<ID3DBlob> pixelShader;
	winrt::com_ptr<ID3DBlob> uiPixelShader;
	errorBlob = nullptr;
	result = D3DCompile(kModel3UICompositeShader, sizeof(kModel3UICompositeShader), "Model3UIComposite",
		nullptr, nullptr, "VSMain", "vs_5_1",
		D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vertexShader.put(), errorBlob.put());
	Util::LogShaderCompileWarnings(errorBlob.get(), "Model3UIComposite VS");
	if (FAILED(result))
		return fail(result);
	errorBlob = nullptr;
	result = D3DCompile(kModel3UICompositeShader, sizeof(kModel3UICompositeShader), "Model3UIComposite",
		nullptr, nullptr, "PSUIOnly", "ps_5_1",
		D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, uiPixelShader.put(), errorBlob.put());
	Util::LogShaderCompileWarnings(errorBlob.get(), "Model3UIUpscale PS");
	if (FAILED(result))
		return fail(result);
	errorBlob = nullptr;
	result = D3DCompile(kModel3UICompositeShader, sizeof(kModel3UICompositeShader), "Model3UIComposite",
		nullptr, nullptr, "PSMain", "ps_5_1",
		D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, pixelShader.put(), errorBlob.put());
	Util::LogShaderCompileWarnings(errorBlob.get(), "Model3UIComposite PS");
	if (FAILED(result))
		return fail(result);

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
	pipelineDesc.pRootSignature = ariseCompositeRootSignature.get();
	pipelineDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
	pipelineDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
	pipelineDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	pipelineDesc.SampleMask = UINT_MAX;
	pipelineDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	pipelineDesc.DepthStencilState.DepthEnable = FALSE;
	pipelineDesc.DepthStencilState.StencilEnable = FALSE;
	pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pipelineDesc.NumRenderTargets = 1;
	pipelineDesc.RTVFormats[0] = swapChainDesc.Format;
	pipelineDesc.SampleDesc.Count = 1;
	result = d3d12Device->CreateGraphicsPipelineState(&pipelineDesc,
		IID_PPV_ARGS(ariseCompositePipelineState.put()));
	if (FAILED(result))
		return fail(result);
	ariseCompositePipelineState->SetName(L"ARISE::Model3UICompositePSO");
	pipelineDesc.PS = { uiPixelShader->GetBufferPointer(), uiPixelShader->GetBufferSize() };
	pipelineDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	result = d3d12Device->CreateGraphicsPipelineState(&pipelineDesc,
		IID_PPV_ARGS(ariseUiScalePipelineState.put()));
	if (FAILED(result))
		return fail(result);
	ariseUiScalePipelineState->SetName(L"ARISE::Model3UIUpscalePSO");

	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.NumDescriptors = kAriseSlotCount * 4;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	result = d3d12Device->CreateDescriptorHeap(&srvHeapDesc,
		IID_PPV_ARGS(ariseCompositeSrvHeap.put()));
	if (FAILED(result))
		return fail(result);
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.NumDescriptors = swapChainDesc.BufferCount + kAriseSlotCount;
	result = d3d12Device->CreateDescriptorHeap(&rtvHeapDesc,
		IID_PPV_ARGS(ariseCompositeRtvHeap.put()));
	if (FAILED(result))
		return fail(result);

	const UINT srvStride = d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	auto srvHandle = ariseCompositeSrvHeap->GetCPUDescriptorHandleForHeapStart();
	for (const auto& slot : ariseSlots) {
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		d3d12Device->CreateShaderResourceView(slot.sealedUI->resource.get(), &srvDesc, srvHandle);
		srvHandle.ptr += static_cast<SIZE_T>(srvStride) * 2;
		d3d12Device->CreateShaderResourceView(slot.displayUI->resource.get(), &srvDesc, srvHandle);
		srvHandle.ptr += static_cast<SIZE_T>(srvStride) * 2;
	}
	const UINT rtvStride = d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	auto rtvHandle = ariseCompositeRtvHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT index = 0; index < swapChainDesc.BufferCount; ++index) {
		d3d12Device->CreateRenderTargetView(swapChainBuffers[index].get(), nullptr, rtvHandle);
		rtvHandle.ptr += rtvStride;
	}
	for (const auto& slot : ariseSlots) {
		d3d12Device->CreateRenderTargetView(slot.displayUI->resource.get(), nullptr, rtvHandle);
		rtvHandle.ptr += rtvStride;
	}
	return S_OK;
}

bool DX12SwapChain::DrainModel3Tail()
{
	if (!NeuralRendering::ARISE::Pipeline::Instance().WaitForWorkerTasksFor(kAriseWaitTimeout, [this] {
			return ariseFatalFailure.load(std::memory_order_acquire);
		})) {
		EnterAriseSynchronousFallback(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "Model 3 worker drain timeout");
		return false;
	}
	return WaitForAriseIdle();
}

bool DX12SwapChain::PrepareAriseGraphPlanChange()
{
	{
		std::scoped_lock lock(ariseMutex);
		arisePendingCaptureValid = false;
		arisePendingPresentSequence = 0;
	}
	if (!DrainModel3Tail())
		return false;
	ariseReconstructionGraph.Reset();
	if (ariseRenderDocCompatibility)
		return ariseGraphAvailable.load(std::memory_order_acquire);
	NeuralRendering::Renderer::Instance().ResetModel3();
	const bool runtimeReady = NeuralRendering::Renderer::Instance().InitializeModel3(
		d3d12Device.get(), [this] { return DrainModel3Tail(); });
	ariseTailNrAvailable.store(runtimeReady, std::memory_order_release);
	if (!runtimeReady)
		logger::error("[ARISE] Model 3 runtime rebuild failed after graph plan change; passthrough remains active");
	return runtimeReady;
}

bool DX12SwapChain::ReleaseArisePresentResources()
{
	ariseTailNrAvailable.store(false, std::memory_order_release);
	ariseGraphAvailable.store(false, std::memory_order_release);
	if (!DrainModel3Tail())
		return false;
	NeuralRendering::Renderer::Instance().ResetModel3();
	ariseReconstructionGraph.Shutdown();
	std::scoped_lock lock(ariseMutex);
	for (auto& slot : ariseSlots) {
		slot.sealedScene.reset();
		slot.sealedUI.reset();
			slot.displayUI.reset();
		slot.sealedDepth.reset();
		slot.sealedMotionVectors.reset();
		slot.capture = {};
		slot.captureValid = false;
		slot.allocator = nullptr;
		slot.commandList = nullptr;
		slot.state = AriseSlotState::kWritable;
		slot.engineReady = 0;
		slot.tailDone = 0;
		slot.frameId = 0;
	}
	ariseCompositeRootSignature = nullptr;
	ariseCompositePipelineState = nullptr;
	ariseUiScalePipelineState = nullptr;
	ariseCompositeSrvHeap = nullptr;
	ariseCompositeRtvHeap = nullptr;
	ariseEngineFence11 = nullptr;
	ariseEngineFence12 = nullptr;
	ariseTailFence12 = nullptr;
	arisePendingDepth.reset();
	arisePendingMotionVectors.reset();
	arisePendingCapture = {};
	arisePendingCaptureValid = false;
	ariseProducerCursor = 0;
	return true;
}

void DX12SwapChain::RecreateWrappedResources(const DXGI_SWAP_CHAIN_DESC1& desc)
{
	D3D11_TEXTURE2D_DESC texDesc11{};
	texDesc11.Width = useArise ? logicalSwapChainDesc.Width : desc.Width;
	texDesc11.Height = useArise ? logicalSwapChainDesc.Height : desc.Height;
	texDesc11.MipLevels = 1;
	texDesc11.ArraySize = 1;
	texDesc11.Format = desc.Format;
	texDesc11.SampleDesc.Count = 1;
	texDesc11.SampleDesc.Quality = 0;
	texDesc11.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;

	// Build both replacements before releasing the active resources so a failed
	// allocation cannot leave the proxy with only half of its interop textures.
	auto newSwapChainBuffer = std::make_unique<WrappedResource>(texDesc11, d3d11Device.get(), d3d12Device.get(), "DX12SwapChain::SwapChainBuffer");

	// Model 3 keeps the entire engine chain, including UI, in logical R. The
	// private D3D12 composite is the only pass that expands scene and UI to D.
	texDesc11.Width = useArise ? logicalSwapChainDesc.Width : desc.Width;
	texDesc11.Height = useArise ? logicalSwapChainDesc.Height : desc.Height;
	texDesc11.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	auto newUiBuffer = std::make_unique<WrappedResource>(texDesc11, d3d11Device.get(), d3d12Device.get(), "DX12SwapChain::UIBuffer");

	delete swapChainBufferWrapped;
	delete uiBufferWrapped;
	swapChainBufferWrapped = newSwapChainBuffer.release();
	uiBufferWrapped = newUiBuffer.release();

	globals::features::upscaling.frameGenerationPrepared = false;

	const float clearColor[4]{};
	d3d11Context->ClearRenderTargetView(swapChainBufferWrapped->rtv, clearColor);
	d3d11Context->ClearRenderTargetView(uiBufferWrapped->rtv, clearColor);
}

DXGISwapChainProxy* DX12SwapChain::GetSwapChainProxy()
{
	return swapChainProxy;
}

void DX12SwapChain::SetD3D11Device(ID3D11Device* a_d3d11Device)
{
	DX::ThrowIfFailed(a_d3d11Device->QueryInterface(IID_PPV_ARGS(&d3d11Device)));
}

void DX12SwapChain::SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context)
{
	DX::ThrowIfFailed(a_d3d11Context->QueryInterface(IID_PPV_ARGS(&d3d11Context)));
}

HRESULT DX12SwapChain::GetBuffer(UINT buffer, REFIID riid, void** ppSurface)
{
	if (!ppSurface)
		return E_POINTER;

	*ppSurface = nullptr;
	if (buffer != 0 || !swapChainBufferWrapped || !swapChainBufferWrapped->resource11)
		return DXGI_ERROR_INVALID_CALL;

	// IDXGISwapChain::GetBuffer returns an owned COM reference. Returning the raw
	// pointer here let the caller's Release destroy the shared texture while the
	// D3D12 side still retained and submitted its corresponding resource.
	return swapChainBufferWrapped->resource11->QueryInterface(riid, ppSurface);
}

HRESULT DX12SwapChain::ResizeBuffers(UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
	if (!swapChain)
		return DXGI_ERROR_INVALID_CALL;

	// DXGI defines zero as "preserve the current buffer count". FidelityFX's
	// frame-generation swap-chain stores the supplied value verbatim and uses it
	// as its replacement-buffer count, so forwarding zero leaves it with no valid
	// source resource at the next Present.
	const UINT effectiveBufferCount = bufferCount ? bufferCount : swapChainDesc.BufferCount;
	if (useArise) {
		if (effectiveBufferCount != kAriseSlotCount)
			return DXGI_ERROR_UNSUPPORTED;
		const double widthScale = logicalSwapChainDesc.Width ?
			static_cast<double>(swapChainDesc.Width) / logicalSwapChainDesc.Width : 1.0;
		const double heightScale = logicalSwapChainDesc.Height ?
			static_cast<double>(swapChainDesc.Height) / logicalSwapChainDesc.Height : 1.0;
		const UINT physicalWidth = width ? std::max(1u, static_cast<UINT>(std::lround(width * widthScale))) : 0u;
		const UINT physicalHeight = height ? std::max(1u, static_cast<UINT>(std::lround(height * heightScale))) : 0u;
		if (!NeuralRendering::ARISE::Pipeline::Instance().WaitForWorkerTasksFor(kAriseWaitTimeout, [this] {
				return ariseFatalFailure.load(std::memory_order_acquire);
			})) {
			const HRESULT result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
			EnterAriseSynchronousFallback(result, "ResizeBuffers worker drain timeout");
			return result;
		}
		if (!WaitForAriseIdle())
			return static_cast<HRESULT>(ariseLastPresentResult.load(std::memory_order_acquire));
		if (ariseFatalFailure.load(std::memory_order_acquire))
			return static_cast<HRESULT>(ariseLastPresentResult.load(std::memory_order_acquire));
		if (!ReleaseArisePresentResources())
			return static_cast<HRESULT>(ariseLastPresentResult.load(std::memory_order_acquire));
		for (auto& buffer : swapChainBuffers)
			buffer = nullptr;
		const HRESULT resizeResult = swapChain->ResizeBuffers(
			effectiveBufferCount, physicalWidth, physicalHeight, format, flags);
		if (FAILED(resizeResult)) {
			try {
				DX::ThrowIfFailed(swapChain->GetDesc1(&swapChainDesc));
				UpdateLogicalSwapChainDesc();
				for (UINT index = 0; index < kAriseSlotCount; ++index)
					DX::ThrowIfFailed(swapChain->GetBuffer(index, IID_PPV_ARGS(swapChainBuffers[index].put())));
				frameIndex = swapChain->GetCurrentBackBufferIndex();
				RecreateWrappedResources(swapChainDesc);
				CreateArisePresentResources();
			} catch (...) {
				LatchAriseFailure(resizeResult, "ResizeBuffers recovery");
				return resizeResult;
			}
			if (IsAriseDeviceFatal(resizeResult))
				LatchAriseFailure(resizeResult, "ResizeBuffers");
			else
				logger::warn("[ARISE] ResizeBuffers failed with HRESULT 0x{:08X}; restored previous D/R resources",
					static_cast<std::uint32_t>(resizeResult));
			return resizeResult;
		}
		try {
			DX::ThrowIfFailed(swapChain->GetDesc1(&swapChainDesc));
			UpdateLogicalSwapChainDesc();
			for (UINT index = 0; index < kAriseSlotCount; ++index)
				DX::ThrowIfFailed(swapChain->GetBuffer(index, IID_PPV_ARGS(swapChainBuffers[index].put())));
			frameIndex = swapChain->GetCurrentBackBufferIndex();
			RecreateWrappedResources(swapChainDesc);
			CreateArisePresentResources();
		} catch (const winrt::hresult_error& error) {
			const HRESULT result = error.code();
			LatchAriseFailure(result, "ResizeBuffers resource rebuild");
			return result;
		} catch (...) {
			LatchAriseFailure(E_FAIL, "ResizeBuffers resource rebuild");
			return E_FAIL;
		}
		ApplyLogicalRenderExtents();
		return S_OK;
	}
	if (!bufferCount)
		logger::warn("[FidelityFX] Normalized ResizeBuffers count from 0 to {} to preserve replacement buffers", effectiveBufferCount);
	if (effectiveBufferCount != backBufferCount) {
		logger::error("[DX12SwapChain] Rejected unsupported resize buffer count change {} -> {}", backBufferCount, effectiveBufferCount);
		return DXGI_ERROR_UNSUPPORTED;
	}

	// These references are to FidelityFX replacement buffers. They must not keep
	// the old generation alive across the provider's resize, and must be refreshed
	// before CS records another copy.
	for (UINT i = 0; i < backBufferCount; i++) {
		swapChainBuffers[i] = nullptr;
	}
	const HRESULT result = swapChain->ResizeBuffers(effectiveBufferCount, width, height, format, flags);
	if (FAILED(result)) {
		// The resize didn't take effect, so the pre-resize buffers should still be
		// valid (unless the device itself is gone, in which case this also fails
		// and Present's null guard below is the last line of defense).
		for (UINT i = 0; i < backBufferCount; i++) {
			swapChain->GetBuffer(i, IID_PPV_ARGS(swapChainBuffers[i].put()));
		}
		return result;
	}

	DXGI_SWAP_CHAIN_DESC1 resizedDesc{};
	const HRESULT descResult = swapChain->GetDesc1(&resizedDesc);
	if (FAILED(descResult)) {
		// The resize itself succeeded; only the desc query failed. Re-fetch the
		// (already resized) buffers so Present isn't left with nulls.
		for (UINT i = 0; i < backBufferCount; i++) {
			swapChain->GetBuffer(i, IID_PPV_ARGS(swapChainBuffers[i].put()));
		}
		return descResult;
	}

	const bool wrappedResourcesChanged = resizedDesc.Width != swapChainDesc.Width ||
	                                     resizedDesc.Height != swapChainDesc.Height ||
	                                     resizedDesc.Format != swapChainDesc.Format;
	if (wrappedResourcesChanged)
		RecreateWrappedResources(resizedDesc);
	swapChainDesc = resizedDesc;

	for (UINT i = 0; i < backBufferCount; i++) {
		DX::ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(swapChainBuffers[i].put())));
	}
	frameIndex = swapChain->GetCurrentBackBufferIndex();
	return S_OK;
}

HRESULT DX12SwapChain::Present(UINT SyncInterval, UINT Flags)
{
	if (useArise)
		return PresentArise(SyncInterval, Flags);

	auto& upscaling = globals::features::upscaling;

	// Scale UI brightness BEFORE fence sync so the D3D11 UIBrightnessCS dispatch
	// is covered by the D3D11→D3D12 fence. Without this, FidelityFX may read
	// uiBufferWrapped on D3D12 before the PQ encoding completes on D3D11.
	// Only runs when HDR Display feature is loaded (UIBrightnessCS may not exist otherwise)
	auto* hdr = globals::features::hdrDisplay.loaded ? &globals::features::hdrDisplay : nullptr;
	if (hdr)
		hdr->ScaleUIBrightnessForFG();

	bool isHDR = hdr && hdr->settings.enableHDR;

	// Wait for D3D11 to finish (includes ApplyHDR scene encoding AND UIBrightnessCS)
	const uint64_t d3d11SignalValue = interopFence.Next();
	DX::ThrowIfFailed(d3d11Context->Signal(interopFence.fence11.get(), d3d11SignalValue));
	DX::ThrowIfFailed(commandQueue->Wait(interopFence.fence12.get(), d3d11SignalValue));

	// New frame, reset
	if (frameFenceValues[frameIndex])
		DX::ThrowIfFailed(interopFence.fence12->SetEventOnCompletion(frameFenceValues[frameIndex], nullptr));
	DX::ThrowIfFailed(commandAllocators[frameIndex]->Reset());
	DX::ThrowIfFailed(commandLists[frameIndex]->Reset(commandAllocators[frameIndex].get(), nullptr));

	// Copy shared texture to swap chain buffer
	{
		auto fakeSwapChain = swapChainBufferWrapped->resource.get();
		auto realSwapChain = swapChainBuffers[frameIndex].get();
		// Null only after a resize failure severe enough that even the pre-resize
		// buffers couldn't be re-fetched (e.g. device removed) -- skip this frame's
		// copy/present rather than pass a null resource to D3D12.
		if (!realSwapChain)
			return DXGI_ERROR_DEVICE_REMOVED;
		{
			std::vector<D3D12_RESOURCE_BARRIER> barriers;
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(fakeSwapChain, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE));
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(realSwapChain, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));
			commandLists[frameIndex]->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		}

		commandLists[frameIndex]->CopyResource(realSwapChain, fakeSwapChain);

		{
			std::vector<D3D12_RESOURCE_BARRIER> barriers;
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(fakeSwapChain, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON));
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(realSwapChain, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT));
			commandLists[frameIndex]->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		}
	}

	if (useDLSSG) {
		auto& streamlineDX12 = upscaling.streamlineDX12;
		streamlineDX12.EnsureFrameToken();
		// The full per-frame PCL marker sequence is structural for interpolation
		// (eSimulationStart is emitted at the Reflex sleep site).
		streamlineDX12.EmitPCLMarker(sl::PCLMarker::eSimulationEnd);
		streamlineDX12.EmitPCLMarker(sl::PCLMarker::eRenderSubmitStart);
		// Skip tagging/interpolation without valid per-frame constants -- otherwise
		// DLSS-G interpolates against stale or default camera data.
		if (streamlineDX12.CheckFrameConstants(streamlineDX12.viewport)) {
			streamlineDX12.TagDX12Resources(commandLists[frameIndex].get(),
				depthBufferShared12 ? depthBufferShared12->resource.get() : nullptr,
				motionVectorBufferShared12 ? motionVectorBufferShared12->resource.get() : nullptr,
				swapChainBufferWrapped ? swapChainBufferWrapped->resource.get() : nullptr,
				uiBufferWrapped ? uiBufferWrapped->resource.get() : nullptr,
				swapChainDesc.Width, swapChainDesc.Height);
			streamlineDX12.ConfigureDLSSG(upscaling.ShouldUseFrameGenerationThisFrame());
		} else {
			streamlineDX12.ConfigureDLSSG(false);
		}
	} else if (!useArise) {
		upscaling.fidelityFX.Present(upscaling.ShouldUseFrameGenerationThisFrame(), isHDR);
	}

	DX::ThrowIfFailed(commandLists[frameIndex]->Close());

	ID3D12CommandList* commandListsToExecute[] = { commandLists[frameIndex].get() };
	commandQueue->ExecuteCommandLists(1, commandListsToExecute);

	if (useDLSSG) {
		upscaling.streamlineDX12.EmitPCLMarker(sl::PCLMarker::eRenderSubmitEnd);
		upscaling.streamlineDX12.EmitPCLMarker(sl::PCLMarker::ePresentStart);
	}

	// Present the frame
	DX::ThrowIfFailed(swapChain->Present(SyncInterval, Flags));

	if (useDLSSG)
		upscaling.streamlineDX12.EmitPCLMarker(sl::PCLMarker::ePresentEnd);

	// Wait for D3D12 to finish
	const uint64_t d3d12SignalValue = interopFence.Next();
	DX::ThrowIfFailed(commandQueue->Signal(interopFence.fence12.get(), d3d12SignalValue));
	frameFenceValues[frameIndex] = d3d12SignalValue;
	DX::ThrowIfFailed(d3d11Context->Wait(interopFence.fence11.get(), d3d12SignalValue));

	// Update the frame index
	frameIndex = swapChain->GetCurrentBackBufferIndex();

	float clearColor[4]{ 0, 0, 0, 0 };
	d3d11Context->ClearRenderTargetView(uiBufferWrapped->rtv, clearColor);

	// If VSync is disabled, use frame limiter to prevent tearing and optimise pacing
	if (SyncInterval == 0)
		upscaling.FrameLimiter();

	// Main_PostProcessing::thunk doesn't run on loading-screen frames, so its own
	// per-frame reset can't clear a stale true left over from the last gameplay
	// frame; reset here too so frame-gen doesn't interpolate through a transition.
	upscaling.frameGenerationPrepared = false;

	return S_OK;
}

HRESULT DX12SwapChain::PresentArise(UINT syncInterval, UINT flags)
{
	if (ariseRenderDocCompatibility)
		globals::features::renderDoc.AdvanceLogicalFrameCapture(d3d11Device.get());
	if ((flags & DXGI_PRESENT_TEST) != 0) {
		std::unique_lock presentLock(arisePhysicalPresentMutex, std::defer_lock);
		if (!presentLock.try_lock_for(kAriseWaitTimeout)) {
			const HRESULT result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
			EnterAriseSynchronousFallback(result, "physical Present test lock timeout");
			return result;
		}
		const HRESULT result = swapChain->Present(syncInterval, flags);
		if (FAILED(result)) {
			if (IsAriseDeviceFatal(result))
				LatchAriseFailure(result, "physical Present test");
			else
				EnterAriseSynchronousFallback(result, "physical Present test");
		}
		return result;
	}
	if (ariseFatalFailure.load(std::memory_order_acquire))
		return static_cast<HRESULT>(ariseLastPresentResult.load(std::memory_order_acquire));

	auto* hdr = globals::features::hdrDisplay.loaded ? &globals::features::hdrDisplay : nullptr;
	if (hdr)
		hdr->ScaleUIBrightnessForFG();

	std::size_t slotIndex = 0;
	for (;;) {
		UINT64 waitValue = 0;
		{
			std::scoped_lock lock(ariseMutex);
			ReapAriseSlotsLocked();
			if (ariseFatalFailure.load(std::memory_order_acquire))
				return static_cast<HRESULT>(ariseLastPresentResult.load(std::memory_order_acquire));
			auto& candidate = ariseSlots[ariseProducerCursor];
			if (candidate.state == AriseSlotState::kWritable) {
				slotIndex = ariseProducerCursor;
				candidate.state = AriseSlotState::kQueued;
				candidate.tailDone = 0;
				break;
			}
			for (const auto& slot : ariseSlots)
				if (slot.state == AriseSlotState::kSubmitted && slot.tailDone &&
					(!waitValue || slot.tailDone < waitValue))
					waitValue = slot.tailDone;
		}

		++ariseBackpressure;
		if (waitValue) {
			if (!WaitForAriseTail(waitValue))
				return static_cast<HRESULT>(ariseLastPresentResult.load(std::memory_order_acquire));
		} else {
			std::unique_lock lock(ariseMutex);
			const bool ready = ariseCondition.wait_for(lock, kAriseWaitTimeout, [this] {
				ReapAriseSlotsLocked();
				if (ariseFatalFailure.load(std::memory_order_acquire) ||
					ariseSlots[ariseProducerCursor].state == AriseSlotState::kWritable)
					return true;
				for (const auto& slot : ariseSlots)
					if (slot.state == AriseSlotState::kSubmitted && slot.tailDone)
						return true;
				return false;
			});
			lock.unlock();
			if (!ready) {
				const HRESULT result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
				EnterAriseSynchronousFallback(result, "present slot backpressure timeout");
				return result;
			}
		}
	}

	auto& slot = ariseSlots[slotIndex];
	const std::uint32_t presentFrameId = globals::state ? globals::state->frameCount : 0;
	auto& streamline = globals::features::upscaling.streamlineDX12;
	sl::FrameToken* logicalFrameToken = nullptr;
	if (streamline.AcquireFrameToken(presentFrameId, logicalFrameToken) && logicalFrameToken)
		streamline.EmitPCLMarker(sl::PCLMarker::eSimulationEnd, *logicalFrameToken);
	globals::features::upscaling.foveatedRender.BlitTitleBackgroundToSwapChain();
	if (globals::features::upscaling.foveatedRender.menuCopyDiagnostics.load(std::memory_order_acquire) &&
		globals::state && globals::state->IsMainOrLoadingMenuOpen())
		globals::features::upscaling.foveatedRender.LogMenuPipeline("present");
	if (globals::features::upscaling.foveatedRender.ariseDimensionDiagnostics.load(std::memory_order_acquire) &&
		(ariseLogicalPresents.load(std::memory_order_relaxed) % 120 == 0)) {
		D3D11_TEXTURE2D_DESC sceneDesc{};
		D3D11_TEXTURE2D_DESC uiDesc{};
		swapChainBufferWrapped->resource11->GetDesc(&sceneDesc);
		uiBufferWrapped->resource11->GetDesc(&uiDesc);
		UINT viewportCount = 1;
		D3D11_VIEWPORT viewport{};
		d3d11Context->RSGetViewports(&viewportCount, &viewport);
		UINT scissorCount = 1;
		D3D11_RECT scissor{};
		d3d11Context->RSGetScissorRects(&scissorCount, &scissor);
		auto* graphicsState = globals::game::graphicsState;
		const auto& runtimeData = graphicsState->GetRuntimeData();
		logger::info("[ARISE DIM] Present logicalScene={}x{} fmt={} ui={}x{} fmt={} physicalD={}x{} graphics={}x{} DRS=({:.4f},{:.4f}) lock={} VP=({:.1f},{:.1f} {:.1f}x{:.1f}) scissor=({},{} {}x{}) index={}",
			sceneDesc.Width, sceneDesc.Height, static_cast<uint>(sceneDesc.Format),
			uiDesc.Width, uiDesc.Height, static_cast<uint>(uiDesc.Format),
			swapChainDesc.Width, swapChainDesc.Height, graphicsState->screenWidth, graphicsState->screenHeight,
			runtimeData.dynamicResolutionWidthRatio, runtimeData.dynamicResolutionHeightRatio,
			runtimeData.dynamicResolutionLock, viewport.TopLeftX, viewport.TopLeftY,
			viewport.Width, viewport.Height, scissor.left, scissor.top,
			scissor.right - scissor.left, scissor.bottom - scissor.top, frameIndex);
	}
	d3d11Context->CopyResource(slot.sealedScene->resource11, swapChainBufferWrapped->resource11);
	if (globals::features::upscaling.foveatedRender.ariseDisableUiComposite.load(std::memory_order_acquire)) {
		const float clearColor[4]{};
		d3d11Context->ClearRenderTargetView(slot.sealedUI->rtv, clearColor);
	} else {
		d3d11Context->CopyResource(slot.sealedUI->resource11, uiBufferWrapped->resource11);
	}
	SealAriseCapture(slot, presentFrameId);
	{
		std::scoped_lock lock(ariseMutex);
		slot.engineReady = ++ariseEngineValue;
		slot.syncInterval = syncInterval;
		slot.flags = flags;
		slot.frameId = presentFrameId;
		ariseProducerCursor = (slotIndex + 1) % kAriseSlotCount;
	}
	const HRESULT signalResult = d3d11Context->Signal(ariseEngineFence11.get(), slot.engineReady);
	if (FAILED(signalResult)) {
		{
			std::scoped_lock lock(ariseMutex);
			slot.state = AriseSlotState::kWritable;
		}
		IsAriseDeviceFatal(signalResult) ? LatchAriseFailure(signalResult, "D3D11 engineReady signal") :
		                                  EnterAriseSynchronousFallback(signalResult, "D3D11 engineReady signal");
		return signalResult;
	}

	++ariseLogicalPresents;
	HRESULT presentResult = S_OK;
	if (!ariseAsyncTail || ariseRenderDocCompatibility || ariseSynchronousFallback.load(std::memory_order_acquire) ||
		globals::features::upscaling.foveatedRender.ariseForceSynchronous.load(std::memory_order_acquire)) {
		presentResult = ExecuteArisePresent(slotIndex);
	} else if (!NeuralRendering::ARISE::Pipeline::Instance().SubmitTailTask(
			[this, slotIndex] { (void)ExecuteArisePresent(slotIndex); })) {
		EnterAriseSynchronousFallback(E_FAIL, "tail task submission");
		presentResult = ExecuteArisePresent(slotIndex);
	}
	const float clearColor[4]{};
	d3d11Context->ClearRenderTargetView(uiBufferWrapped->rtv, clearColor);
	return presentResult;
}

HRESULT DX12SwapChain::ExecuteArisePresent(std::size_t slotIndex)
{
	auto& slot = ariseSlots[slotIndex];
	auto& streamline = globals::features::upscaling.streamlineDX12;
	{
		std::scoped_lock lock(ariseMutex);
		if (slot.state != AriseSlotState::kQueued)
			return E_UNEXPECTED;
		slot.state = AriseSlotState::kConsuming;
	}

	auto fail = [this, &slot](HRESULT result, const char* operation, bool gpuSubmitted) {
		{
			std::scoped_lock lock(ariseMutex);
			slot.state = gpuSubmitted ? AriseSlotState::kSubmitted : AriseSlotState::kWritable;
			if (!gpuSubmitted)
				slot.tailDone = 0;
		}
		ariseCondition.notify_all();
		NeuralRendering::ARISE::Pipeline::Instance().NotifyWorkerWaiters();
		result = FAILED(result) ? result : E_FAIL;
		if (!gpuSubmitted)
			slot.commandList->Close();
		if (IsAriseDeviceFatal(result))
			LatchAriseFailure(result, operation);
		else
			EnterAriseSynchronousFallback(result, operation);
		return result;
	};
	std::unique_lock presentLock(arisePhysicalPresentMutex, std::defer_lock);
	if (!presentLock.try_lock_for(kAriseWaitTimeout))
		return fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "physical Present lock timeout", false);
	HRESULT result = commandQueue->Wait(ariseEngineFence12.get(), slot.engineReady);
	if (FAILED(result))
		return fail(result, "D3D12 engineReady wait", false);
	if (FAILED(result = slot.allocator->Reset()) ||
		FAILED(result = slot.commandList->Reset(slot.allocator.get(), nullptr)))
		return fail(result, "tail command context reset", false);

	ID3D12Resource* physicalBuffer = swapChainBuffers[frameIndex].get();
	if (!physicalBuffer)
		return fail(DXGI_ERROR_DEVICE_REMOVED, "physical backbuffer acquisition", false);
	NeuralRendering::AriseReconstructionGraph::Result graphResult{
		slot.sealedScene->resource.get(), D3D12_RESOURCE_STATE_COMMON, false };
	const bool graphBypass = globals::features::upscaling.foveatedRender.ariseBypassGraph.load(std::memory_order_acquire);
	const bool graphAvailable = ariseRenderDocCompatibility ?
		ariseGraphAvailable.load(std::memory_order_acquire) : ariseTailNrAvailable.load(std::memory_order_acquire);
	const bool canRunGraph = !graphBypass && graphAvailable && slot.captureValid &&
		slot.sealedDepth && slot.sealedMotionVectors;
	if (canRunGraph) {
		graphResult = ariseReconstructionGraph.Record(slot.commandList.get(), slotIndex,
			{ slot.sealedScene->resource.get(), slot.sealedDepth->resource.get(),
				slot.sealedMotionVectors->resource.get() }, slot.capture.plan);
	}
	if (graphResult.reconstructed)
		++ariseReconstructed;
	else
		++ariseFallback;
	ID3D12Resource* sceneSource = graphResult.finalResource ? graphResult.finalResource : slot.sealedScene->resource.get();
	if (globals::features::upscaling.foveatedRender.ariseDimensionDiagnostics.load(std::memory_order_acquire) &&
		(slot.capture.frameId % 120 == 0)) {
		const auto logicalDesc = slot.sealedScene->resource->GetDesc();
		const auto graphDesc = sceneSource->GetDesc();
		const auto physicalDesc = physicalBuffer->GetDesc();
		DXGI_SWAP_CHAIN_DESC1 actualSwapDesc{};
		const HRESULT descResult = swapChain->GetDesc1(&actualSwapDesc);
		logger::info("[ARISE DIM] Tail logical={}x{} graphOut={}x{} physicalBuffer={}x{} actualSwap={}x{} descResult=0x{:08X} graphBypass={}",
			logicalDesc.Width, logicalDesc.Height, graphDesc.Width, graphDesc.Height,
			physicalDesc.Width, physicalDesc.Height, actualSwapDesc.Width, actualSwapDesc.Height,
			static_cast<uint32_t>(descResult), graphBypass);
	}
	const auto slotIndex32 = static_cast<UINT>(slotIndex);
	const UINT srvStride = d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const UINT rtvStride = d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	auto slotSrv = ariseCompositeSrvHeap->GetGPUDescriptorHandleForHeapStart();
	slotSrv.ptr += static_cast<UINT64>(slotIndex32) * 4 * srvStride;
	auto displayUiRtv = ariseCompositeRtvHeap->GetCPUDescriptorHandleForHeapStart();
	displayUiRtv.ptr += static_cast<SIZE_T>(swapChainDesc.BufferCount + slotIndex32) * rtvStride;
	D3D12_RESOURCE_BARRIER uiScaleBarriers[]{
		CD3DX12_RESOURCE_BARRIER::Transition(slot.sealedUI->resource.get(),
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(slot.displayUI->resource.get(),
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
	};
	slot.commandList->ResourceBarrier(static_cast<UINT>(std::size(uiScaleBarriers)), uiScaleBarriers);
	ID3D12DescriptorHeap* descriptorHeaps[]{ ariseCompositeSrvHeap.get() };
	slot.commandList->SetDescriptorHeaps(1, descriptorHeaps);
	slot.commandList->SetGraphicsRootSignature(ariseCompositeRootSignature.get());
	slot.commandList->SetPipelineState(ariseUiScalePipelineState.get());
	slot.commandList->SetGraphicsRootDescriptorTable(0, slotSrv);
	slot.commandList->OMSetRenderTargets(1, &displayUiRtv, FALSE, nullptr);
	const D3D12_VIEWPORT displayViewport{ 0.0f, 0.0f, static_cast<float>(swapChainDesc.Width),
		static_cast<float>(swapChainDesc.Height), 0.0f, 1.0f };
	const D3D12_RECT displayScissor{ 0, 0, static_cast<LONG>(swapChainDesc.Width), static_cast<LONG>(swapChainDesc.Height) };
	slot.commandList->RSSetViewports(1, &displayViewport);
	slot.commandList->RSSetScissorRects(1, &displayScissor);
	slot.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	slot.commandList->DrawInstanced(3, 1, 0, 0);
	D3D12_RESOURCE_BARRIER uiReadyBarriers[]{
		CD3DX12_RESOURCE_BARRIER::Transition(slot.sealedUI->resource.get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
		CD3DX12_RESOURCE_BARRIER::Transition(slot.displayUI->resource.get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON),
	};
	slot.commandList->ResourceBarrier(static_cast<UINT>(std::size(uiReadyBarriers)), uiReadyBarriers);

	sl::FrameToken* frameToken = nullptr;
	streamline.AcquireFrameToken(slot.frameId, frameToken);
	const bool displaySceneReady = graphResult.reconstructed &&
		sceneSource->GetDesc().Width == swapChainDesc.Width &&
		sceneSource->GetDesc().Height == swapChainDesc.Height;
	const bool useFrameGeneration = useDLSSG && slot.captureValid && displaySceneReady &&
		globals::features::upscaling.ShouldUseFrameGenerationThisFrame() && frameToken;
	if (frameToken) {
		streamline.EmitPCLMarker(sl::PCLMarker::eRenderSubmitStart, *frameToken);
		if (useFrameGeneration) {
			sl::Constants constants = slot.capture.plan.act1Camera.value;
			if (streamline.SetConstants(*frameToken, constants, streamline.viewport)) {
				streamline.TagDX12Resources(*frameToken, slot.commandList.get(),
					slot.sealedDepth->resource.get(), slot.sealedMotionVectors->resource.get(), sceneSource,
					slot.displayUI->resource.get(), slot.capture.guideWidth, slot.capture.guideHeight,
					swapChainDesc.Width, swapChainDesc.Height);
				streamline.ConfigureDLSSG(true, slot.capture.guideWidth, slot.capture.guideHeight,
					swapChainDesc.Width, swapChainDesc.Height,
					static_cast<DXGI_FORMAT>(slot.sealedDepth->resource->GetDesc().Format),
					static_cast<DXGI_FORMAT>(slot.sealedMotionVectors->resource->GetDesc().Format),
					static_cast<DXGI_FORMAT>(sceneSource->GetDesc().Format),
					static_cast<DXGI_FORMAT>(slot.displayUI->resource->GetDesc().Format), swapChainDesc.BufferCount);
			} else {
				streamline.ConfigureDLSSG(false);
			}
		} else if (useDLSSG) {
			streamline.ConfigureDLSSG(false);
		}
	}

	D3D12_RESOURCE_BARRIER compositeBarriers[]{
		CD3DX12_RESOURCE_BARRIER::Transition(sceneSource,
			graphResult.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(slot.displayUI->resource.get(),
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
		CD3DX12_RESOURCE_BARRIER::Transition(physicalBuffer,
			D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET),
	};
	slot.commandList->ResourceBarrier(static_cast<UINT>(std::size(compositeBarriers)), compositeBarriers);
	if (!RecordAriseComposite(slot, sceneSource, physicalBuffer, frameIndex))
		return fail(E_FAIL, "tail scene/UI composite recording", false);
	D3D12_RESOURCE_BARRIER presentBarriers[]{
		CD3DX12_RESOURCE_BARRIER::Transition(sceneSource,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
		CD3DX12_RESOURCE_BARRIER::Transition(slot.displayUI->resource.get(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
		CD3DX12_RESOURCE_BARRIER::Transition(physicalBuffer,
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT),
	};
	slot.commandList->ResourceBarrier(static_cast<UINT>(std::size(presentBarriers)), presentBarriers);
	if (FAILED(result = slot.commandList->Close()))
		return fail(result, "tail command list close", false);
	ID3D12CommandList* lists[]{ slot.commandList.get() };
	commandQueue->ExecuteCommandLists(1, lists);
	if (frameToken) {
		streamline.EmitPCLMarker(sl::PCLMarker::eRenderSubmitEnd, *frameToken);
		streamline.EmitPCLMarker(sl::PCLMarker::ePresentStart, *frameToken);
	}
	const UINT64 tailDone = ariseTailValue + 1;
	if (FAILED(result = commandQueue->Signal(ariseTailFence12.get(), tailDone))) {
		{
			std::scoped_lock lock(ariseMutex);
			slot.state = AriseSlotState::kConsuming;
			slot.tailDone = 0;
		}
		LatchAriseFailure(result, "tailDone signal after GPU submission");
		return result;
	}
	{
		std::scoped_lock lock(ariseMutex);
		ariseTailValue = tailDone;
		slot.tailDone = tailDone;
	}

	result = swapChain->Present(slot.syncInterval, slot.flags);
	if (frameToken)
		streamline.EmitPCLMarker(sl::PCLMarker::ePresentEnd, *frameToken);
	if (FAILED(result))
		return fail(result, "physical Present", true);
	++arisePhysicalPresents;
	++ariseUiComposites;
	frameIndex = swapChain->GetCurrentBackBufferIndex();
	{
		std::scoped_lock lock(ariseMutex);
		slot.state = AriseSlotState::kSubmitted;
	}
	ariseCondition.notify_all();
	NeuralRendering::ARISE::Pipeline::Instance().NotifyWorkerWaiters();
	return S_OK;
}

bool DX12SwapChain::SealAriseCapture(ArisePresentSlot& slot, std::uint64_t presentFrameId)
{
	std::scoped_lock lock(ariseMutex);
	slot.captureValid = false;
	if (!arisePendingCaptureValid || !arisePendingDepth || !arisePendingMotionVectors)
		return false;
	const std::uint64_t presentSequence = ariseLogicalPresents.load(std::memory_order_relaxed) + 1;
	if (arisePendingPresentSequence != presentSequence) {
		logger::warn("[ARISE] Rejecting stale Tail capture: target logical {} current logical {} captured frame {} present frame {}",
			arisePendingPresentSequence, presentSequence, arisePendingCapture.frameId, presentFrameId);
		arisePendingCaptureValid = false;
		arisePendingPresentSequence = 0;
		return false;
	}

	D3D11_TEXTURE2D_DESC depthDesc{};
	D3D11_TEXTURE2D_DESC motionDesc{};
	arisePendingDepth->resource11->GetDesc(&depthDesc);
	arisePendingMotionVectors->resource11->GetDesc(&motionDesc);
	auto matches = [](const std::unique_ptr<WrappedResource>& resource, const D3D11_TEXTURE2D_DESC& desc) {
		if (!resource)
			return false;
		D3D11_TEXTURE2D_DESC current{};
		resource->resource11->GetDesc(&current);
		return current.Width == desc.Width && current.Height == desc.Height && current.Format == desc.Format;
	};
	const auto slotIndex = static_cast<std::size_t>(&slot - ariseSlots.data());
	try {
		if (!matches(slot.sealedDepth, depthDesc))
			slot.sealedDepth = std::make_unique<WrappedResource>(depthDesc, d3d11Device.get(), d3d12Device.get(),
				"ARISE::PresentSlotDepth[" + std::to_string(slotIndex) + "]");
		if (!matches(slot.sealedMotionVectors, motionDesc))
			slot.sealedMotionVectors = std::make_unique<WrappedResource>(motionDesc, d3d11Device.get(), d3d12Device.get(),
				"ARISE::PresentSlotMotion[" + std::to_string(slotIndex) + "]");
	} catch (const std::exception& error) {
		logger::error("[ARISE] Model 3 slot capture allocation failed: {}", error.what());
		return false;
	} catch (...) {
		logger::error("[ARISE] Model 3 slot capture allocation failed");
		return false;
	}
	d3d11Context->CopyResource(slot.sealedDepth->resource11, arisePendingDepth->resource11);
	d3d11Context->CopyResource(slot.sealedMotionVectors->resource11, arisePendingMotionVectors->resource11);
	slot.capture = arisePendingCapture;
	slot.captureValid = true;
	arisePendingCaptureValid = false;
	arisePendingPresentSequence = 0;
	++ariseCaptured;
	return true;
}

void DX12SwapChain::CaptureAriseScene(ID3D11Resource* depth, ID3D11Resource* motionVectors,
	const AriseSceneCapture& capture)
{
	if (!useArise || !depth || !motionVectors || !capture.guideWidth ||
		!capture.guideHeight || !capture.plan.displayWidth || !capture.plan.displayHeight ||
		!capture.plan.nrWidth || !capture.plan.nrHeight)
		return;
	winrt::com_ptr<ID3D11Texture2D> depthTexture;
	winrt::com_ptr<ID3D11Texture2D> motionTexture;
	if (FAILED(depth->QueryInterface(IID_PPV_ARGS(depthTexture.put()))) ||
		FAILED(motionVectors->QueryInterface(IID_PPV_ARGS(motionTexture.put()))))
		return;
	D3D11_TEXTURE2D_DESC depthDesc{};
	D3D11_TEXTURE2D_DESC motionDesc{};
	depthTexture->GetDesc(&depthDesc);
	motionTexture->GetDesc(&motionDesc);
	for (auto* desc : { &depthDesc, &motionDesc }) {
		desc->Usage = D3D11_USAGE_DEFAULT;
		desc->BindFlags = 0;
		desc->CPUAccessFlags = 0;
		desc->MiscFlags = 0;
	}
	auto matches = [](const std::unique_ptr<WrappedResource>& resource, const D3D11_TEXTURE2D_DESC& desc) {
		if (!resource)
			return false;
		D3D11_TEXTURE2D_DESC current{};
		resource->resource11->GetDesc(&current);
		return current.Width == desc.Width && current.Height == desc.Height && current.Format == desc.Format;
	};
	std::scoped_lock lock(ariseMutex);
	try {
		if (!matches(arisePendingDepth, depthDesc))
			arisePendingDepth = std::make_unique<WrappedResource>(depthDesc, d3d11Device.get(), d3d12Device.get(),
				"ARISE::PendingDepth");
		if (!matches(arisePendingMotionVectors, motionDesc))
			arisePendingMotionVectors = std::make_unique<WrappedResource>(motionDesc, d3d11Device.get(), d3d12Device.get(),
				"ARISE::PendingMotion");
	} catch (const std::exception& error) {
		logger::error("[ARISE] Model 3 pending capture allocation failed: {}", error.what());
		arisePendingCaptureValid = false;
		return;
	} catch (...) {
		logger::error("[ARISE] Model 3 pending capture allocation failed");
		arisePendingCaptureValid = false;
		return;
	}
	d3d11Context->CopyResource(arisePendingDepth->resource11, depth);
	d3d11Context->CopyResource(arisePendingMotionVectors->resource11, motionVectors);
	arisePendingCapture = capture;
	arisePendingPresentSequence = ariseLogicalPresents.load(std::memory_order_relaxed) + 1;
	arisePendingCaptureValid = true;
}

void DX12SwapChain::ClearAriseSceneCapture()
{
	std::scoped_lock lock(ariseMutex);
	arisePendingCaptureValid = false;
	arisePendingPresentSequence = 0;
}

bool DX12SwapChain::RecordAriseComposite(ArisePresentSlot& slot, ID3D12Resource* scene,
	ID3D12Resource* physicalBuffer, UINT backBufferIndex)
{
	if (!scene || !physicalBuffer || !ariseCompositeRootSignature || !ariseCompositePipelineState ||
		!ariseCompositeSrvHeap || !ariseCompositeRtvHeap)
		return false;
	const auto slotIndex = static_cast<UINT>(&slot - ariseSlots.data());
	const UINT srvStride = d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const UINT rtvStride = d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	auto srvHandle = ariseCompositeSrvHeap->GetGPUDescriptorHandleForHeapStart();
	srvHandle.ptr += (static_cast<UINT64>(slotIndex) * 4 + 2) * srvStride;
	auto sceneHandle = ariseCompositeSrvHeap->GetCPUDescriptorHandleForHeapStart();
	sceneHandle.ptr += (static_cast<SIZE_T>(slotIndex) * 4 + 3) * srvStride;
	D3D12_SHADER_RESOURCE_VIEW_DESC sceneDesc{};
	sceneDesc.Format = scene->GetDesc().Format;
	sceneDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	sceneDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sceneDesc.Texture2D.MipLevels = 1;
	d3d12Device->CreateShaderResourceView(scene, &sceneDesc, sceneHandle);
	auto rtvHandle = ariseCompositeRtvHeap->GetCPUDescriptorHandleForHeapStart();
	rtvHandle.ptr += static_cast<SIZE_T>(backBufferIndex) * rtvStride;
	ID3D12DescriptorHeap* heaps[]{ ariseCompositeSrvHeap.get() };
	slot.commandList->SetDescriptorHeaps(1, heaps);
	slot.commandList->SetGraphicsRootSignature(ariseCompositeRootSignature.get());
	slot.commandList->SetPipelineState(ariseCompositePipelineState.get());
	slot.commandList->SetGraphicsRootDescriptorTable(0, srvHandle);
	slot.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
	const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(swapChainDesc.Width),
		static_cast<float>(swapChainDesc.Height), 0.0f, 1.0f };
	const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(swapChainDesc.Width), static_cast<LONG>(swapChainDesc.Height) };
	slot.commandList->RSSetViewports(1, &viewport);
	slot.commandList->RSSetScissorRects(1, &scissor);
	slot.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	slot.commandList->DrawInstanced(3, 1, 0, 0);
	return true;
}

bool DX12SwapChain::WaitForAriseTail(UINT64 value)
{
	if (!ariseTailFence12)
		return true;
	const UINT64 completed = ariseTailFence12->GetCompletedValue();
	if (completed == UINT64_MAX) {
		LatchAriseFailure(DXGI_ERROR_DEVICE_REMOVED, "tail fence completion query");
		return false;
	}
	if (completed >= value)
		return true;
	HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!eventHandle) {
		EnterAriseSynchronousFallback(HRESULT_FROM_WIN32(GetLastError()), "tail wait event creation");
		return false;
	}
	const HRESULT eventResult = ariseTailFence12->SetEventOnCompletion(value, eventHandle);
	const DWORD waitResult = SUCCEEDED(eventResult) ? WaitForSingleObject(eventHandle, 2000) : WAIT_FAILED;
	CloseHandle(eventHandle);
	if (FAILED(eventResult) || waitResult != WAIT_OBJECT_0) {
		const HRESULT result = FAILED(eventResult) ? eventResult : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
		IsAriseDeviceFatal(result) ? LatchAriseFailure(result, "tailDone CPU wait") :
		                             EnterAriseSynchronousFallback(result, "tailDone CPU wait");
		return false;
	}
	return true;
}

bool DX12SwapChain::WaitForAriseIdle()
{
	const UINT64 value = ariseTailValue;
	return !value || WaitForAriseTail(value);
}

void DX12SwapChain::ReapAriseSlotsLocked()
{
	if (!ariseTailFence12)
		return;
	const UINT64 completed = ariseTailFence12->GetCompletedValue();
	if (completed == UINT64_MAX) {
		LatchAriseFailure(DXGI_ERROR_DEVICE_REMOVED, "tail fence completion query");
		return;
	}
	for (auto& slot : ariseSlots)
		if (slot.state == AriseSlotState::kSubmitted && completed >= slot.tailDone)
			slot.state = AriseSlotState::kWritable;
}

void DX12SwapChain::LatchAriseFailure(HRESULT result, const char* operation)
{
	if (SUCCEEDED(result))
		result = E_FAIL;
	ariseLastPresentResult.store(result, std::memory_order_release);
	++ariseFailures;
	ariseFatalFailure.store(true, std::memory_order_release);
	logger::error("[ARISE] Model 3 {} failed fatally: HRESULT 0x{:08X}", operation, static_cast<std::uint32_t>(result));
	ariseCondition.notify_all();
	NeuralRendering::ARISE::Pipeline::Instance().NotifyWorkerWaiters();
}

void DX12SwapChain::EnterAriseSynchronousFallback(HRESULT result, const char* operation)
{
	if (SUCCEEDED(result))
		result = E_FAIL;
	long expected = S_OK;
	ariseLastPresentResult.compare_exchange_strong(expected, result, std::memory_order_acq_rel);
	++ariseFailures;
	const bool wasSynchronous = ariseSynchronousFallback.exchange(true, std::memory_order_acq_rel);
	if (!wasSynchronous)
		logger::error("[ARISE] Model 3 {} failed: HRESULT 0x{:08X}; switching to synchronous present",
			operation, static_cast<std::uint32_t>(result));
	ariseCondition.notify_all();
	NeuralRendering::ARISE::Pipeline::Instance().NotifyWorkerWaiters();
}

DX12SwapChain::AriseStatistics DX12SwapChain::GetAriseStatistics() const
{
	return {
		.logicalPresents = ariseLogicalPresents.load(std::memory_order_relaxed),
		.physicalPresents = arisePhysicalPresents.load(std::memory_order_relaxed),
		.captured = ariseCaptured.load(std::memory_order_relaxed),
		.reconstructed = ariseReconstructed.load(std::memory_order_relaxed),
		.fallback = ariseFallback.load(std::memory_order_relaxed),
		.uiComposites = ariseUiComposites.load(std::memory_order_relaxed),
		.backpressure = ariseBackpressure.load(std::memory_order_relaxed),
		.failures = ariseFailures.load(std::memory_order_relaxed),
		.lastPresentResult = static_cast<HRESULT>(ariseLastPresentResult.load(std::memory_order_relaxed)),
		.tailNrAvailable = ariseTailNrAvailable.load(std::memory_order_relaxed),
		.dlssAvailable = ariseReconstructionGraph.IsDlssAvailable(),
		.renderDocCompatibility = ariseRenderDocCompatibility,
		.asyncTail = ariseAsyncTail,
		.graph = ariseReconstructionGraph.GetStatistics(),
		.graphPath = ariseReconstructionGraph.ActivePath(),
	};
}

HRESULT DX12SwapChain::GetDevice(REFIID uuid, void** ppDevice)
{
	if (uuid == __uuidof(ID3D11Device) || uuid == __uuidof(ID3D11Device1) || uuid == __uuidof(ID3D11Device2) || uuid == __uuidof(ID3D11Device3) || uuid == __uuidof(ID3D11Device4) || uuid == __uuidof(ID3D11Device5)) {
		*ppDevice = d3d11Device.get();
		return S_OK;
	}

	return swapChain->GetDevice(uuid, ppDevice);
}

HANDLE DX12SwapChain::GetFrameLatencyWaitableObject()
{
	return swapChain->GetFrameLatencyWaitableObject();
}

float DX12SwapChain::GetFrameTime() const
{
	// Calculate frame time based on swap chain presentation
	static float lastPresentTime = 0.0f;
	static float frameTime = 1.0f / 60.0f;  // Default to 60 fps
	static LARGE_INTEGER frequency = {};
	static LARGE_INTEGER currentTime = {};

	if (frequency.QuadPart == 0) {
		QueryPerformanceFrequency(&frequency);
	}

	QueryPerformanceCounter(&currentTime);
	float time = static_cast<float>(currentTime.QuadPart) / static_cast<float>(frequency.QuadPart);

	if (lastPresentTime > 0.0f) {
		frameTime = time - lastPresentTime;
	}
	lastPresentTime = time;

	return frameTime;
}

WrappedResource::WrappedResource(D3D11_TEXTURE2D_DESC a_texDesc, ID3D11Device5* a_d3d11Device, ID3D12Device* a_d3d12Device, const std::string& a_name)
{
	// Create D3D11 shared texture directly instead of wrapping D3D12 resource
	a_texDesc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	auto throwIfFailed = [&](HRESULT a_result, const char* a_operation) {
		if (FAILED(a_result)) {
			logger::error(
				"[DX12SwapChain] Wrapped resource '{}' {} failed: HRESULT 0x{:08X}, dimensions {}x{}, format {}, bind flags 0x{:X}, misc flags 0x{:X}",
				a_name.empty() ? "<unnamed>" : a_name.c_str(),
				a_operation,
				static_cast<uint32_t>(a_result),
				a_texDesc.Width,
				a_texDesc.Height,
				static_cast<uint32_t>(a_texDesc.Format),
				a_texDesc.BindFlags,
				a_texDesc.MiscFlags);
		}
		DX::ThrowIfFailed(a_result);
	};

	// The raw members are assigned only at the end: a throw before those detaches would
	// leak them, since a partially constructed object runs no destructor.
	winrt::com_ptr<ID3D11Texture2D> texture11;
	throwIfFailed(a_d3d11Device->CreateTexture2D(&a_texDesc, nullptr, texture11.put()), "CreateTexture2D");
	if (!a_name.empty())
		Util::SetResourceName(texture11.get(), "%s", a_name.c_str());

	// Get shared handle from D3D11 texture to enable D3D12 access
	winrt::com_ptr<IDXGIResource1> dxgiResource;
	throwIfFailed(texture11->QueryInterface(IID_PPV_ARGS(dxgiResource.put())), "QueryInterface(IDXGIResource1)");
	winrt::handle sharedHandle;
	throwIfFailed(dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, sharedHandle.put()), "CreateSharedHandle");

	// Open the shared D3D11 texture as D3D12 resource
	winrt::com_ptr<ID3D12Resource> resource12;
	throwIfFailed(a_d3d12Device->OpenSharedHandle(sharedHandle.get(), IID_PPV_ARGS(resource12.put())), "OpenSharedHandle");
	if (!a_name.empty())
		resource12->SetName(winrt::to_hstring(a_name).c_str());

	winrt::com_ptr<ID3D11ShaderResourceView> srv11;
	if (a_texDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = a_texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;

		throwIfFailed(a_d3d11Device->CreateShaderResourceView(texture11.get(), &srvDesc, srv11.put()), "CreateShaderResourceView");
		if (!a_name.empty())
			Util::SetResourceName(srv11.get(), "%s SRV", a_name.c_str());
	}

	winrt::com_ptr<ID3D11UnorderedAccessView> uav11;
	if (a_texDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
		if (a_texDesc.ArraySize > 1) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = a_texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			uavDesc.Texture2DArray.FirstArraySlice = 0;
			uavDesc.Texture2DArray.ArraySize = a_texDesc.ArraySize;

			throwIfFailed(a_d3d11Device->CreateUnorderedAccessView(texture11.get(), &uavDesc, uav11.put()), "CreateUnorderedAccessView");
		} else {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = a_texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;

			throwIfFailed(a_d3d11Device->CreateUnorderedAccessView(texture11.get(), &uavDesc, uav11.put()), "CreateUnorderedAccessView");
		}
		if (!a_name.empty())
			Util::SetResourceName(uav11.get(), "%s UAV", a_name.c_str());
	}

	winrt::com_ptr<ID3D11RenderTargetView> rtv11;
	if (a_texDesc.BindFlags & D3D11_BIND_RENDER_TARGET) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = a_texDesc.Format;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rtvDesc.Texture2D.MipSlice = 0;
		throwIfFailed(a_d3d11Device->CreateRenderTargetView(texture11.get(), &rtvDesc, rtv11.put()), "CreateRenderTargetView");
		if (!a_name.empty())
			Util::SetResourceName(rtv11.get(), "%s RTV", a_name.c_str());
	}

	resource11 = texture11.detach();
	srv = srv11.detach();
	uav = uav11.detach();
	rtv = rtv11.detach();
	resource = std::move(resource12);
}

WrappedResource::~WrappedResource()
{
	if (resource11) {
		resource11->Release();
		resource11 = nullptr;
	}
	if (srv) {
		srv->Release();
		srv = nullptr;
	}
	if (uav) {
		uav->Release();
		uav = nullptr;
	}
	if (rtv) {
		rtv->Release();
		rtv = nullptr;
	}
	// resource (winrt::com_ptr) will be automatically released
}

void SharedFence::Create(ID3D12Device* a_device12, ID3D11Device5* a_device11, const char* a_name)
{
	DX::ThrowIfFailed(a_device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12)));
	winrt::handle sharedHandle;
	DX::ThrowIfFailed(a_device12->CreateSharedHandle(fence12.get(), nullptr, GENERIC_ALL, nullptr, sharedHandle.put()));
	DX::ThrowIfFailed(a_device11->OpenSharedFence(sharedHandle.get(), IID_PPV_ARGS(&fence11)));

	fence12->SetName(winrt::to_hstring(a_name).c_str());
	Util::SetResourceName(fence11.get(), "%s", a_name);
}

bool SharedFence::CpuWait(uint64_t a_value, DWORD a_timeoutMs) const
{
	if (!fence12 || a_value == 0)
		return true;
	if (fence12->GetCompletedValue() >= a_value)
		return true;

	winrt::handle fenceEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
	if (!fenceEvent)
		return false;
	if (FAILED(fence12->SetEventOnCompletion(a_value, fenceEvent.get())))
		return false;

	winrt::com_ptr<ID3D12Device> device12;
	DWORD waitedMs = 0;
	while (waitedMs < a_timeoutMs) {
		const DWORD sliceMs = std::min<DWORD>(kRemovalPollMs, a_timeoutMs - waitedMs);
		const DWORD waitResult = WaitForSingleObject(fenceEvent.get(), sliceMs);
		if (waitResult == WAIT_OBJECT_0)
			return true;
		if (waitResult != WAIT_TIMEOUT)
			return false;
		waitedMs += sliceMs;
		if (!device12)
			fence12->GetDevice(IID_PPV_ARGS(&device12));
		if (device12 && FAILED(device12->GetDeviceRemovedReason()))
			return false;
	}

	return false;
}

DXGISwapChainProxy::DXGISwapChainProxy(IDXGISwapChain4* a_swapChain)
{
	swapChain = a_swapChain;
}

/****IUknown****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::QueryInterface(REFIID riid, void** ppvObj)
{
	if (!ppvObj)
		return E_POINTER;
	*ppvObj = nullptr;
	// Keep COM identity on this proxy. Forwarding QI to the physical chain
	// leaks D through GetDesc1 and hands out a mismatched vtable.
	if (!IsProxySwapChainInterface(riid))
		return E_NOINTERFACE;
	*ppvObj = static_cast<IDXGISwapChain4*>(this);
	AddRef();
	return S_OK;
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::AddRef()
{
	return swapChain->AddRef();
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::Release()
{
	return swapChain->Release();
}

/****IDXGIObject****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateData(_In_ REFGUID Name, UINT DataSize, _In_reads_bytes_(DataSize) const void* pData)
{
	return swapChain->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateDataInterface(_In_ REFGUID Name, _In_opt_ const IUnknown* pUnknown)
{
	return swapChain->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetPrivateData(_In_ REFGUID Name, _Inout_ UINT* pDataSize, _Out_writes_bytes_(*pDataSize) void* pData)
{
	return swapChain->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetParent(_In_ REFIID riid, _COM_Outptr_ void** ppParent)
{
	return swapChain->GetParent(riid, ppParent);
}

/****IDXGIDeviceSubObject****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice)
{
	return globals::features::upscaling.dx12SwapChain.GetDevice(riid, ppDevice);
}

/****IDXGISwapChain****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::Present(UINT SyncInterval, UINT Flags)
{
	return globals::features::upscaling.dx12SwapChain.Present(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetBuffer(UINT buffer, _In_ REFIID riid, _COM_Outptr_ void** ppSurface)
{
	return globals::features::upscaling.dx12SwapChain.GetBuffer(buffer, riid, ppSurface);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetFullscreenState(BOOL Fullscreen, _In_opt_ IDXGIOutput* pTarget)
{
	return swapChain->SetFullscreenState(Fullscreen, pTarget);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFullscreenState(_Out_opt_ BOOL* pFullscreen, _COM_Outptr_opt_result_maybenull_ IDXGIOutput** ppTarget)
{
	return swapChain->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDesc(_Out_ DXGI_SWAP_CHAIN_DESC* pDesc)
{
	if (!pDesc)
		return E_POINTER;
	const HRESULT result = swapChain->GetDesc(pDesc);
	if (SUCCEEDED(result) && globals::features::upscaling.dx12SwapChain.useArise) {
		pDesc->BufferDesc.Width = globals::features::upscaling.dx12SwapChain.LogicalWidth();
		pDesc->BufferDesc.Height = globals::features::upscaling.dx12SwapChain.LogicalHeight();
	}
	return result;
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
	return globals::features::upscaling.dx12SwapChain.ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeTarget(_In_ const DXGI_MODE_DESC* pNewTargetParameters)
{
	if (globals::features::upscaling.dx12SwapChain.useArise && pNewTargetParameters) {
		DXGI_MODE_DESC physicalMode = *pNewTargetParameters;
		MapLogicalExtentToPhysical(globals::features::upscaling.dx12SwapChain,
			physicalMode.Width, physicalMode.Height);
		return swapChain->ResizeTarget(&physicalMode);
	}
	return swapChain->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetContainingOutput(_COM_Outptr_ IDXGIOutput** ppOutput)
{
	return swapChain->GetContainingOutput(ppOutput);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFrameStatistics(_Out_ DXGI_FRAME_STATISTICS* pStats)
{
	return swapChain->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetLastPresentCount(_Out_ UINT* pLastPresentCount)
{
	return swapChain->GetLastPresentCount(pLastPresentCount);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDesc1(_Out_ DXGI_SWAP_CHAIN_DESC1* pDesc)
{
	if (!pDesc)
		return E_POINTER;
	const HRESULT result = swapChain->GetDesc1(pDesc);
	if (SUCCEEDED(result) && globals::features::upscaling.dx12SwapChain.useArise) {
		pDesc->Width = globals::features::upscaling.dx12SwapChain.LogicalWidth();
		pDesc->Height = globals::features::upscaling.dx12SwapChain.LogicalHeight();
	}
	return result;
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFullscreenDesc(_Out_ DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
	return swapChain->GetFullscreenDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetHwnd(_Out_ HWND* pHwnd)
{
	return swapChain->GetHwnd(pHwnd);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetCoreWindow(_In_ REFIID refiid, _COM_Outptr_ void** ppUnk)
{
	return swapChain->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::Present1(UINT SyncInterval, UINT PresentFlags,
	_In_ const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
	(void)pPresentParameters;
	return Present(SyncInterval, PresentFlags);
}

BOOL STDMETHODCALLTYPE DXGISwapChainProxy::IsTemporaryMonoSupported()
{
	return swapChain->IsTemporaryMonoSupported();
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetRestrictToOutput(_COM_Outptr_ IDXGIOutput** ppRestrictToOutput)
{
	return swapChain->GetRestrictToOutput(ppRestrictToOutput);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetBackgroundColor(_In_ const DXGI_RGBA* pColor)
{
	return swapChain->SetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetBackgroundColor(_Out_ DXGI_RGBA* pColor)
{
	return swapChain->GetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetRotation(_In_ DXGI_MODE_ROTATION Rotation)
{
	return swapChain->SetRotation(Rotation);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetRotation(_Out_ DXGI_MODE_ROTATION* pRotation)
{
	return swapChain->GetRotation(pRotation);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetSourceSize(UINT Width, UINT Height)
{
	auto& owner = globals::features::upscaling.dx12SwapChain;
	if (owner.useArise)
		MapLogicalExtentToPhysical(owner, Width, Height);
	return swapChain->SetSourceSize(Width, Height);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetSourceSize(_Out_ UINT* pWidth, _Out_ UINT* pHeight)
{
	if (!pWidth || !pHeight)
		return E_POINTER;
	const HRESULT result = swapChain->GetSourceSize(pWidth, pHeight);
	if (SUCCEEDED(result) && globals::features::upscaling.dx12SwapChain.useArise) {
		*pWidth = globals::features::upscaling.dx12SwapChain.LogicalWidth();
		*pHeight = globals::features::upscaling.dx12SwapChain.LogicalHeight();
	}
	return result;
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetMaximumFrameLatency(UINT MaxLatency)
{
	return swapChain->SetMaximumFrameLatency(MaxLatency);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetMaximumFrameLatency(_Out_ UINT* pMaxLatency)
{
	return swapChain->GetMaximumFrameLatency(pMaxLatency);
}

HANDLE STDMETHODCALLTYPE DXGISwapChainProxy::GetFrameLatencyWaitableObject()
{
	return globals::features::upscaling.dx12SwapChain.GetFrameLatencyWaitableObject();
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
	return swapChain->SetMatrixTransform(pMatrix);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetMatrixTransform(_Out_ DXGI_MATRIX_3X2_F* pMatrix)
{
	return swapChain->GetMatrixTransform(pMatrix);
}

UINT STDMETHODCALLTYPE DXGISwapChainProxy::GetCurrentBackBufferIndex()
{
	// GetBuffer only exposes the single logical R wrapper at index 0.
	return 0;
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::CheckColorSpaceSupport(
	_In_ DXGI_COLOR_SPACE_TYPE ColorSpace, _Out_ UINT* pColorSpaceSupport)
{
	return swapChain->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetColorSpace1(_In_ DXGI_COLOR_SPACE_TYPE ColorSpace)
{
	auto& owner = globals::features::upscaling.dx12SwapChain;
	if (owner.useArise && ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
		logger::warn("[ARISE] Ignoring HDR color-space request while Model 3 is active");
		ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
	}
	return swapChain->SetColorSpace1(ColorSpace);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
	DXGI_FORMAT Format, UINT SwapChainFlags, _In_reads_(BufferCount) const UINT* pCreationNodeMask,
	_In_reads_(BufferCount) IUnknown* const* ppPresentQueue)
{
	(void)pCreationNodeMask;
	(void)ppPresentQueue;
	return ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetHDRMetaData(
	_In_ DXGI_HDR_METADATA_TYPE Type, UINT Size, _In_reads_bytes_opt_(Size) void* pMetaData)
{
	return swapChain->SetHDRMetaData(Type, Size, pMetaData);
}

void DX12SwapChain::ApplyLogicalRenderExtents() const
{
	if (!useArise || globals::game::isVR)
		return;
	auto* viewport = globals::game::graphicsState;
	if (!viewport)
		return;
	viewport->screenWidth = LogicalWidth();
	viewport->screenHeight = LogicalHeight();
	viewport->frameBufferViewport[0] = LogicalWidth();
	viewport->frameBufferViewport[1] = LogicalHeight();
	auto& runtimeData = viewport->GetRuntimeData();
	runtimeData.dynamicResolutionPreviousWidthRatio = 1;
	runtimeData.dynamicResolutionPreviousHeightRatio = 1;
	runtimeData.dynamicResolutionWidthRatio = 1;
	runtimeData.dynamicResolutionHeightRatio = 1;
	runtimeData.dynamicResolutionLock = 1;
}

void DX12SwapChain::SetColorSpace(bool enableHDR)
{
	if (!swapChain)
		return;
	if (useArise && enableHDR) {
		logger::warn("[ARISE] Ignoring HDR color-space request while Model 3 is active");
		enableHDR = false;
	}

	if (enableHDR) {
		swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
		logger::info("[DX12SwapChain] Set color space to HDR10 (PQ/BT.2020)");
	} else {
		swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
		logger::info("[DX12SwapChain] Set color space to SDR (sRGB)");
	}
}

DX12SwapChain::BlurResources DX12SwapChain::GetBlurResources() const
{
	BlurResources res;
	if (swapChainBufferWrapped) {
		res.backbufferTex = swapChainBufferWrapped->resource11;
		res.backbufferRTV = swapChainBufferWrapped->rtv;
		res.backbufferSRV = swapChainBufferWrapped->srv;
	}
	if (uiBufferWrapped) {
		res.uiBufferSRV = uiBufferWrapped->srv;
		res.uiBufferRTV = uiBufferWrapped->rtv;
	}
	return res;
}

void DX12SwapChain::CreateSharedResources()
{
	auto renderer = globals::game::renderer;

	// Create depth buffer
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	D3D11_TEXTURE2D_DESC texDesc{};
	main.texture->GetDesc(Util::AsW32(&texDesc));
	texDesc.Format = DXGI_FORMAT_R32_FLOAT;
	depthBufferShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get(), "DX12SwapChain::DepthBufferShared");

	// Create motion vector buffer
	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	motionVector.texture->GetDesc(Util::AsW32(&texDesc));
	motionVectorBufferShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get(), "DX12SwapChain::MotionVectorBufferShared");
}
