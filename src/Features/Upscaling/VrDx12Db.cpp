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
}

namespace VrDx12Db
{
	bool Record(const Inputs& in, Output& out)
	{
		if (!in.list || !in.device || !in.queue || !in.color || !in.colorState || in.srcWidth < 2 || !in.srcHeight)
			return false;
		// First command on this list. Closes VrDb::SubmitGap, which is the GPU
		// idle while this list was recorded after the input copy.
		VrPassTimer::Mark(in.list, "VrDb::Crop");
		auto& perf = globals::features::upscaling.perfMode;
		const bool logical = perf.IsLogicalEyeActive() && perf.GetDisplayEyeWidth() && perf.GetDisplayEyeHeight();
		const UINT eyeW = in.srcWidth / 2;
		const UINT eyeH = in.srcHeight;
		const UINT tdEyeW = logical ? perf.GetDisplayEyeWidth() : eyeW;
		const UINT tdEyeH = logical ? perf.GetDisplayEyeHeight() : eyeH;
		const UINT tdW = tdEyeW * 2;
		const UINT tdH = tdEyeH;
		if (!tdW || !tdH || !EnsureShaders(in.device) ||
			!EnsureTex(in.device, present, tdW, tdH, DXGI_FORMAT_R8G8B8A8_UNORM, true, L"VrDx12Db::Present"))
			return false;

		auto& upscaling = globals::features::upscaling;
		auto& foveated = upscaling.foveatedRender;
		const Util::Subrect::UVRegion* dbLeft = nullptr;
		const Util::Subrect::UVRegion* dbRight = nullptr;
		const bool dbCrop = globals::features::dlssBooster.TryGetVrSubrect(dbLeft, dbRight);
		const Util::Subrect::UVRegion left = dbCrop ? *dbLeft : foveated.subrectController.GetUV();
		const Util::Subrect::UVRegion right = dbCrop ? *dbRight : foveated.subrectController.GetRightEyeUV();
		const bool runSr = !in.colorOnly && in.guides && in.depth && in.motion && in.depthState && in.motionState &&
						   in.constantsValid && in.constants && upscaling.IsVrDlssPostInjectActive() &&
						   left.w == right.w && left.h == right.h &&
						   in.depth->GetDesc().Width == in.srcWidth && in.motion->GetDesc().Width == in.srcWidth;

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
			const DXGI_FORMAT colorFormat = in.color->GetDesc().Format;
			if (model3Ready && (!graphReady || graphFormat != colorFormat)) {
				graphReady = graph.Initialize(in.device, colorFormat, 2, &upscaling.streamlineDX12);
				graphFormat = colorFormat;
			}
			auto atLeast = [](uint32_t size, uint32_t limit) {
				const uint32_t floor = std::min(limit, 64u);
				return std::min(limit, std::max(size, floor));
			};
			const Util::Subrect::UVRegion uvs[2]{ left, right };
			const uint32_t subInW = atLeast(std::max(1u, static_cast<uint32_t>(eyeW * left.w)), eyeW);
			const uint32_t subInH = atLeast(std::max(1u, static_cast<uint32_t>(eyeH * left.h)), eyeH);
			const uint32_t subOutW = atLeast(std::max(1u, static_cast<uint32_t>(tdEyeW * left.w)), tdEyeW);
			const uint32_t subOutH = atLeast(std::max(1u, static_cast<uint32_t>(tdEyeH * left.h)), tdEyeH);
			auto flat = foveated.BuildFlatPipelinePlan(subOutW, subOutH, subInW, subInH);
			if (flat.act1Backend == FoveatedRender::FlatAct1Backend::kFSR3)
				flat.act1Backend = FoveatedRender::FlatAct1Backend::kBypass;
			NeuralRendering::ReconcileVrAct1Plan(flat);
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
			if (graphReady && flat.engineWidth && flat.engineHeight && flat.engineWidth <= eyeW && flat.engineHeight <= eyeH) {
				const DXGI_FORMAT depthFormat = in.depth->GetDesc().Format;
				const DXGI_FORMAT motionFormat = in.motion->GetDesc().Format;
				NeuralRendering::AriseReconstructionGraph::Plan plans[2]{};
				bool prepared = true;
				for (uint32_t eye = 0; eye < 2; ++eye) {
					if (!BuildPlan(flat, foveated.settings, in.constants[eye], uvs[eye], eye, eyeW, eyeH, subInW, subInH,
							reset, in.frameId, plans[eye]) ||
						!EnsureTex(in.device, eyeColor[eye], plans[eye].engineWidth, plans[eye].engineHeight, colorFormat,
							false, eye ? L"VrDx12Db::EyeColor1" : L"VrDx12Db::EyeColor0") ||
						!EnsureTex(in.device, eyeDepth[eye], plans[eye].engineWidth, plans[eye].engineHeight, depthFormat,
							false, eye ? L"VrDx12Db::EyeDepth1" : L"VrDx12Db::EyeDepth0") ||
						!EnsureTex(in.device, eyeMotion[eye], plans[eye].engineWidth, plans[eye].engineHeight, motionFormat,
							false, eye ? L"VrDx12Db::EyeMotion1" : L"VrDx12Db::EyeMotion0"))
						prepared = false;
				}
				if (prepared) {
					Transition(in.list, in.color, *in.colorState, D3D12_RESOURCE_STATE_COPY_SOURCE);
					*in.colorState = D3D12_RESOURCE_STATE_COPY_SOURCE;
					Transition(in.list, in.depth, *in.depthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
					*in.depthState = D3D12_RESOURCE_STATE_COPY_SOURCE;
					Transition(in.list, in.motion, *in.motionState, D3D12_RESOURCE_STATE_COPY_SOURCE);
					*in.motionState = D3D12_RESOURCE_STATE_COPY_SOURCE;
					for (uint32_t eye = 0; eye < 2; ++eye) {
						const UINT copyW = plans[eye].engineWidth;
						const UINT copyH = plans[eye].engineHeight;
						const UINT cropX = std::min(static_cast<UINT>(uvs[eye].x * eyeW), eyeW - copyW);
						const UINT cropY = std::min(static_cast<UINT>(uvs[eye].y * eyeH), eyeH - copyH);
						const UINT sbsX = (eye ? eyeW : 0) + cropX;
						Transition(in.list, eyeColor[eye].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
						Transition(in.list, eyeDepth[eye].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
						Transition(in.list, eyeMotion[eye].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
						CopyBox(in.list, in.color, eyeColor[eye].Get(), sbsX, cropY, copyW, copyH);
						CopyBox(in.list, in.depth, eyeDepth[eye].Get(), sbsX, cropY, copyW, copyH);
						CopyBox(in.list, in.motion, eyeMotion[eye].Get(), sbsX, cropY, copyW, copyH);
						Transition(in.list, eyeColor[eye].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
						Transition(in.list, eyeDepth[eye].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
						Transition(in.list, eyeMotion[eye].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
						boxX[eye] = static_cast<UINT>(uvs[eye].x * tdEyeW);
						boxY[eye] = static_cast<UINT>(uvs[eye].y * tdEyeH);
						boxW[eye] = plans[eye].displayWidth;
						boxH[eye] = plans[eye].displayHeight;
						if (boxW[eye] < tdEyeW && boxX[eye] + boxW[eye] > tdEyeW)
							boxX[eye] = tdEyeW - boxW[eye];
						if (boxH[eye] < tdEyeH && boxY[eye] + boxH[eye] > tdEyeH)
							boxY[eye] = tdEyeH - boxH[eye];
					}
					Transition(in.list, in.color, *in.colorState, D3D12_RESOURCE_STATE_COMMON);
					*in.colorState = D3D12_RESOURCE_STATE_COMMON;
					Transition(in.list, in.depth, *in.depthState, D3D12_RESOURCE_STATE_COMMON);
					*in.depthState = D3D12_RESOURCE_STATE_COMMON;
					Transition(in.list, in.motion, *in.motionState, D3D12_RESOURCE_STATE_COMMON);
					*in.motionState = D3D12_RESOURCE_STATE_COMMON;
					for (uint32_t eye = 0; eye < 2; ++eye) {
						if (!graph.EnsureSlotResources(eye, { eyeColor[eye].Get(), eyeDepth[eye].Get(), eyeMotion[eye].Get() }, plans[eye]))
							continue;
					}
					for (uint32_t eye = 0; eye < 2; ++eye) {
						const auto result = graph.Record(in.list, eye,
							{ eyeColor[eye].Get(), eyeDepth[eye].Get(), eyeMotion[eye].Get() }, plans[eye]);
						if (!result.finalResource || result.finalResource == eyeColor[eye].Get())
							continue;
						eyeFinal[eye] = result.finalResource;
						eyeState[eye] = result.state;
						eyeOk[eye] = true;
					}
					static bool logged = false;
					if (!logged && (eyeOk[0] || eyeOk[1])) {
						logged = true;
						logger::info("[VrDx12Db] DX12 DLSS SR {}x{} -> center {}x{} TD {}x{} nr={}",
							flat.engineWidth, flat.engineHeight, flat.displayWidth, flat.displayHeight, tdW, tdH,
							foveated.settings.neuralRenderingEnabled ? 1 : 0);
					}
				}
			}
		}

		Transition(in.list, in.color, *in.colorState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		*in.colorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		Transition(in.list, present.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ID3D12DescriptorHeap* heaps[]{ heap.Get() };
		in.list->SetDescriptorHeaps(1, heaps);
		VrPassTimer::Mark(in.list, "VrDb::Stretch");
		const auto stretchTable = Bind(in.device, 0, in.color, in.color->GetDesc().Format, present.Get(),
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

		VrPassTimer::Mark(in.list, "VrDb::Blend");
		const auto blendMode = foveated.GetSubrectBlendMode();
		for (uint32_t eye = 0; eye < 2; ++eye) {
			if (!eyeOk[eye] || !eyeFinal[eye] || !boxW[eye] || !boxH[eye])
				continue;
			const auto srcDesc = eyeFinal[eye]->GetDesc();
			Transition(in.list, eyeFinal[eye], eyeState[eye], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			const auto table = Bind(in.device, 2 + eye * 2, eyeFinal[eye], srcDesc.Format, present.Get(),
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
		Transition(in.list, present.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		presentW = tdW;
		presentH = tdH;
		out.present = present.Get();
		out.presentState = D3D12_RESOURCE_STATE_COMMON;
		out.width = tdW;
		out.height = tdH;
		VrPassTimer::Finish(in.list);
		return true;
	}
}
