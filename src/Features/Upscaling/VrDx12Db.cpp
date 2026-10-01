#include "PCH.h"
#include "VrDx12Db.h"

#include "VrPassTimer.h"

#include "NeuralRendering/AriseReconstructionGraph.h"
#include "NeuralRendering/Integration.h"
#include "NeuralRendering/Renderer.h"
#include "VrFfxOp/Operator.h"
#include "../DLSSBooster.h"
#include "../Upscaling.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

namespace
{
	using Microsoft::WRL::ComPtr;

	ComPtr<ID3D12RootSignature> root;
	ComPtr<ID3D12PipelineState> stretchPso;
	ComPtr<ID3D12PipelineState> blendPso;
	ComPtr<ID3D12DescriptorHeap> heap;
	UINT descriptorSize = 0;
	ComPtr<ID3D12Resource> present;
	UINT presentW = 0;
	UINT presentH = 0;
	ComPtr<ID3D12Resource> eyeColor[2];
	ComPtr<ID3D12Resource> eyeDepth[2];
	ComPtr<ID3D12Resource> eyeMotion[2];
	ComPtr<ID3D12Resource> stripScene;
	ComPtr<ID3D12Resource> stripDepth;
	ComPtr<ID3D12Resource> stripMotion;
	ComPtr<ID3D12Resource> stripDepthCopy;
	ComPtr<ID3D12Resource> stripMotionCopy;
	ComPtr<ID3D12Resource> stripDisplay;
	ComPtr<ID3D12DescriptorHeap> paintHeap;
	NeuralRendering::AriseReconstructionGraph graph;
	bool graphReady = false;
	DXGI_FORMAT graphFormat = DXGI_FORMAT_UNKNOWN;
	bool model3Ready = false;
	std::uint64_t planSignature = 0;
	bool havePlan = false;

	struct StretchCB
	{
		uint32_t data[8];
		uint32_t stretchMode;
		float blurRadius;
		uint32_t debugVisualize;
		uint32_t pad;
	};
	static_assert(sizeof(StretchCB) == 48);

	struct alignas(16) BlendCB
	{
		uint32_t dstOffsetX;
		uint32_t dstOffsetY;
		uint32_t subWidth;
		uint32_t subHeight;
		uint32_t blendMode;
		float featherWidth;
		uint32_t frameIndex;
		uint32_t srcOffsetX;
		float ditherStrength;
		uint32_t srcWidth;
		uint32_t srcHeight;
		uint32_t pad;
	};
	static_assert(sizeof(BlendCB) == 48);

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

	bool WaitFence(ID3D12Fence* fence, UINT64 value)
	{
		if (!fence || !value || fence->GetCompletedValue() >= value)
			return true;
		static HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!event)
			return false;
		ResetEvent(event);
		if (FAILED(fence->SetEventOnCompletion(value, event)))
			return false;
		return WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0;
	}

	bool CompileShader(const wchar_t* path, const char* name, ComPtr<ID3DBlob>& blob)
	{
		ComPtr<ID3DBlob> errors;
		if (FAILED(D3DCompileFromFile(path, nullptr, nullptr, "main", "cs_5_1",
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors))) {
			logger::error("[VrDx12Db] {} compile failed: {}", name,
				errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
			return false;
		}
		return true;
	}

	bool EnsureShaders(ID3D12Device* device)
	{
		if (stretchPso && blendPso && heap)
			return true;
		D3D12_DESCRIPTOR_RANGE ranges[2]{};
		ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
		ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 };
		D3D12_ROOT_PARAMETER params[2]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants = { 0, 0, 12 };
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable = { 2, ranges };
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_STATIC_SAMPLER_DESC sampler{};
		sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_ROOT_SIGNATURE_DESC desc{ 2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
		ComPtr<ID3DBlob> serialized;
		ComPtr<ID3DBlob> errors;
		if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
			FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
				IID_PPV_ARGS(&root))))
			return false;
		ComPtr<ID3DBlob> stretchBlob;
		ComPtr<ID3DBlob> blendBlob;
		if (!CompileShader(L"Data/Shaders/Upscaling/FoveatedRender/SubrectStretchCS.hlsl", "stretch", stretchBlob) ||
			!CompileShader(L"Data/Shaders/Upscaling/FoveatedRender/SubrectBlendCS.hlsl", "blend", blendBlob))
			return false;
		D3D12_COMPUTE_PIPELINE_STATE_DESC stretchDesc{};
		stretchDesc.pRootSignature = root.Get();
		stretchDesc.CS = { stretchBlob->GetBufferPointer(), stretchBlob->GetBufferSize() };
		D3D12_COMPUTE_PIPELINE_STATE_DESC blendDesc = stretchDesc;
		blendDesc.CS = { blendBlob->GetBufferPointer(), blendBlob->GetBufferSize() };
		if (FAILED(device->CreateComputePipelineState(&stretchDesc, IID_PPV_ARGS(&stretchPso))) ||
			FAILED(device->CreateComputePipelineState(&blendDesc, IID_PPV_ARGS(&blendPso))))
			return false;
		stretchPso->SetName(L"VrDx12Db::Stretch");
		blendPso->SetName(L"VrDx12Db::Blend");
		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16,
			D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
		if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap))))
			return false;
		heap->SetName(L"VrDx12Db::Descriptors");
		descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		return true;
	}

	bool EnsureTex(ID3D12Device* device, ComPtr<ID3D12Resource>& tex, UINT w, UINT h, DXGI_FORMAT format,
		bool uav, const wchar_t* name)
	{
		if (tex) {
			const auto desc = tex->GetDesc();
			if (desc.Width == w && desc.Height == h && desc.Format == format)
				return true;
		}
		tex.Reset();
		D3D12_HEAP_PROPERTIES heapProps{};
		heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = w;
		desc.Height = h;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		if (uav)
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex))))
			return false;
		tex->SetName(name);
		return true;
	}

	void CopyBox(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* dst,
		UINT x, UINT y, UINT w, UINT h)
	{
		D3D12_TEXTURE_COPY_LOCATION srcLoc{};
		srcLoc.pResource = src;
		srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_TEXTURE_COPY_LOCATION dstLoc = srcLoc;
		dstLoc.pResource = dst;
		const D3D12_BOX box{ x, y, 0, x + w, y + h, 1 };
		list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);
	}

	void CopySubrect(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* dst,
		UINT srcX, UINT srcY, UINT dstX, UINT dstY, UINT w, UINT h)
	{
		D3D12_TEXTURE_COPY_LOCATION srcLoc{};
		srcLoc.pResource = src;
		srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_TEXTURE_COPY_LOCATION dstLoc = srcLoc;
		dstLoc.pResource = dst;
		const D3D12_BOX box{ srcX, srcY, 0, srcX + w, srcY + h, 1 };
		list->CopyTextureRegion(&dstLoc, dstX, dstY, 0, &srcLoc, &box);
	}

	D3D12_GPU_DESCRIPTOR_HANDLE Bind(ID3D12Device* device, UINT slot, ID3D12Resource* srv, DXGI_FORMAT srvFormat,
		ID3D12Resource* uav, DXGI_FORMAT uavFormat)
	{
		auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = srvFormat;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(srv, &srvDesc, cpu);
		cpu.ptr += descriptorSize;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = uavFormat;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device->CreateUnorderedAccessView(uav, nullptr, &uavDesc, cpu);
		auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
		return gpu;
	}

	void DispatchConstants(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, const void* constants,
		D3D12_GPU_DESCRIPTOR_HANDLE table, UINT width, UINT height)
	{
		list->SetComputeRootSignature(root.Get());
		list->SetPipelineState(pso);
		list->SetComputeRoot32BitConstants(0, 12, constants, 0);
		list->SetComputeRootDescriptorTable(1, table);
		list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
	}

	NeuralRendering::Tuning TuningOf(const FoveatedRender::Settings& settings)
	{
		return {
			settings.neuralRenderingIntensity,
			settings.neuralRenderingLocalTone,
			settings.neuralRenderingLocalStructure,
			settings.neuralRenderingSkinStructure,
			settings.neuralRenderingStyle,
			settings.neuralRenderingAutoMask,
			settings.neuralRenderingUICorrection,
			0.0f,
			0.0f,
			false,
		};
	}

	NeuralRendering::Tuning Act3TuningOf(const FoveatedRender::Settings& settings)
	{
		return {
			settings.act3Intensity,
			settings.act3LocalTone,
			settings.act3LocalStructure,
			settings.act3SkinStructure,
			settings.act3Style,
			settings.act3AutoMask,
			settings.act3UICorrection,
			0.0f,
			0.0f,
			false,
		};
	}

	bool BuildPlan(const FoveatedRender::FlatPipelinePlan& flat, const FoveatedRender::Settings& settings,
		const sl::Constants& camera, const Util::Subrect::UVRegion& uv, uint32_t eye, uint32_t eyeW, uint32_t eyeH,
		uint32_t subInW, uint32_t subInH, bool reset, UINT64 frameId,
		NeuralRendering::AriseReconstructionGraph::Plan& plan)
	{
		using Graph = NeuralRendering::AriseReconstructionGraph;
		plan = {};
		plan.frameId = frameId ? frameId : 1;
		plan.sceneInputWidth = flat.engineWidth;
		plan.sceneInputHeight = flat.engineHeight;
		plan.engineWidth = flat.engineWidth;
		plan.engineHeight = flat.engineHeight;
		plan.displayWidth = flat.displayWidth;
		plan.displayHeight = flat.displayHeight;
		plan.act1OutputWidth = flat.act1OutputWidth;
		plan.act1OutputHeight = flat.act1OutputHeight;
		plan.nrWidth = flat.nrWidth;
		plan.nrHeight = flat.nrHeight;
		plan.act1 = flat.act1Backend == FoveatedRender::FlatAct1Backend::kDLSS ? Graph::Act1Backend::kDLSS :
																				   Graph::Act1Backend::kBypass;
		switch (flat.act2SrBackend) {
		case FoveatedRender::FlatAct2SrBackend::kDLSS: plan.act2Sr = Graph::Act2SrBackend::kDLSS; break;
		case FoveatedRender::FlatAct2SrBackend::kTent: plan.act2Sr = Graph::Act2SrBackend::kTent; break;
		case FoveatedRender::FlatAct2SrBackend::kMitchell: plan.act2Sr = Graph::Act2SrBackend::kMitchell; break;
		case FoveatedRender::FlatAct2SrBackend::kEasu: plan.act2Sr = Graph::Act2SrBackend::kEasu; break;
		case FoveatedRender::FlatAct2SrBackend::kNIS: plan.act2Sr = Graph::Act2SrBackend::kNIS; break;
		default: plan.act2Sr = Graph::Act2SrBackend::kLanczos; break;
		}
		plan.fusionEnabled = flat.fusion;
		plan.act1Downsample = static_cast<uint32_t>(flat.act1Downsample);
		plan.act1DepthDownsample = static_cast<uint32_t>(flat.act1DepthDownsample);
		plan.act1Quality = flat.act1Quality;
		plan.act1Preset = flat.act1Preset;
		plan.act2Quality = flat.act2Quality;
		plan.act2Preset = settings.flatPipelineAct2Preset;
		plan.act1Viewport = eye ? NeuralRendering::AriseDlssProvider::kAct1ViewportRight :
								  NeuralRendering::AriseDlssProvider::kAct1Viewport;
		plan.act2Viewport = eye ? NeuralRendering::AriseDlssProvider::kAct2ViewportRight :
								  NeuralRendering::AriseDlssProvider::kAct2Viewport;
		plan.nrSlot = eye;
		plan.act1Camera.value = camera;
		if (settings.flatPipelineAct1Jitter != 0)
			plan.act1Camera.value.jitterOffset = { 0.0f, 0.0f };
		plan.act1Camera.value.mvecScale = { uv.w > 0.0f ? 1.0f / uv.w : 1.0f, uv.h > 0.0f ? 1.0f / uv.h : 1.0f };
		plan.act2Camera.value = plan.act1Camera.value;
		if (settings.flatPipelineAct2Jitter == 0 && flat.engineWidth && flat.engineHeight) {
			plan.act2Camera.value.jitterOffset.x *= static_cast<float>(flat.nrWidth) / static_cast<float>(flat.engineWidth);
			plan.act2Camera.value.jitterOffset.y *= static_cast<float>(flat.nrHeight) / static_cast<float>(flat.engineHeight);
		} else
			plan.act2Camera.value.jitterOffset = { 0.0f, 0.0f };
		plan.fusion = {
			settings.neuralRenderingFusionLuma,
			settings.neuralRenderingFusionChroma,
			settings.neuralRenderingFusionEdgeProtection,
			settings.neuralRenderingFusionResidualLimit,
			settings.fusionCompareMode,
			settings.fusionDebugView,
			settings.fusionGainPrior,
			settings.flatPipelineFusionSoftness,
			settings.flatPipelineFusionFrequencyGuidance,
			settings.flatPipelineFusionDepthGuidance,
			settings.fusionDepthDilate,
			settings.fusionDetailGain,
			settings.fusionToneGainLuma,
			settings.fusionToneGainChroma,
			settings.fusionDither,
			0u,
			settings.fusionEdgeBlockGThresh,
			settings.fusionFitSideSigma,
			settings.fusionDirectionalSharpen,
			settings.fusionSoftEdge,
		};
		const auto act3 = FoveatedRender::MakeAct3Crop(flat.displayWidth, flat.displayHeight, settings.act3Scale);
		plan.act3 = {
			settings.act3Enabled,
			settings.act3Scale,
			act3.x,
			act3.y,
			act3.width,
			act3.height,
			Act3TuningOf(settings),
			settings.act3Sharpen,
			settings.act3Sharpness,
			settings.act3ResetEveryFrame,
			settings.act3BlendMode,
			settings.act3FeatherWidth,
			settings.act3DitherStrength,
			settings.act3GuidanceEnabled,
			settings.act3GuidanceLuma,
			settings.act3GuidanceChroma,
			settings.act3GuidancePrior,
			settings.act3GuidanceMaxGain,
			settings.act3GuidanceToneLuma,
			settings.act3GuidanceToneChroma,
			settings.act3GuidanceDetailGain,
			settings.act3GuidanceDither,
			settings.act3GuidanceDirSharpen,
		};
		plan.tuning = TuningOf(settings);
		plan.motionVectorScaleX = static_cast<float>(eyeW) * static_cast<float>(flat.nrWidth) /
								   static_cast<float>(std::max(1u, subInW));
		plan.motionVectorScaleY = static_cast<float>(eyeH) * static_cast<float>(flat.nrHeight) /
								   static_cast<float>(std::max(1u, subInH));
		plan.sharpness = settings.flatPipelineSharpness;
		plan.sharpen = settings.flatPipelineSharpen && flat.sharpen;
		plan.requested = settings.neuralRenderingEnabled;
		plan.eligible = flat.displayWidth && flat.displayHeight && flat.engineWidth && flat.engineHeight &&
						flat.nrWidth && flat.nrHeight;
		plan.colorBuffersHDR = false;
		plan.reset = reset;
		return plan.eligible;
	}

	uint32_t BlendMode(FoveatedRender::SubrectBlendMode mode, bool scale)
	{
		if (mode == FoveatedRender::SubrectBlendMode::kHardCopy)
			return 3u;
		if (mode == FoveatedRender::SubrectBlendMode::kInwardDither)
			return 2u;
		if (mode == FoveatedRender::SubrectBlendMode::kDither)
			return 1u;
		(void)scale;
		return 0u;
	}

	bool FillEdge(UINT srcWidth, UINT srcHeight, VrDx12Db::EdgePlan& out, FoveatedRender::FlatPipelinePlan& flat)
	{
		out = {};
		flat = {};
		out.srcWidth = srcWidth;
		out.srcHeight = srcHeight;
		if (srcWidth < 2 || !srcHeight)
			return false;
		auto& perf = globals::features::upscaling.perfMode;
		const bool logical = perf.IsLogicalEyeActive() && perf.GetDisplayEyeWidth() && perf.GetDisplayEyeHeight();
		out.eyeW = srcWidth / 2;
		out.eyeH = srcHeight;
		out.tdEyeW = logical ? perf.GetDisplayEyeWidth() : out.eyeW;
		out.tdEyeH = logical ? perf.GetDisplayEyeHeight() : out.eyeH;
		out.tdW = out.tdEyeW * 2;
		out.tdH = out.tdEyeH;
		if (!out.eyeW || !out.eyeH || !out.tdW || !out.tdH)
			return false;
		auto& upscaling = globals::features::upscaling;
		auto& foveated = upscaling.foveatedRender;
		const Util::Subrect::UVRegion* dbLeft = nullptr;
		const Util::Subrect::UVRegion* dbRight = nullptr;
		const bool dbCrop = globals::features::dlssBooster.TryGetVrSubrect(dbLeft, dbRight);
		const Util::Subrect::UVRegion left = dbCrop ? *dbLeft : foveated.subrectController.GetUV();
		const Util::Subrect::UVRegion right = dbCrop ? *dbRight : foveated.subrectController.GetRightEyeUV();
		const Util::Subrect::UVRegion uvs[2]{ left, right };
		for (uint32_t eye = 0; eye < 2; ++eye) {
			out.uvX[eye] = uvs[eye].x;
			out.uvY[eye] = uvs[eye].y;
			out.uvW[eye] = uvs[eye].w;
			out.uvH[eye] = uvs[eye].h;
		}
		if (left.w != right.w || left.h != right.h || !upscaling.IsVrDlssPostInjectActive())
			return true;
		auto atLeast = [](uint32_t size, uint32_t limit) {
			const uint32_t floor = std::min(limit, 64u);
			return std::min(limit, std::max(size, floor));
		};
		const uint32_t subInW0 = atLeast(std::max(1u, static_cast<uint32_t>(out.eyeW * left.w)), out.eyeW);
		const uint32_t subInH0 = atLeast(std::max(1u, static_cast<uint32_t>(out.eyeH * left.h)), out.eyeH);
		const uint32_t subOutW = atLeast(std::max(1u, static_cast<uint32_t>(out.tdEyeW * left.w)), out.tdEyeW);
		const uint32_t subOutH = atLeast(std::max(1u, static_cast<uint32_t>(out.tdEyeH * left.h)), out.tdEyeH);
		// Latched SRSSAA and quality. Reverse this box to G then R, ask NGX for
		// the minimum input, and move the box onto that size. A few pixels.
		const auto unified = foveated.ActiveUnifiedPipelineSettings();
		const bool srss = unified.act1Srss;
		const uint32_t quality = std::min(srss ? unified.act1SrssQuality :
			foveated.bootSnapshot.Boot(&FoveatedRender::Settings::flatPipelineAct1Quality), 4u);
		const float guidanceScale = srss ? std::max(unified.guidanceScale, 1.0f) : 1.0f;
		const float ratio = Upscaling::GetQualityModeRatio(quality);
		auto even = [](float value) {
			return std::max<uint32_t>(2u, static_cast<uint32_t>(std::lround(value)) & ~1u);
		};
		uint32_t subInW = subInW0;
		uint32_t subInH = subInH0;
		if (std::isfinite(ratio) && ratio > 0.0f) {
			const uint32_t guideW = even(static_cast<float>(subOutW) * guidanceScale);
			const uint32_t guideH = even(static_cast<float>(subOutH) * guidanceScale);
			uint32_t reversedW = even(static_cast<float>(guideW) / ratio);
			uint32_t reversedH = even(static_cast<float>(guideH) / ratio);
			uint32_t minW = 0, minH = 0, maxW = 0, maxH = 0;
			if (upscaling.streamline.QueryDLSSRenderRange(quality, guideW, guideH, minW, minH, maxW, maxH)) {
				if (reversedW < minW)
					reversedW = minW;
				if (reversedH < minH)
					reversedH = minH;
				if ((reversedW & 1u) && reversedW + 1u <= maxW)
					++reversedW;
				if ((reversedH & 1u) && reversedH + 1u <= maxH)
					++reversedH;
			}
			reversedW = std::min(reversedW, out.eyeW);
			reversedH = std::min(reversedH, out.eyeH);
			if (reversedW >= 2 && reversedH >= 2) {
				if (reversedW != subInW0 || reversedH != subInH0) {
					static uint32_t lastInW = 0, lastInH = 0, lastW = 0, lastH = 0;
					if (lastInW != subInW0 || lastInH != subInH0 || lastW != reversedW || lastH != reversedH) {
						logger::info("[VrDx12Db] box {}x{} -> {}x{} (G {}x{} NGX min {}x{} mode {})",
							subInW0, subInH0, reversedW, reversedH, guideW, guideH, minW, minH, quality);
						lastInW = subInW0;
						lastInH = subInH0;
						lastW = reversedW;
						lastH = reversedH;
					}
				}
				subInW = reversedW;
				subInH = reversedH;
			}
		}
		flat = foveated.BuildFlatPipelinePlan(subOutW, subOutH, subInW, subInH);
		if (flat.act1Backend == FoveatedRender::FlatAct1Backend::kFSR3)
			flat.act1Backend = FoveatedRender::FlatAct1Backend::kBypass;
		NeuralRendering::ReconcileVrAct1Plan(flat);
		if (!flat.engineWidth || !flat.engineHeight || flat.engineWidth > out.eyeW || flat.engineHeight > out.eyeH ||
			!flat.nrWidth || !flat.nrHeight || !flat.displayWidth || !flat.displayHeight ||
			!flat.act1OutputWidth || !flat.act1OutputHeight)
			return true;
		out.cropW = flat.engineWidth;
		out.cropH = flat.engineHeight;
		out.nrW = flat.nrWidth;
		out.nrH = flat.nrHeight;
		out.act1W = flat.act1OutputWidth;
		out.act1H = flat.act1OutputHeight;
		out.displayW = flat.displayWidth;
		out.displayH = flat.displayHeight;
		out.depthFilter = static_cast<UINT>(flat.act1DepthDownsample);
		out.subInW = subInW;
		out.subInH = subInH;
		for (uint32_t eye = 0; eye < 2; ++eye) {
			out.cropX[eye] = std::min(static_cast<UINT>(uvs[eye].x * out.eyeW), out.eyeW - out.cropW);
			out.cropY[eye] = std::min(static_cast<UINT>(uvs[eye].y * out.eyeH), out.eyeH - out.cropH);
		}
		// 5% of the box, toward the nose, and only while that eye still has pixels there.
		const UINT pad = std::max(2u, (static_cast<UINT>(std::lround(static_cast<float>(out.cropW) * 0.05f)) + 1u) & ~1u);
		const UINT leftRoom = (out.eyeW - (out.cropX[0] + out.cropW)) & ~1u;
		const UINT leftPad = std::min(pad, leftRoom);
		out.srcX[0] = out.cropX[0];
		out.srcW[0] = out.cropW + leftPad;
		const UINT rightRoom = out.cropX[1] & ~1u;
		const UINT rightPad = std::min(pad, rightRoom);
		out.srcX[1] = out.cropX[1] - rightPad;
		out.srcW[1] = out.cropW + rightPad;
		out.srReady = true;
		return true;
	}

	// One reconstruction of both padded eye boxes. 0 = not started, 1 = recorded, -1 = commands already written and failed.
	int RecordNasalStrip(const VrDx12Db::Inputs& in, const VrDx12Db::EdgePlan& edge,
		const FoveatedRender::Settings& settings, bool reset, VrDx12Db::Output& out)
	{
		if (!edge.srReady || !edge.cropW || !edge.cropH || !edge.displayW || !edge.displayH ||
			!edge.srcW[0] || !edge.srcW[1] || !in.constantsValid || !in.constants)
			return 0;
		const UINT guideW = edge.srcW[0] + edge.srcW[1];
		const UINT guideH = edge.cropH;
		const UINT leftOut = edge.srcW[0] * edge.displayW / edge.cropW;
		const UINT rightOut = edge.srcW[1] * edge.displayW / edge.cropW;
		const UINT rightPad = edge.srcW[1] > edge.cropW ? edge.srcW[1] - edge.cropW : 0;
		const UINT rightPadOut = rightPad * edge.displayW / edge.cropW;
		UINT boxW = edge.displayW;
		if (leftOut < boxW)
			boxW = leftOut;
		const UINT rightVis = rightOut > rightPadOut ? rightOut - rightPadOut : 0;
		if (rightVis < boxW)
			boxW = rightVis;
		const UINT colorW = leftOut + rightOut;
		if (guideW < 2 || colorW < 2 || !boxW || !edge.displayH)
			return 0;
		ID3D12Resource* sources[2]{};
		UINT srcX[2]{};
		UINT srcY[2]{};
		if (in.prepared) {
			for (uint32_t eye = 0; eye < 2; ++eye) {
				if (!in.eyeColor[eye] || !in.eyeDepth[eye] || !in.eyeMotion[eye])
					return 0;
				const auto desc = in.eyeColor[eye]->GetDesc();
				if (desc.Width != edge.srcW[eye] || desc.Height != edge.cropH)
					return 0;
				sources[eye] = in.eyeColor[eye];
			}
		} else {
			if (!in.color || !in.depth || !in.motion)
				return 0;
			sources[0] = in.color;
			sources[1] = in.color;
			srcY[0] = edge.cropY[0];
			srcY[1] = edge.cropY[1];
			srcX[0] = edge.srcX[0];
			srcX[1] = edge.eyeW + edge.srcX[1];
		}
		const DXGI_FORMAT colorFormat = sources[0]->GetDesc().Format;
		const DXGI_FORMAT depthFormat = (in.prepared ? in.eyeDepth[0] : in.depth)->GetDesc().Format;
		const DXGI_FORMAT motionFormat = (in.prepared ? in.eyeMotion[0] : in.motion)->GetDesc().Format;
		if (!EnsureTex(in.device, stripScene, guideW, guideH, colorFormat, false, L"VrDx12Db::StripScene") ||
			!EnsureTex(in.device, stripDepth, guideW, guideH, depthFormat, false, L"VrDx12Db::StripDepth") ||
			!EnsureTex(in.device, stripMotion, guideW, guideH, motionFormat, false, L"VrDx12Db::StripMotion") ||
			!EnsureTex(in.device, stripDepthCopy, guideW, guideH, depthFormat, false, L"VrDx12Db::StripDepthCopy") ||
			!EnsureTex(in.device, stripMotionCopy, guideW, guideH, motionFormat, false, L"VrDx12Db::StripMotionCopy") ||
			!EnsureTex(in.device, stripDisplay, colorW, edge.displayH, DXGI_FORMAT_R8G8B8A8_UNORM, true, L"VrDx12Db::StripDisplay"))
			return 0;

		auto& foveated = globals::features::upscaling.foveatedRender;
		auto flat = foveated.BuildFlatPipelinePlan(colorW, edge.displayH, guideW, guideH);
		NeuralRendering::ReconcileVrAct1Plan(flat);
		if (!flat.engineWidth || !flat.displayWidth || !flat.nrWidth || !flat.act1OutputWidth)
			return 0;
		NeuralRendering::AriseReconstructionGraph::Plan plan{};
		const Util::Subrect::UVRegion uv{ edge.uvX[0], edge.uvY[0], edge.uvW[0], edge.uvH[0] };
		if (!BuildPlan(flat, settings, in.constants[0], uv, 0, edge.eyeW, edge.eyeH, guideW, guideH,
				reset, in.frameId, plan))
			return 0;
		// Stored motion of 1 is one full eye. The strip is wider than one box, so the scale is eye/strip.
		plan.act1Camera.value.mvecScale = {
			static_cast<float>(edge.eyeW) / static_cast<float>(guideW),
			static_cast<float>(edge.eyeH) / static_cast<float>(guideH)
		};
		plan.act1Camera.value.cameraAspectRatio = static_cast<float>(guideW) / static_cast<float>(guideH);
		plan.act2Camera.value.mvecScale = plan.act1Camera.value.mvecScale;
		plan.act2Camera.value.cameraAspectRatio = plan.act1Camera.value.cameraAspectRatio;
		plan.motionVectorScaleX = static_cast<float>(edge.eyeW) * static_cast<float>(plan.nrWidth) / static_cast<float>(guideW);
		plan.motionVectorScaleY = static_cast<float>(edge.eyeH) * static_cast<float>(plan.nrHeight) / static_cast<float>(guideH);
		if (plan.act3.enabled) {
			plan.act3.cropX = 0;
			plan.act3.cropY = 0;
			plan.act3.cropWidth = plan.displayWidth & ~1u;
			plan.act3.cropHeight = plan.displayHeight & ~1u;
		}
		ID3D12Resource* depthSrc[2]{ in.prepared ? in.eyeDepth[0] : in.depth, in.prepared ? in.eyeDepth[1] : in.depth };
		ID3D12Resource* motionSrc[2]{ in.prepared ? in.eyeMotion[0] : in.motion, in.prepared ? in.eyeMotion[1] : in.motion };
		auto toSource = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES* state) {
			const auto before = state ? *state : D3D12_RESOURCE_STATE_COMMON;
			if (before == D3D12_RESOURCE_STATE_COPY_SOURCE)
				return;
			Transition(in.list, resource, before, D3D12_RESOURCE_STATE_COPY_SOURCE);
			if (state)
				*state = D3D12_RESOURCE_STATE_COPY_SOURCE;
		};
		auto toCommon = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES* state) {
			if (state && *state == D3D12_RESOURCE_STATE_COMMON)
				return;
			Transition(in.list, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			if (state)
				*state = D3D12_RESOURCE_STATE_COMMON;
		};
		if (in.prepared) {
			for (uint32_t eye = 0; eye < 2; ++eye) {
				toSource(sources[eye], nullptr);
				toSource(depthSrc[eye], nullptr);
				toSource(motionSrc[eye], nullptr);
			}
		} else {
			toSource(in.color, in.colorState);
			toSource(in.depth, in.depthState);
			toSource(in.motion, in.motionState);
		}
		Transition(in.list, stripScene.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		Transition(in.list, stripDepth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		Transition(in.list, stripMotion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		UINT dstX = 0;
		for (uint32_t eye = 0; eye < 2; ++eye) {
			const UINT eyeSrcX = in.prepared ? 0u : srcX[eye];
			const UINT eyeSrcY = in.prepared ? 0u : srcY[eye];
			CopySubrect(in.list, sources[eye], stripScene.Get(), eyeSrcX, eyeSrcY, dstX, 0, edge.srcW[eye], guideH);
			CopySubrect(in.list, depthSrc[eye], stripDepth.Get(), eyeSrcX, eyeSrcY, dstX, 0, edge.srcW[eye], guideH);
			CopySubrect(in.list, motionSrc[eye], stripMotion.Get(), eyeSrcX, eyeSrcY, dstX, 0, edge.srcW[eye], guideH);
			dstX += edge.srcW[eye];
		}
		Transition(in.list, stripDepth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Transition(in.list, stripMotion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Transition(in.list, stripDepthCopy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		Transition(in.list, stripMotionCopy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		in.list->CopyResource(stripDepthCopy.Get(), stripDepth.Get());
		in.list->CopyResource(stripMotionCopy.Get(), stripMotion.Get());
		Transition(in.list, stripScene.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		Transition(in.list, stripDepth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
		Transition(in.list, stripMotion.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
		Transition(in.list, stripDepthCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		Transition(in.list, stripMotionCopy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		if (in.prepared) {
			for (uint32_t eye = 0; eye < 2; ++eye) {
				toCommon(sources[eye], nullptr);
				toCommon(depthSrc[eye], nullptr);
				toCommon(motionSrc[eye], nullptr);
			}
		} else {
			toCommon(in.color, in.colorState);
			toCommon(in.depth, in.depthState);
			toCommon(in.motion, in.motionState);
		}
		static bool releasedRight = false;
		if (!releasedRight) {
			graph.ReleaseSecondaryViewports();
			releasedRight = true;
		}
		NeuralRendering::AriseReconstructionGraph::Inputs graphIn{
			.scene = stripScene.Get(),
			.depth = stripDepth.Get(),
			.motionVectors = stripMotion.Get(),
		};
		if (!graph.EnsureSlotResources(0, graphIn, plan))
			return -1;
		const auto result = graph.Record(in.list, 0, graphIn, plan);
		if (!result.finalResource || result.finalResource == stripScene.Get())
			return -1;
		ID3D12DescriptorHeap* heaps[]{ heap.Get() };
		in.list->SetDescriptorHeaps(1, heaps);
		Transition(in.list, result.finalResource, result.state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		Transition(in.list, stripDisplay.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		// Slot 0 is the stretch that runs later on this same list. Descriptors are read at execute.
		const auto table = Bind(in.device, 8, result.finalResource, result.finalResource->GetDesc().Format,
			stripDisplay.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
		BlendCB convert{};
		convert.subWidth = colorW;
		convert.subHeight = edge.displayH;
		convert.blendMode = 3u;
		convert.srcWidth = colorW;
		convert.srcHeight = edge.displayH;
		DispatchConstants(in.list, blendPso.Get(), &convert, table, colorW, edge.displayH);
		Transition(in.list, result.finalResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, result.state);
		Transition(in.list, stripDisplay.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		out.stripColor = stripDisplay.Get();
		out.stripDepth = stripDepthCopy.Get();
		out.stripMotion = stripMotionCopy.Get();
		out.strip.active = true;
		out.strip.colorW = colorW;
		out.strip.colorH = edge.displayH;
		out.strip.guideW = guideW;
		out.strip.guideH = guideH;
		out.strip.boxW = boxW;
		out.strip.boxH = edge.displayH;
		out.strip.srcOffsetX[0] = 0;
		out.strip.srcOffsetX[1] = leftOut + rightPadOut;
		out.strip.dstX[0] = static_cast<UINT>(edge.uvX[0] * edge.tdEyeW);
		out.strip.dstY[0] = static_cast<UINT>(edge.uvY[0] * edge.tdEyeH);
		out.strip.dstX[1] = edge.tdEyeW + static_cast<UINT>(edge.uvX[1] * edge.tdEyeW);
		out.strip.dstY[1] = static_cast<UINT>(edge.uvY[1] * edge.tdEyeH);
		if (out.strip.dstX[0] + boxW > edge.tdEyeW)
			out.strip.dstX[0] = edge.tdEyeW - boxW;
		if (out.strip.dstX[1] + boxW > edge.tdW)
			out.strip.dstX[1] = edge.tdW - boxW;
		if (out.strip.dstY[0] + edge.displayH > edge.tdH)
			out.strip.dstY[0] = edge.tdH - edge.displayH;
		if (out.strip.dstY[1] + edge.displayH > edge.tdH)
			out.strip.dstY[1] = edge.tdH - edge.displayH;
		out.strip.mvecScaleX = static_cast<float>(edge.eyeW);
		out.strip.mvecScaleY = static_cast<float>(edge.eyeH);
		static UINT loggedW = 0;
		static UINT loggedH = 0;
		if (loggedW != colorW || loggedH != guideH) {
			loggedW = colorW;
			loggedH = guideH;
			VrFfxOp::NoteDiscontinuity();
			logger::info("[VrDx12Db] nasal strip color {}x{} guide {}x{} box {}x{} pad {}+{} mvec {:.0f}x{:.0f}",
				colorW, edge.displayH, guideW, guideH, boxW, edge.displayH,
				edge.srcW[0] - edge.cropW, edge.srcW[1] - edge.cropW,
				out.strip.mvecScaleX, out.strip.mvecScaleY);
		}
		return 1;
	}
}

namespace VrDx12Db
{
	bool ComputeEdgePlan(UINT srcWidth, UINT srcHeight, EdgePlan& out)
	{
		FoveatedRender::FlatPipelinePlan unused{};
		return FillEdge(srcWidth, srcHeight, out, unused);
	}

	bool Record(const Inputs& in, Output& out)
	{
		if (!in.list || !in.device || !in.queue || in.srcWidth < 2 || !in.srcHeight)
			return false;
		EdgePlan edge{};
		FoveatedRender::FlatPipelinePlan flat{};
		if (!FillEdge(in.srcWidth, in.srcHeight, edge, flat) || !edge.tdW || !edge.tdH || !EnsureShaders(in.device))
			return false;
		const bool preStretched = in.prepared && in.backdrop && in.backdropState &&
			in.backdrop->GetDesc().Width == edge.tdW && in.backdrop->GetDesc().Height == edge.tdH;
		if (in.prepared && !preStretched)
			return false;
		if (!preStretched && (!in.color || !in.colorState))
			return false;
		D3D12_RESOURCE_STATES ownedPresentState = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES* presentState = &ownedPresentState;
		ID3D12Resource* presentRes = nullptr;
		if (preStretched) {
			presentRes = in.backdrop;
			presentState = in.backdropState;
		} else if (!EnsureTex(in.device, present, edge.tdW, edge.tdH, DXGI_FORMAT_R8G8B8A8_UNORM, true, L"VrDx12Db::Present")) {
			return false;
		} else {
			presentRes = present.Get();
		}
		// First command on this list. Closes VrDb::SubmitGap, which is the GPU
		// idle while this list was recorded after the input copy.
		VrPassTimer::Mark(in.list, "VrDb::Crop");
		const UINT eyeW = edge.eyeW;
		const UINT eyeH = edge.eyeH;
		const UINT tdEyeW = edge.tdEyeW;
		const UINT tdEyeH = edge.tdEyeH;
		const UINT tdW = edge.tdW;
		const UINT tdH = edge.tdH;

		auto& upscaling = globals::features::upscaling;
		auto& foveated = upscaling.foveatedRender;
		bool eyesMatch = in.prepared;
		if (in.prepared) {
			for (uint32_t eye = 0; eye < 2; ++eye) {
				if (!in.eyeColor[eye] || !in.eyeDepth[eye] || !in.eyeMotion[eye]) {
					eyesMatch = false;
					break;
				}
				const auto colorDesc = in.eyeColor[eye]->GetDesc();
				const auto depthDesc = in.eyeDepth[eye]->GetDesc();
				const auto motionDesc = in.eyeMotion[eye]->GetDesc();
				const UINT expectW = edge.srcW[eye] ? edge.srcW[eye] : edge.cropW;
				if (colorDesc.Width != expectW || colorDesc.Height != edge.cropH ||
					depthDesc.Width != expectW || depthDesc.Height != edge.cropH ||
					motionDesc.Width != expectW || motionDesc.Height != edge.cropH) {
					eyesMatch = false;
					break;
				}
			}
		}
		const bool runSr = edge.srReady && !in.colorOnly && in.guides && in.depth && in.motion &&
						   in.depthState && in.motionState && in.constantsValid && in.constants &&
						   in.depth->GetDesc().Width == in.srcWidth && in.motion->GetDesc().Width == in.srcWidth &&
						   (!in.prepared || eyesMatch);

		ID3D12Resource* eyeFinal[2]{};
		D3D12_RESOURCE_STATES eyeState[2]{ D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
		UINT boxX[2]{};
		UINT boxY[2]{};
		UINT boxW[2]{};
		UINT boxH[2]{};
		bool eyeOk[2]{};
		if (runSr) {
			if (!model3Ready) {
				model3Ready = NeuralRendering::Renderer::Instance().InitializeModel3(in.device, [&] {
					return WaitFence(in.fence, in.fenceValue);
				});
				if (!model3Ready) {
					static bool logged = false;
					if (!logged) {
						logged = true;
						logger::error("[VrDx12Db] Feature 18 did not bind to the shared device");
					}
				}
			}
			const DXGI_FORMAT colorFormat = (in.prepared && in.eyeColor[0]) ?
				in.eyeColor[0]->GetDesc().Format : in.color->GetDesc().Format;
			if (model3Ready && (!graphReady || graphFormat != colorFormat)) {
				graphReady = graph.Initialize(in.device, colorFormat, 2, &upscaling.streamlineDX12);
				graphFormat = colorFormat;
			}
			std::uint64_t signature = flat.engineWidth;
			signature = (signature << 16) ^ flat.engineHeight ^ (static_cast<std::uint64_t>(flat.nrWidth) << 32) ^
						flat.act1OutputWidth ^ (static_cast<std::uint64_t>(flat.displayWidth) << 8);
			const bool planChanged = !havePlan || signature != planSignature;
			havePlan = true;
			planSignature = signature;
			const bool reset = planChanged || in.constants[0].reset == sl::Boolean::eTrue ||
							   in.constants[1].reset == sl::Boolean::eTrue;
			if (planChanged)
				VrFfxOp::NoteDiscontinuity();
			out.strip = {};
			out.stripColor = nullptr;
			out.stripDepth = nullptr;
			out.stripMotion = nullptr;
			const int stripResult = graphReady ? RecordNasalStrip(in, edge, foveated.settings, reset, out) : 0;
			if (stripResult < 0)
				return false;
			if (!stripResult && graphReady && flat.engineWidth && flat.engineHeight && flat.engineWidth <= eyeW && flat.engineHeight <= eyeH) {
				const DXGI_FORMAT depthFormat = in.prepared ? in.eyeDepth[0]->GetDesc().Format : in.depth->GetDesc().Format;
				const DXGI_FORMAT motionFormat = in.prepared ? in.eyeMotion[0]->GetDesc().Format : in.motion->GetDesc().Format;
				NeuralRendering::AriseReconstructionGraph::Plan plans[2]{};
				ID3D12Resource* eyeScene[2]{};
				ID3D12Resource* eyeDepthSrc[2]{};
				ID3D12Resource* eyeMotionSrc[2]{};
				bool eyesReady = true;
				for (uint32_t eye = 0; eye < 2; ++eye) {
					const Util::Subrect::UVRegion uv{ edge.uvX[eye], edge.uvY[eye], edge.uvW[eye], edge.uvH[eye] };
					if (!BuildPlan(flat, foveated.settings, in.constants[eye], uv, eye, eyeW, eyeH, edge.subInW, edge.subInH,
							reset, in.frameId, plans[eye]))
						eyesReady = false;
					if (!in.prepared) {
						if (!EnsureTex(in.device, eyeColor[eye], plans[eye].engineWidth, plans[eye].engineHeight, colorFormat,
								false, eye ? L"VrDx12Db::EyeColor1" : L"VrDx12Db::EyeColor0") ||
							!EnsureTex(in.device, eyeDepth[eye], plans[eye].engineWidth, plans[eye].engineHeight, depthFormat,
								false, eye ? L"VrDx12Db::EyeDepth1" : L"VrDx12Db::EyeDepth0") ||
							!EnsureTex(in.device, eyeMotion[eye], plans[eye].engineWidth, plans[eye].engineHeight, motionFormat,
								false, eye ? L"VrDx12Db::EyeMotion1" : L"VrDx12Db::EyeMotion0"))
							eyesReady = false;
						eyeScene[eye] = eyeColor[eye].Get();
						eyeDepthSrc[eye] = eyeDepth[eye].Get();
						eyeMotionSrc[eye] = eyeMotion[eye].Get();
					} else {
						eyeScene[eye] = in.eyeColor[eye];
						eyeDepthSrc[eye] = in.eyeDepth[eye];
						eyeMotionSrc[eye] = in.eyeMotion[eye];
					}
				}
				if (eyesReady && !in.prepared) {
					Transition(in.list, in.color, *in.colorState, D3D12_RESOURCE_STATE_COPY_SOURCE);
					*in.colorState = D3D12_RESOURCE_STATE_COPY_SOURCE;
					Transition(in.list, in.depth, *in.depthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
					*in.depthState = D3D12_RESOURCE_STATE_COPY_SOURCE;
					Transition(in.list, in.motion, *in.motionState, D3D12_RESOURCE_STATE_COPY_SOURCE);
					*in.motionState = D3D12_RESOURCE_STATE_COPY_SOURCE;
					for (uint32_t eye = 0; eye < 2; ++eye) {
						const UINT copyW = plans[eye].engineWidth;
						const UINT copyH = plans[eye].engineHeight;
						const UINT sbsX = (eye ? eyeW : 0) + edge.cropX[eye];
						Transition(in.list, eyeScene[eye], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
						Transition(in.list, eyeDepthSrc[eye], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
						Transition(in.list, eyeMotionSrc[eye], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
						CopyBox(in.list, in.color, eyeScene[eye], sbsX, edge.cropY[eye], copyW, copyH);
						CopyBox(in.list, in.depth, eyeDepthSrc[eye], sbsX, edge.cropY[eye], copyW, copyH);
						CopyBox(in.list, in.motion, eyeMotionSrc[eye], sbsX, edge.cropY[eye], copyW, copyH);
						Transition(in.list, eyeScene[eye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
						Transition(in.list, eyeDepthSrc[eye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
						Transition(in.list, eyeMotionSrc[eye], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
					}
					Transition(in.list, in.color, *in.colorState, D3D12_RESOURCE_STATE_COMMON);
					*in.colorState = D3D12_RESOURCE_STATE_COMMON;
					Transition(in.list, in.depth, *in.depthState, D3D12_RESOURCE_STATE_COMMON);
					*in.depthState = D3D12_RESOURCE_STATE_COMMON;
					Transition(in.list, in.motion, *in.motionState, D3D12_RESOURCE_STATE_COMMON);
					*in.motionState = D3D12_RESOURCE_STATE_COMMON;
				}
				if (eyesReady) {
					for (uint32_t eye = 0; eye < 2; ++eye) {
						boxX[eye] = static_cast<UINT>(edge.uvX[eye] * tdEyeW);
						boxY[eye] = static_cast<UINT>(edge.uvY[eye] * tdEyeH);
						boxW[eye] = plans[eye].displayWidth;
						boxH[eye] = plans[eye].displayHeight;
						if (boxW[eye] < tdEyeW && boxX[eye] + boxW[eye] > tdEyeW)
							boxX[eye] = tdEyeW - boxW[eye];
						if (boxH[eye] < tdEyeH && boxY[eye] + boxH[eye] > tdEyeH)
							boxY[eye] = tdEyeH - boxH[eye];
					}
					for (uint32_t eye = 0; eye < 2; ++eye) {
						NeuralRendering::AriseReconstructionGraph::Inputs graphIn{
							.scene = eyeScene[eye],
							.depth = eyeDepthSrc[eye],
							.motionVectors = eyeMotionSrc[eye],
							.depthN = in.eyeDepthN[eye],
							.motionN = in.eyeMotionN[eye],
						};
						if (!graph.EnsureSlotResources(eye, graphIn, plans[eye]))
							continue;
					}
					for (uint32_t eye = 0; eye < 2; ++eye) {
						NeuralRendering::AriseReconstructionGraph::Inputs graphIn{
							.scene = eyeScene[eye],
							.depth = eyeDepthSrc[eye],
							.motionVectors = eyeMotionSrc[eye],
							.depthN = in.eyeDepthN[eye],
							.motionN = in.eyeMotionN[eye],
						};
						const auto result = graph.Record(in.list, eye, graphIn, plans[eye]);
						if (!result.finalResource || result.finalResource == eyeScene[eye])
							continue;
						eyeFinal[eye] = result.finalResource;
						eyeState[eye] = result.state;
						eyeOk[eye] = true;
					}
					static bool logged = false;
					if (!logged && (eyeOk[0] || eyeOk[1])) {
						logged = true;
						logger::info("[VrDx12Db] DX12 DLSS SR {}x{} -> center {}x{} TD {}x{} nr={} prep={}",
							flat.engineWidth, flat.engineHeight, flat.displayWidth, flat.displayHeight, tdW, tdH,
							foveated.settings.neuralRenderingEnabled ? 1 : 0, in.prepared ? 1 : 0);
					}
				}
			}
		}

		ID3D12DescriptorHeap* heaps[]{ heap.Get() };
		in.list->SetDescriptorHeaps(1, heaps);
		bool presentIsUav = false;
		if (!preStretched) {
			Transition(in.list, in.color, *in.colorState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			*in.colorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			Transition(in.list, presentRes, *presentState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			*presentState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			presentIsUav = true;
			VrPassTimer::Mark(in.list, "VrDb::Stretch");
			const auto stretchTable = Bind(in.device, 0, in.color, in.color->GetDesc().Format, presentRes,
				DXGI_FORMAT_R8G8B8A8_UNORM);
			for (uint32_t eye = 0; eye < 2; ++eye) {
				StretchCB cb{};
				cb.data[0] = eye ? tdEyeW : 0;
				cb.data[1] = tdEyeW;
				cb.data[2] = tdEyeH;
				cb.data[3] = eye ? eyeW : 0;
				cb.data[4] = in.srcWidth;
				cb.data[5] = in.srcHeight;
				cb.data[6] = eyeW;
				cb.data[7] = eyeH;
				cb.stretchMode = 2u;
				cb.blurRadius = 1.0f;
				DispatchConstants(in.list, stretchPso.Get(), &cb, stretchTable, tdEyeW, tdEyeH);
			}
			Transition(in.list, in.color, *in.colorState, D3D12_RESOURCE_STATE_COMMON);
			*in.colorState = D3D12_RESOURCE_STATE_COMMON;
		} else {
			VrPassTimer::Mark(in.list, "VrDb::Stretch");
		}

		VrPassTimer::Mark(in.list, "VrDb::Blend");
		const auto blendMode = foveated.GetSubrectBlendMode();
		for (uint32_t eye = 0; eye < 2; ++eye) {
			if (!eyeOk[eye] || !eyeFinal[eye] || !boxW[eye] || !boxH[eye])
				continue;
			if (!presentIsUav) {
				Transition(in.list, presentRes, *presentState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				*presentState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
				presentIsUav = true;
			}
			const auto srcDesc = eyeFinal[eye]->GetDesc();
			Transition(in.list, eyeFinal[eye], eyeState[eye], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			const auto table = Bind(in.device, 2 + eye * 2, eyeFinal[eye], srcDesc.Format, presentRes,
				DXGI_FORMAT_R8G8B8A8_UNORM);
			BlendCB cb{};
			cb.dstOffsetX = (eye ? tdEyeW : 0) + boxX[eye];
			cb.dstOffsetY = boxY[eye];
			cb.subWidth = boxW[eye];
			cb.subHeight = boxH[eye];
			cb.blendMode = BlendMode(blendMode, srcDesc.Width != boxW[eye] || srcDesc.Height != boxH[eye]);
			cb.featherWidth = foveated.settings.subrectFeatherWidth;
			cb.frameIndex = static_cast<uint32_t>(in.frameId);
			cb.ditherStrength = foveated.settings.subrectDitherStrength;
			cb.srcWidth = static_cast<uint32_t>(srcDesc.Width);
			cb.srcHeight = srcDesc.Height;
			DispatchConstants(in.list, blendPso.Get(), &cb, table, boxW[eye], boxH[eye]);
			Transition(in.list, eyeFinal[eye], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, eyeState[eye]);
		}
		if (presentIsUav) {
			Transition(in.list, presentRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			*presentState = D3D12_RESOURCE_STATE_COMMON;
		}
		presentW = tdW;
		presentH = tdH;
		out.present = presentRes;
		out.presentState = D3D12_RESOURCE_STATE_COMMON;
		out.width = tdW;
		out.height = tdH;
		VrPassTimer::Finish(in.list);
		return true;
	}

	bool PaintCenters(ID3D12GraphicsCommandList* list, ID3D12Device* device,
		ID3D12Resource* source, ID3D12Resource* dest, const StripView& strip, uint32_t frameId,
		UINT tableSlot)
	{
		if (!list || !device || !source || !dest || !strip.active || !strip.boxW || !strip.boxH)
			return false;
		if (!EnsureShaders(device))
			return false;
		if (!paintHeap) {
			D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
				D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
			if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&paintHeap))))
				return false;
			paintHeap->SetName(L"VrDx12Db::Paint");
		}
		auto cpu = paintHeap->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += static_cast<SIZE_T>(tableSlot) * descriptorSize;
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = source->GetDesc().Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(source, &srvDesc, cpu);
		cpu.ptr += descriptorSize;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = dest->GetDesc().Format;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device->CreateUnorderedAccessView(dest, nullptr, &uavDesc, cpu);
		auto gpu = paintHeap->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += static_cast<SIZE_T>(tableSlot) * descriptorSize;
		ID3D12DescriptorHeap* heaps[]{ paintHeap.Get() };
		list->SetDescriptorHeaps(1, heaps);
		Transition(list, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		auto& foveated = globals::features::upscaling.foveatedRender;
		for (uint32_t eye = 0; eye < 2; ++eye) {
			BlendCB cb{};
			cb.dstOffsetX = strip.dstX[eye];
			cb.dstOffsetY = strip.dstY[eye];
			cb.subWidth = strip.boxW;
			cb.subHeight = strip.boxH;
			cb.blendMode = BlendMode(foveated.GetSubrectBlendMode(), false);
			cb.featherWidth = foveated.settings.subrectFeatherWidth;
			cb.frameIndex = frameId;
			cb.srcOffsetX = strip.srcOffsetX[eye];
			cb.ditherStrength = foveated.settings.subrectDitherStrength;
			cb.srcWidth = strip.boxW;
			cb.srcHeight = strip.boxH;
			DispatchConstants(list, blendPso.Get(), &cb, gpu, strip.boxW, strip.boxH);
		}
		Transition(list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
		return true;
	}
}
