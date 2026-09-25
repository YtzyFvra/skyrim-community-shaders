#include "VrBoosterTail.h"

#include "NeuralRendering/AriseReconstructionGraph.h"
#include "NeuralRendering/Renderer.h"
#include "../Upscaling.h"
#include "../DLSSBooster.h"

#include <atomic>

#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#ifdef ENABLE_SKYRIM_VR
#	include <sl.h>
#endif

using Microsoft::WRL::ComPtr;

namespace
{
	std::atomic<std::uint32_t> sealedFrame{ UINT32_MAX };

	ComPtr<ID3D11Texture2D> present11;
	HANDLE presentHandle = nullptr;
	std::uint32_t presentW = 0;
	std::uint32_t presentH = 0;

	struct Gpu
	{
		bool ready = false;
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		ComPtr<ID3D12Fence> fence;
		HANDLE fenceEvent = nullptr;
		std::uint64_t fenceValue = 0;
		ComPtr<ID3D12Resource> present12;
		ComPtr<ID3D12Resource> eyeColor[2];
		ComPtr<ID3D12Resource> eyeDepth[2];
		ComPtr<ID3D12Resource> eyeMotion[2];
		ComPtr<ID3D12Resource> cropColor[2];
		ComPtr<ID3D12Resource> cropDepth[2];
		ComPtr<ID3D12Resource> cropMotion[2];
		std::uint32_t eyeW = 0;
		std::uint32_t eyeH = 0;
		std::uint32_t cropW = 0;
		std::uint32_t cropH = 0;
		NeuralRendering::AriseReconstructionGraph graph;
		std::uint64_t planSignature = 0;
		bool havePlan = false;
	};

	Gpu& TailGpu()
	{
		static Gpu gpu;
		return gpu;
	}

	void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
		D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		if (!list || !resource || before == after)
			return;
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = resource;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = after;
		list->ResourceBarrier(1, &barrier);
	}

	bool EnsureTex(ID3D12Device* device, ComPtr<ID3D12Resource>& tex, std::uint32_t w, std::uint32_t h,
		DXGI_FORMAT format, const wchar_t* name)
	{
		if (tex) {
			const auto desc = tex->GetDesc();
			if (desc.Width == w && desc.Height == h && desc.Format == format)
				return true;
			tex.Reset();
		}
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = w;
		desc.Height = h;
		desc.MipLevels = 1;
		desc.DepthOrArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex))))
			return false;
		tex->SetName(name);
		return true;
	}

	void CopyBox(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* dst,
		std::uint32_t srcX, std::uint32_t srcY, std::uint32_t w, std::uint32_t h,
		std::uint32_t dstX, std::uint32_t dstY)
	{
		D3D12_TEXTURE_COPY_LOCATION srcLoc{ src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
		srcLoc.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION dstLoc{ dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
		dstLoc.SubresourceIndex = 0;
		D3D12_BOX box{ srcX, srcY, 0, srcX + w, srcY + h, 1 };
		list->CopyTextureRegion(&dstLoc, dstX, dstY, 0, &srcLoc, &box);
	}
}

namespace VrBoosterTail
{
	bool UseAsyncTail()
	{
#ifdef ENABLE_SKYRIM_VR
		auto& upscaling = globals::features::upscaling;
		return globals::game::isVR && upscaling.IsVrDlssPostInjectActive() &&
			upscaling.streamlineDX12.initialized;
#else
		return false;
#endif
	}

	void NoteSealed(std::uint32_t frame)
	{
		sealedFrame.store(frame, std::memory_order_release);
	}

	bool OwnsSubmit()
	{
		const auto* state = globals::state;
		return state && sealedFrame.load(std::memory_order_acquire) == state->frameCount;
	}

	void PreparePresent(std::uint32_t width, std::uint32_t height)
	{
		if (!width || !height || (present11 && presentW == width && presentH == height))
			return;
		present11.Reset();
		if (presentHandle) {
			CloseHandle(presentHandle);
			presentHandle = nullptr;
		}
		if (!globals::d3d::device)
			return;
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		if (FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, present11.GetAddressOf())))
			return;
		ComPtr<IDXGIResource1> dxgi;
		if (FAILED(present11.As(&dxgi)) ||
			FAILED(dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &presentHandle))) {
			present11.Reset();
			return;
		}
		presentW = width;
		presentH = height;
		TailGpu().present12.Reset();
	}

	ID3D11Texture2D* PresentTexture()
	{
		return present11.Get();
	}

	ID3D12Resource* Reconstruct(ID3D12Device* device, ID3D12CommandQueue* queue,
		ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* mvec,
		std::uint32_t width, std::uint32_t height,
		std::uint32_t guideWidth, std::uint32_t guideHeight,
		DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat, DXGI_FORMAT motionFormat,
		const sl::Constants* eyeConstants,
		std::uint32_t& outWidth, std::uint32_t& outHeight, DXGI_FORMAT& outFormat)
	{
#ifndef ENABLE_SKYRIM_VR
		(void)device; (void)queue; (void)color; (void)depth; (void)mvec;
		(void)width; (void)height; (void)guideWidth; (void)guideHeight;
		(void)colorFormat; (void)depthFormat; (void)motionFormat; (void)eyeConstants;
		(void)outWidth; (void)outHeight; (void)outFormat;
		return nullptr;
#else
		if (sealedFrame.load(std::memory_order_acquire) == UINT32_MAX || !device || !queue || !color || !depth || !mvec || width < 2 || !height ||
			guideWidth != width || guideHeight != height)
			return nullptr;
		auto& gpu = TailGpu();
		auto& upscaling = globals::features::upscaling;
		if (!gpu.ready) {
			ComPtr<IDXGIDevice> dxgiDevice;
			ComPtr<IDXGIAdapter> adapter;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())))
				return nullptr;
			upscaling.streamlineDX12.SetD3DDevice12(device);
			upscaling.streamlineDX12.CheckFeatures(adapter.Get());
			upscaling.streamlineDX12.PostDevice();
			if (!gpu.graph.Initialize(device, colorFormat, 4, &upscaling.streamlineDX12))
				return nullptr;
			if (!NeuralRendering::Renderer::Instance().InitializeModel3(device, [&gpu] {
					if (!gpu.fence || !gpu.fenceValue)
						return true;
					return gpu.fence->GetCompletedValue() >= gpu.fenceValue;
				}))
				return nullptr;
			if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator))) ||
				FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr, IID_PPV_ARGS(&gpu.list))) ||
				FAILED(gpu.list->Close()) ||
				FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence))))
				return nullptr;
			gpu.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			gpu.ready = gpu.fenceEvent != nullptr;
			if (!gpu.ready)
				return nullptr;
			logger::info("[VrBoosterTail] DX12 reconstruction tail ready");
		}

		const std::uint32_t engineW = width / 2;
		const std::uint32_t engineH = height;
		const bool logical = upscaling.perfMode.IsLogicalEyeActive() &&
			upscaling.perfMode.GetDisplayEyeWidth() && upscaling.perfMode.GetDisplayEyeHeight();
		const std::uint32_t displayW = logical ? upscaling.perfMode.GetDisplayEyeWidth() : engineW;
		const std::uint32_t displayH = logical ? upscaling.perfMode.GetDisplayEyeHeight() : engineH;
		const Util::Subrect::UVRegion* dbLeft = nullptr;
		const Util::Subrect::UVRegion* dbRight = nullptr;
		const auto& left = globals::features::dlssBooster.TryGetVrSubrect(dbLeft, dbRight) && dbLeft ?
			*dbLeft : upscaling.foveatedRender.subrectController.GetUV();
		const auto& right = globals::features::dlssBooster.TryGetVrSubrect(dbLeft, dbRight) && dbRight ?
			*dbRight : upscaling.foveatedRender.subrectController.GetRightEyeUV();
		if (left.w != right.w || left.h != right.h)
			return nullptr;
		const std::uint32_t subInW = std::max(1u, static_cast<std::uint32_t>(engineW * left.w));
		const std::uint32_t subInH = std::max(1u, static_cast<std::uint32_t>(engineH * left.h));
		const std::uint32_t subOutW = std::max(1u, static_cast<std::uint32_t>(displayW * left.w));
		const std::uint32_t subOutH = std::max(1u, static_cast<std::uint32_t>(displayH * left.h));
		auto flat = upscaling.foveatedRender.BuildFlatPipelinePlan(subOutW, subOutH, subInW, subInH);
		if (!flat.engineWidth || !flat.displayWidth || !flat.nrWidth)
			return nullptr;

		auto& foveated = upscaling.foveatedRender;
		if (!EnsureTex(device, gpu.eyeColor[0], engineW, engineH, colorFormat, L"VrBoosterTail::EyeColor0") ||
			!EnsureTex(device, gpu.eyeColor[1], engineW, engineH, colorFormat, L"VrBoosterTail::EyeColor1") ||
			!EnsureTex(device, gpu.eyeDepth[0], engineW, engineH, depthFormat, L"VrBoosterTail::EyeDepth0") ||
			!EnsureTex(device, gpu.eyeDepth[1], engineW, engineH, depthFormat, L"VrBoosterTail::EyeDepth1") ||
			!EnsureTex(device, gpu.eyeMotion[0], engineW, engineH, motionFormat, L"VrBoosterTail::EyeMotion0") ||
			!EnsureTex(device, gpu.eyeMotion[1], engineW, engineH, motionFormat, L"VrBoosterTail::EyeMotion1") ||
			!EnsureTex(device, gpu.cropColor[0], subInW, subInH, colorFormat, L"VrBoosterTail::CropColor0") ||
			!EnsureTex(device, gpu.cropColor[1], subInW, subInH, colorFormat, L"VrBoosterTail::CropColor1") ||
			!EnsureTex(device, gpu.cropDepth[0], subInW, subInH, depthFormat, L"VrBoosterTail::CropDepth0") ||
			!EnsureTex(device, gpu.cropDepth[1], subInW, subInH, depthFormat, L"VrBoosterTail::CropDepth1") ||
			!EnsureTex(device, gpu.cropMotion[0], subInW, subInH, motionFormat, L"VrBoosterTail::CropMotion0") ||
			!EnsureTex(device, gpu.cropMotion[1], subInW, subInH, motionFormat, L"VrBoosterTail::CropMotion1"))
			return nullptr;
		if (presentHandle && !gpu.present12) {
			if (FAILED(device->OpenSharedHandle(presentHandle, IID_PPV_ARGS(&gpu.present12))))
				return nullptr;
		}

		if (gpu.fence && gpu.fenceEvent && gpu.fenceValue &&
			gpu.fence->GetCompletedValue() < gpu.fenceValue) {
			gpu.fence->SetEventOnCompletion(gpu.fenceValue, gpu.fenceEvent);
			WaitForSingleObject(gpu.fenceEvent, 2000);
		}
		if (FAILED(gpu.allocator->Reset()) || FAILED(gpu.list->Reset(gpu.allocator.Get(), nullptr)))
			return nullptr;
		auto* list = gpu.list.Get();
		Transition(list, color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Transition(list, depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Transition(list, mvec, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
		const Util::Subrect::UVRegion eyesUv[2]{ left, right };
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			const auto& uv = eyesUv[eye];
			const std::uint32_t eyeX = eye == 0 ? 0 : engineW;
			const std::uint32_t cropX = eyeX + static_cast<std::uint32_t>(uv.x * engineW);
			const std::uint32_t cropY = static_cast<std::uint32_t>(uv.y * engineH);
			Transition(list, gpu.eyeColor[eye].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			Transition(list, gpu.eyeDepth[eye].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			Transition(list, gpu.eyeMotion[eye].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			Transition(list, gpu.cropColor[eye].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			Transition(list, gpu.cropDepth[eye].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			Transition(list, gpu.cropMotion[eye].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			CopyBox(list, color, gpu.eyeColor[eye].Get(), eyeX, 0, engineW, engineH, 0, 0);
			CopyBox(list, depth, gpu.eyeDepth[eye].Get(), eyeX, 0, engineW, engineH, 0, 0);
			CopyBox(list, mvec, gpu.eyeMotion[eye].Get(), eyeX, 0, engineW, engineH, 0, 0);
			CopyBox(list, color, gpu.cropColor[eye].Get(), cropX, cropY, subInW, subInH, 0, 0);
			CopyBox(list, depth, gpu.cropDepth[eye].Get(), cropX, cropY, subInW, subInH, 0, 0);
			CopyBox(list, mvec, gpu.cropMotion[eye].Get(), cropX, cropY, subInW, subInH, 0, 0);
			Transition(list, gpu.eyeColor[eye].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Transition(list, gpu.eyeDepth[eye].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Transition(list, gpu.eyeMotion[eye].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Transition(list, gpu.cropColor[eye].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Transition(list, gpu.cropDepth[eye].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Transition(list, gpu.cropMotion[eye].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}
		Transition(list, color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
		Transition(list, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
		Transition(list, mvec, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

		using Graph = NeuralRendering::AriseReconstructionGraph;
		Graph::Plan upsample{};
		upsample.eligible = true;
		upsample.requested = false;
		upsample.act1 = Graph::Act1Backend::kBypass;
		upsample.sceneInputWidth = engineW;
		upsample.sceneInputHeight = engineH;
		upsample.engineWidth = engineW;
		upsample.engineHeight = engineH;
		upsample.displayWidth = displayW;
		upsample.displayHeight = displayH;
		upsample.act1OutputWidth = displayW;
		upsample.act1OutputHeight = displayH;
		upsample.nrWidth = displayW;
		upsample.nrHeight = displayH;
		upsample.act1Downsample = 1;
		Graph::Plan sharp = upsample;
		sharp.requested = foveated.settings.neuralRenderingEnabled;
		sharp.act1 = Graph::Act1Backend::kDLSS;
		sharp.sceneInputWidth = flat.engineWidth;
		sharp.sceneInputHeight = flat.engineHeight;
		sharp.engineWidth = flat.engineWidth;
		sharp.engineHeight = flat.engineHeight;
		sharp.displayWidth = flat.displayWidth;
		sharp.displayHeight = flat.displayHeight;
		sharp.act1OutputWidth = flat.act1OutputWidth;
		sharp.act1OutputHeight = flat.act1OutputHeight;
		sharp.nrWidth = flat.nrWidth;
		sharp.nrHeight = flat.nrHeight;
		sharp.act1Quality = flat.act1Quality;
		sharp.act1Preset = flat.act1Preset;
		sharp.act2Quality = flat.act2Quality;
		sharp.act2Preset = foveated.settings.flatPipelineAct2Preset;
		sharp.act2Sr = flat.act2SrBackend == FoveatedRender::FlatAct2SrBackend::kDLSS ?
			Graph::Act2SrBackend::kDLSS : Graph::Act2SrBackend::kLanczos;
		sharp.sharpen = foveated.settings.flatPipelineSharpen;
		sharp.sharpness = foveated.settings.flatPipelineSharpness;
		sharp.motionVectorScaleX = static_cast<float>(engineW) * static_cast<float>(flat.nrWidth) / static_cast<float>(subInW);
		sharp.motionVectorScaleY = static_cast<float>(engineH) * static_cast<float>(flat.nrHeight) / static_cast<float>(subInH);
		sharp.tuning = {
			foveated.settings.neuralRenderingIntensity,
			foveated.settings.neuralRenderingLocalTone,
			foveated.settings.neuralRenderingLocalStructure,
			foveated.settings.neuralRenderingSkinStructure,
			foveated.settings.neuralRenderingStyle,
			foveated.settings.neuralRenderingAutoMask,
			foveated.settings.neuralRenderingUICorrection,
			0.0f, 0.0f, false
		};
		const std::uint64_t signature = (static_cast<std::uint64_t>(sharp.sceneInputWidth) << 48) ^
			(static_cast<std::uint64_t>(sharp.displayWidth) << 32) ^
			(static_cast<std::uint64_t>(sharp.nrWidth) << 16) ^ sharp.act1Quality;
		sharp.reset = !gpu.havePlan || gpu.planSignature != signature;
		gpu.havePlan = true;
		gpu.planSignature = signature;
		if (eyeConstants) {
			sharp.act1Camera.value = eyeConstants[0];
			sharp.act2Camera.value = eyeConstants[0];
		} else {
			sharp.act1Camera.value.jitterOffset = { -upscaling.jitter.x, -upscaling.jitter.y };
			sharp.act2Camera.value.jitterOffset = sharp.act1Camera.value.jitterOffset;
		}
		const float mvecX = left.w > 0.001f && left.w < 0.999f ? 1.0f / left.w : 1.0f;
		const float mvecY = left.h > 0.001f && left.h < 0.999f ? 1.0f / left.h : 1.0f;
		const float act2ScaleX = static_cast<float>(flat.nrWidth) / static_cast<float>(std::max(flat.engineWidth, 1u));
		const float act2ScaleY = static_cast<float>(flat.nrHeight) / static_cast<float>(std::max(flat.engineHeight, 1u));
		auto applyJitterPolicy = [&](sl::Constants& act1, sl::Constants& act2) {
			act1.mvecScale = { mvecX, mvecY };
			act2.mvecScale = { mvecX, mvecY };
			if (foveated.settings.flatPipelineAct1Jitter != 0)
				act1.jitterOffset = { 0.0f, 0.0f };
			if (foveated.settings.flatPipelineAct2Jitter == 0) {
				act2.jitterOffset.x *= act2ScaleX;
				act2.jitterOffset.y *= act2ScaleY;
			} else {
				act2.jitterOffset = { 0.0f, 0.0f };
			}
		};
		applyJitterPolicy(sharp.act1Camera.value, sharp.act2Camera.value);

		ID3D12Resource* eyeOut[2]{};
		ID3D12Resource* boxOut[2]{};
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			Graph::Plan eyeUpsample = upsample;
			eyeUpsample.frameId = gpu.fenceValue + 1;
			auto up = gpu.graph.Record(list, 2 + eye,
				{ gpu.eyeColor[eye].Get(), gpu.eyeDepth[eye].Get(), gpu.eyeMotion[eye].Get() }, eyeUpsample);
			eyeOut[eye] = up.finalResource;
			Graph::Plan eyeSharp = sharp;
			eyeSharp.frameId = gpu.fenceValue + 1;
			eyeSharp.nrSlot = eye;
			eyeSharp.act1Viewport = eye == 0 ? NeuralRendering::AriseDlssProvider::kAct1Viewport :
				NeuralRendering::AriseDlssProvider::kAct1ViewportRight;
			eyeSharp.act2Viewport = eye == 0 ? NeuralRendering::AriseDlssProvider::kAct2Viewport :
				NeuralRendering::AriseDlssProvider::kAct2ViewportRight;
			if (eye == 1 && eyeConstants) {
				eyeSharp.act1Camera.value = eyeConstants[1];
				eyeSharp.act2Camera.value = eyeConstants[1];
				applyJitterPolicy(eyeSharp.act1Camera.value, eyeSharp.act2Camera.value);
			}
			auto box = gpu.graph.Record(list, eye,
				{ gpu.cropColor[eye].Get(), gpu.cropDepth[eye].Get(), gpu.cropMotion[eye].Get() }, eyeSharp);
			boxOut[eye] = box.reconstructed ? box.finalResource : nullptr;
		}
		if (!eyeOut[0] || !eyeOut[1]) {
			gpu.list->Close();
			return nullptr;
		}

		static ComPtr<ID3D12Resource> displaySbs[2];
		const std::uint32_t sbsW = displayW * 2;
		const std::uint32_t sbsH = displayH;
		const std::uint32_t buffer = static_cast<std::uint32_t>(gpu.fenceValue & 1u);
		if (!EnsureTex(device, displaySbs[buffer], sbsW, sbsH, DXGI_FORMAT_R16G16B16A16_FLOAT, L"VrBoosterTail::DisplaySbs")) {
			gpu.list->Close();
			return nullptr;
		}
		Transition(list, displaySbs[buffer].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			Transition(list, eyeOut[eye], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
			CopyBox(list, eyeOut[eye], displaySbs[buffer].Get(), 0, 0, displayW, displayH, eye == 0 ? 0 : displayW, 0);
			Transition(list, eyeOut[eye], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			if (!boxOut[eye])
				continue;
			const auto& uv = eyesUv[eye];
			std::uint32_t boxX = static_cast<std::uint32_t>(uv.x * displayW);
			std::uint32_t boxY = static_cast<std::uint32_t>(uv.y * displayH);
			if (subOutW < displayW && boxX + subOutW > displayW)
				boxX = displayW - subOutW;
			if (subOutH < displayH && boxY + subOutH > displayH)
				boxY = displayH - subOutH;
			Transition(list, boxOut[eye], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
			CopyBox(list, boxOut[eye], displaySbs[buffer].Get(), 0, 0, subOutW, subOutH,
				(eye == 0 ? 0 : displayW) + boxX, boxY);
			Transition(list, boxOut[eye], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
		}
		if (gpu.present12) {
			Transition(list, gpu.present12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			list->CopyResource(gpu.present12.Get(), displaySbs[buffer].Get());
			Transition(list, gpu.present12.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		}
		Transition(list, displaySbs[buffer].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			Transition(list, gpu.eyeColor[eye].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			Transition(list, gpu.eyeDepth[eye].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			Transition(list, gpu.eyeMotion[eye].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			Transition(list, gpu.cropColor[eye].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			Transition(list, gpu.cropDepth[eye].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			Transition(list, gpu.cropMotion[eye].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		}
		if (FAILED(list->Close()))
			return nullptr;
		ID3D12CommandList* lists[]{ list };
		queue->ExecuteCommandLists(1, lists);
		const std::uint64_t signal = ++gpu.fenceValue;
		if (FAILED(queue->Signal(gpu.fence.Get(), signal)))
			return nullptr;
		if (gpu.fence->GetCompletedValue() < signal) {
			if (FAILED(gpu.fence->SetEventOnCompletion(signal, gpu.fenceEvent)))
				return nullptr;
			WaitForSingleObject(gpu.fenceEvent, 500);
		}
		outWidth = sbsW;
		outHeight = sbsH;
		outFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		static bool logged = false;
		if (!logged) {
			logged = true;
			logger::info("[VrBoosterTail] sealed eye {}x{} -> TD {}x{} center {}x{} -> {}x{}",
				engineW, engineH, displayW, displayH, subInW, subInH, subOutW, subOutH);
		}
		return displaySbs[buffer].Get();
#endif
	}
}
