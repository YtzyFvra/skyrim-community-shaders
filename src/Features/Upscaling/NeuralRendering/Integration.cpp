#include "Integration.h"

#include "Act2FsrScaler.h"
#include "ExperimentCapture.h"
#include "NISScaler.h"
#include "Renderer.h"
#include "Features/HDRDisplay.h"
#include "Features/Upscaling.h"
#include "Features/Upscaling/FoveatedRender/Bridge.h"
#include "Utils/D3D.h"
#include "Features/Upscaling/FoveatedRender/Core.h"
#include "Features/Upscaling/FoveatedRender/Ops.h"
#ifdef ENABLE_SKYRIM_VR
#	include "Features/Upscaling/VrAriseTail.h"
#	include "Features/Upscaling/VrSubmitDlssg.h"
#endif
#include "Globals.h"
#include "GpuPass.h"
#include "Utils/Game.h"
#include "Utils/LazyShader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <span>

#include <d3d11.h>

namespace NeuralRendering
{
	namespace
	{
		eastl::unique_ptr<Texture2D> color[2];
		eastl::unique_ptr<Texture2D> halfResolutionColor;
		eastl::unique_ptr<Texture2D> upscaledNeuralColor;
		eastl::unique_ptr<Texture2D> fusionOutput;
		eastl::unique_ptr<Texture2D> nrInputColor;
		eastl::unique_ptr<Texture2D> guidanceGain;
		eastl::unique_ptr<Texture2D> guidanceEditLow;
		eastl::unique_ptr<Texture2D> guideLowDisplay;
		bool bandCaptureRequested = false;
		eastl::unique_ptr<Texture2D> resizedDepthGuide;
		eastl::unique_ptr<Texture2D> resizedMotionGuide;
		eastl::unique_ptr<Texture2D> act2ReactiveMask;
		eastl::unique_ptr<Texture2D> act2TransparencyMask;
		eastl::unique_ptr<Texture2D> flatAct1Output;
		eastl::unique_ptr<Texture2D> flatAct2Output;
		eastl::unique_ptr<Texture2D> flatFusionReference;
		eastl::unique_ptr<Texture2D> flatSharpenOutput;
		eastl::unique_ptr<Texture2D> act3Color;
		eastl::unique_ptr<Texture2D> act3Depth;
		eastl::unique_ptr<Texture2D> act3Motion;
		eastl::unique_ptr<Texture2D> act3DepthFull;
		eastl::unique_ptr<Texture2D> act3MotionFull;
		eastl::unique_ptr<Texture2D> act3SharpenOutput;
		eastl::unique_ptr<Texture2D> act3Writeback;
		eastl::unique_ptr<Texture2D> act3Input;
		eastl::unique_ptr<Texture2D> act3Fusion;
		eastl::unique_ptr<Texture2D> act3Gain;
		eastl::unique_ptr<Texture2D> act3EditLow;
		eastl::unique_ptr<Texture2D> act3GuideLow;
		eastl::unique_ptr<Texture2D> vrCompositeSbs;
		eastl::unique_ptr<Texture2D> vrEyeColor[2];
		std::uint32_t colorWidth = 0;
		std::uint32_t colorHeight = 0;
		DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
		Util::LazyShader<ID3D11ComputeShader> downsampleCS;
		Util::LazyShader<ID3D11ComputeShader> tentCS;
		Util::LazyShader<ID3D11ComputeShader> mitchellCS;
		Util::LazyShader<ID3D11ComputeShader> easuCS;
		Util::LazyShader<ID3D11ComputeShader> fusionCS;
		Util::LazyShader<ID3D11ComputeShader> guidanceFitCS;
		Util::LazyShader<ID3D11ComputeShader> resizeGuidesCS;
		winrt::com_ptr<ID3D11Buffer> downsampleCB;
		winrt::com_ptr<ID3D11Buffer> fusionCB;
		winrt::com_ptr<ID3D11Buffer> resizeGuidesCB;
		NISScaler nisScaler;
		Act2FsrScaler act1FsrScaler;
		Act2FsrScaler act2FsrScaler;
		std::uint32_t lastAppliedFrame = UINT32_MAX;
		std::uint32_t lastAttemptedFrame = UINT32_MAX;
		bool writebackLogged = false;
		bool flatRouteWasActive = false;
		bool vrSixPassWasActive = false;
		bool vrSixPassLogged = false;
		bool vrSixPassTempLogged = false;
		std::uint32_t vrSixPassDoneFrame = UINT32_MAX;
		std::uint32_t lastFlatInputPrefilter = UINT32_MAX;
		std::uint32_t lastFlatMotionVectorMode = UINT32_MAX;
		bool lastFlatResetEveryFrame = false;
		bool lastFlatBypassOutput = false;
		bool flatPostInjectNrFallbackLogged = false;
		bool act2DlssLogged = false;
		bool act2FsrLogged = false;
		std::uint32_t lastFlatOutputMode = UINT32_MAX;
		std::uint32_t lastFlatAct1JitterMode = UINT32_MAX;
		std::uint32_t lastFlatAct2JitterMode = UINT32_MAX;
		std::uint64_t lastFlatPlanSignature = 0;
		std::uint64_t lastModel3PlanSignature = 0;
		bool hasModel3Plan = false;
		std::uint64_t lastModel3Frame = UINT64_MAX;
		std::uint64_t lastModel3CaptureFrame = UINT64_MAX;
		bool lastModel3Eligible = false;
		bool model3RestrictionLogged = false;
		bool model3CaptureFailureLogged = false;
		Act2BackendStats act2BackendStats;

		struct DownsampleConstants
		{
			std::uint32_t sourceWidth;
			std::uint32_t sourceHeight;
			std::uint32_t destinationWidth;
			std::uint32_t destinationHeight;
		};

		// Positional twin of the HLSL FusionConstants cbuffer (32 dwords); ARISE mirrors it as root constants.
		struct FusionConstants
		{
			std::uint32_t fullWidth;
			std::uint32_t fullHeight;
			std::uint32_t guidanceWidth;
			std::uint32_t guidanceHeight;
			float detailLuma;
			float detailChroma;
			float rdEdgeFloor;
			float regularization;
			std::uint32_t compareMode;
			std::uint32_t debugView;
			float gainPrior;
			float fitWindow;
			std::uint32_t depthWidth;
			std::uint32_t depthHeight;
			float maxGain;
			float depthGuidance;
			std::uint32_t edgeBlockMode;
			float edgeBlockGThresh;
			std::uint32_t neuralWidth;
			std::uint32_t neuralHeight;
			float cameraFar;
			float cameraNear;
			float cameraRange;
			float cameraProduct;
			float depthDilate;
			float detailGain;
			float toneGainLuma;
			float toneGainChroma;
			float ditherStrength;
			float edgeBlockSkip;
			float edgeBlockDilateD;
			float edgeBlockMix;
		};
		static_assert(sizeof(FusionConstants) == 32 * sizeof(std::uint32_t));

		enum class FusionKnobSet : std::uint32_t
		{
			kPrimary,
			kAct3,
		};

		std::uint32_t PackCropOrigin(std::uint32_t x, std::uint32_t y)
		{
			return (x & 0xffffu) | ((y & 0xffffu) << 16);
		}

		bool FitGuidanceGain(ID3D11DeviceContext* context, ID3D11ShaderResourceView* neuralInput,
			ID3D11ShaderResourceView* neuralColor, ID3D11ShaderResourceView* depthGuide,
			std::uint32_t neuralWidth, std::uint32_t neuralHeight, std::uint32_t depthWidth, std::uint32_t depthHeight,
			const FoveatedRender::Settings& settings,
			Texture2D* gainOut = nullptr, Texture2D* editOut = nullptr,
			FusionKnobSet knobs = FusionKnobSet::kPrimary, std::uint32_t cropPacked = 0);
		bool ResolveNeuralRendering(ID3D11DeviceContext* context, ID3D11ShaderResourceView* referenceColor,
			ID3D11ShaderResourceView* neuralColor, ID3D11ShaderResourceView* guideLow,
			ID3D11ShaderResourceView* depthGuide,
			std::uint32_t width, std::uint32_t height, std::uint32_t guidanceWidth, std::uint32_t guidanceHeight,
			std::uint32_t neuralWidth, std::uint32_t neuralHeight, std::uint32_t depthWidth, std::uint32_t depthHeight,
			const FoveatedRender::Settings& settings,
			Texture2D* gain = nullptr, Texture2D* editLow = nullptr, Texture2D* output = nullptr,
			FusionKnobSet knobs = FusionKnobSet::kPrimary, std::uint32_t cropPacked = 0);

		struct ResizeGuidesConstants
		{
			std::uint32_t sourceWidth;
			std::uint32_t sourceHeight;
			std::uint32_t targetWidth;
			std::uint32_t targetHeight;
			std::uint32_t guidanceWidth;
			std::uint32_t guidanceHeight;
			std::uint32_t filter;
			float motionVectorMultiplier;
		};

		float GetMotionVectorMultiplier(std::uint32_t mode)
		{
			switch (mode) {
			case 2: return 0.0f;
			case 3: return -1.0f;
			case 4: return 0.5f;
			case 5: return 2.0f;
			default: return 1.0f;
			}
		}

		float GetWorkingScale(const FoveatedRender::Settings& settings)
		{
			switch (settings.neuralRenderingWorkingScale) {
			case 1: return 2.0f / 3.0f;
			case 2: return 0.77f;
			default: return 0.5f;
			}
		}

		float GetInputPrefilterScale(const FoveatedRender::Settings& settings)
		{
			switch (settings.neuralRenderingInputPrefilter) {
			case 1: return 0.77f;
			case 2: return 2.0f / 3.0f;
			case 3: return 0.5f;
			default: return 1.0f;
			}
		}

		bool EnsureConstantBuffer(std::size_t size, winrt::com_ptr<ID3D11Buffer>& buffer)
		{
			const UINT byteWidth = static_cast<UINT>((size + 15) & ~std::size_t(15));
			if (buffer) {
				D3D11_BUFFER_DESC current{};
				buffer->GetDesc(&current);
				if (current.ByteWidth >= byteWidth)
					return true;
				buffer = nullptr;
			}
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = byteWidth;
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			return SUCCEEDED(globals::d3d::device->CreateBuffer(&desc, nullptr, buffer.put()));
		}

		bool EnsurePipelineTexture(eastl::unique_ptr<Texture2D>& texture, ID3D11Resource* source,
			std::uint32_t width, std::uint32_t height, const char* name)
		{
			if (!source || !width || !height)
				return false;
			winrt::com_ptr<ID3D11Texture2D> sourceTexture;
			if (FAILED(source->QueryInterface(sourceTexture.put())))
				return false;
			D3D11_TEXTURE2D_DESC sourceDesc{}, currentDesc{};
			sourceTexture->GetDesc(&sourceDesc);
			if (texture) {
				texture->resource->GetDesc(&currentDesc);
				if (currentDesc.Width == width && currentDesc.Height == height && currentDesc.Format == sourceDesc.Format)
					return true;
			}
			texture = Upscaling::CreateTextureFromSource(source, width, height, false, true, true, name);
			return texture && texture->srv && texture->uav;
		}

		// Gains exceed 1.0, so they cannot live in the scene color format; force fp16.
		bool EnsureFloatTexture(eastl::unique_ptr<Texture2D>& texture, std::uint32_t width, std::uint32_t height,
			const char* name)
		{
			if (!width || !height)
				return false;
			constexpr DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			if (texture) {
				D3D11_TEXTURE2D_DESC currentDesc{};
				texture->resource->GetDesc(&currentDesc);
				if (currentDesc.Width == width && currentDesc.Height == height && currentDesc.Format == format)
					return true;
			}
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			texture = eastl::make_unique<Texture2D>(desc);
			Util::SetResourceName(texture->resource.get(), name);
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			texture->CreateSRV(srvDesc);
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			texture->CreateUAV(uavDesc);
			return texture->srv && texture->uav;
		}

		template <class Constants>
		bool UpdateConstantBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const Constants& constants)
		{
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (!buffer || FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
				return false;
			std::memcpy(mapped.pData, &constants, sizeof(constants));
			context->Unmap(buffer, 0);
			return true;
		}

		ID3D11Texture2D* ResolveRenderTargetTexture(
			const RE::BSGraphics::RenderTargetData& target,
			winrt::com_ptr<ID3D11Texture2D>& holder)
		{
			if (target.texture)
				return target.texture;
			auto resolveView = [&](ID3D11View* view) -> ID3D11Texture2D* {
				if (!view)
					return nullptr;
				winrt::com_ptr<ID3D11Resource> resource;
				view->GetResource(resource.put());
				if (!resource || FAILED(resource->QueryInterface(holder.put())))
					return nullptr;
				return holder.get();
			};
			if (auto* texture = resolveView(target.SRV))
				return texture;
			return resolveView(target.RTV);
		}

		bool EnsureColorResources(ID3D11Resource* source, std::uint32_t width, std::uint32_t height)
		{
			winrt::com_ptr<ID3D11Texture2D> sourceTexture;
			if (!source || FAILED(source->QueryInterface(sourceTexture.put())))
				return false;
			D3D11_TEXTURE2D_DESC sourceDesc{};
			sourceTexture->GetDesc(&sourceDesc);
			if (color[0] && colorWidth == width && colorHeight == height && colorFormat == sourceDesc.Format)
				return true;
			const std::uint32_t resourceCount = globals::game::isVR ? 2u : 1u;
			for (std::uint32_t eye = 0; eye < resourceCount; ++eye) {
				color[eye] = Upscaling::CreateTextureFromSource(source, width, height, false, true, true,
					eye == 0 ? "NeuralRendering::LdrColorLeft" : "NeuralRendering::LdrColorRight");
				if (!color[eye])
					return false;
			}
			if (!globals::game::isVR)
				color[1].reset();
			colorWidth = width;
			colorHeight = height;
			colorFormat = sourceDesc.Format;
			return true;
		}

		bool EnsureFusionResources(ID3D11Resource* source, std::uint32_t width, std::uint32_t height,
			std::uint32_t lowWidth, std::uint32_t lowHeight)
		{
			winrt::com_ptr<ID3D11Texture2D> sourceTexture;
			if (!source || FAILED(source->QueryInterface(sourceTexture.put())))
				return false;
			D3D11_TEXTURE2D_DESC sourceDesc{};
			sourceTexture->GetDesc(&sourceDesc);
			if (!halfResolutionColor || !upscaledNeuralColor || !fusionOutput) {
				halfResolutionColor = Upscaling::CreateTextureFromSource(source, lowWidth, lowHeight, false, true, true,
					"NeuralRendering::HalfResolutionColor");
				upscaledNeuralColor = Upscaling::CreateTextureFromSource(source, width, height, false, true, true,
					"NeuralRendering::UpscaledNeuralColor");
				fusionOutput = Upscaling::CreateTextureFromSource(source, width, height, false, true, true,
					"NeuralRendering::FusionOutput");
			}
			if (!halfResolutionColor || !upscaledNeuralColor || !fusionOutput)
				return false;
			D3D11_TEXTURE2D_DESC halfDesc{}, upscaledDesc{}, fusionDesc{};
			halfResolutionColor->resource->GetDesc(&halfDesc);
			upscaledNeuralColor->resource->GetDesc(&upscaledDesc);
			fusionOutput->resource->GetDesc(&fusionDesc);
			if (halfDesc.Width != lowWidth || halfDesc.Height != lowHeight || halfDesc.Format != sourceDesc.Format ||
				upscaledDesc.Width != width || upscaledDesc.Height != height || upscaledDesc.Format != sourceDesc.Format ||
				fusionDesc.Width != width || fusionDesc.Height != height || fusionDesc.Format != sourceDesc.Format) {
				halfResolutionColor = Upscaling::CreateTextureFromSource(source, lowWidth, lowHeight, false, true, true,
					"NeuralRendering::HalfResolutionColor");
				upscaledNeuralColor = Upscaling::CreateTextureFromSource(source, width, height, false, true, true,
					"NeuralRendering::UpscaledNeuralColor");
				fusionOutput = Upscaling::CreateTextureFromSource(source, width, height, false, true, true,
					"NeuralRendering::FusionOutput");
			}
			return halfResolutionColor && halfResolutionColor->srv && halfResolutionColor->uav &&
				upscaledNeuralColor && upscaledNeuralColor->srv && upscaledNeuralColor->uav &&
				fusionOutput && fusionOutput->srv && fusionOutput->uav &&
				EnsureConstantBuffer(sizeof(DownsampleConstants), downsampleCB) &&
				EnsureConstantBuffer(sizeof(FusionConstants), fusionCB);
		}

		bool EnsureFlatPipelineResources(ID3D11Resource* source, const FoveatedRender::FlatPipelinePlan& plan)
		{
			winrt::com_ptr<ID3D11Texture2D> sourceTexture;
			if (!source || FAILED(source->QueryInterface(sourceTexture.put())))
				return false;
			D3D11_TEXTURE2D_DESC sourceDesc{};
			sourceTexture->GetDesc(&sourceDesc);
			auto matches = [](Texture2D* texture, std::uint32_t width, std::uint32_t height, DXGI_FORMAT format) {
				if (!texture)
					return false;
				D3D11_TEXTURE2D_DESC desc{};
				texture->resource->GetDesc(&desc);
				return desc.Width == width && desc.Height == height && desc.Format == format;
			};

			if (!matches(halfResolutionColor.get(), plan.nrWidth, plan.nrHeight, sourceDesc.Format))
				halfResolutionColor = Upscaling::CreateTextureFromSource(source, plan.nrWidth, plan.nrHeight,
					false, true, true, "NeuralRendering::NrInputColor");
			const bool needsAct2Spatial = plan.nrWidth != plan.displayWidth || plan.nrHeight != plan.displayHeight;
			const bool needsDisplayIntermediate = needsAct2Spatial && (
				plan.act2SrBackend == FoveatedRender::FlatAct2SrBackend::kNIS ||
				plan.act2SrBackend == FoveatedRender::FlatAct2SrBackend::kLanczos ||
				plan.act2SrBackend == FoveatedRender::FlatAct2SrBackend::kTent ||
				plan.act2SrBackend == FoveatedRender::FlatAct2SrBackend::kMitchell ||
				plan.act2SrBackend == FoveatedRender::FlatAct2SrBackend::kEasu);
			if (needsDisplayIntermediate &&
				!matches(upscaledNeuralColor.get(), plan.displayWidth, plan.displayHeight, sourceDesc.Format))
				upscaledNeuralColor = Upscaling::CreateTextureFromSource(source, plan.displayWidth, plan.displayHeight,
					false, true, true, "NeuralRendering::UpscaledNeuralColor");
			if (!needsDisplayIntermediate)
				upscaledNeuralColor.reset();

			const bool needsFusion = plan.fusion;
			const bool needsAct2Lo = plan.fusion || plan.act3Guidance;
			if (needsFusion && !matches(fusionOutput.get(), plan.displayWidth, plan.displayHeight, sourceDesc.Format))
				fusionOutput = Upscaling::CreateTextureFromSource(source, plan.displayWidth, plan.displayHeight,
					false, true, true, "NeuralRendering::FusionOutput");
			if (needsAct2Lo && !matches(nrInputColor.get(), plan.nrWidth, plan.nrHeight, sourceDesc.Format))
				nrInputColor = Upscaling::CreateTextureFromSource(source, plan.nrWidth, plan.nrHeight,
					false, true, true, "NeuralRendering::NrInputCopy");
			if (needsAct2Lo && !matches(guideLowDisplay.get(), plan.displayWidth, plan.displayHeight, sourceDesc.Format))
				guideLowDisplay = Upscaling::CreateTextureFromSource(source, plan.displayWidth, plan.displayHeight,
					false, true, true, "NeuralRendering::GuideLowDisplay");
			if (needsFusion && (!EnsureFloatTexture(guidanceGain, plan.nrWidth, plan.nrHeight, "NeuralRendering::GuidanceGain") ||
				!EnsureFloatTexture(guidanceEditLow, plan.nrWidth, plan.nrHeight, "NeuralRendering::GuidanceEditLow")))
				return false;
			if (!needsFusion) {
				fusionOutput.reset();
				guidanceGain.reset();
				guidanceEditLow.reset();
				if (!needsAct2Lo) {
					nrInputColor.reset();
					guideLowDisplay.reset();
					fusionCB = nullptr;
				}
			}
			if (plan.act1Backend == FoveatedRender::FlatAct1Backend::kBypass)
				flatAct1Output.reset();
			const bool needsTemporalAct2 = plan.act2SrBackend == FoveatedRender::FlatAct2SrBackend::kDLSS ||
				plan.act2SrBackend == FoveatedRender::FlatAct2SrBackend::kFSR3;
			if (!needsTemporalAct2) {
				flatAct2Output.reset();
				act2ReactiveMask.reset();
				act2TransparencyMask.reset();
			}
			if (!needsFusion)
				flatFusionReference.reset();
			if (!plan.sharpen)
				flatSharpenOutput.reset();

			return halfResolutionColor && halfResolutionColor->srv && halfResolutionColor->uav &&
				(!needsDisplayIntermediate || (upscaledNeuralColor && upscaledNeuralColor->srv && upscaledNeuralColor->uav)) &&
				(!needsAct2Lo || (nrInputColor && nrInputColor->srv &&
					guideLowDisplay && guideLowDisplay->srv && guideLowDisplay->uav &&
					EnsureConstantBuffer(sizeof(FusionConstants), fusionCB))) &&
				(!needsFusion || (fusionOutput && fusionOutput->srv && fusionOutput->uav)) &&
				EnsureConstantBuffer(sizeof(DownsampleConstants), downsampleCB);
		}

		bool EnsurePrefilterResources(ID3D11Resource* source, std::uint32_t width, std::uint32_t height,
			const FoveatedRender::Settings& settings)
		{
			const float scale = GetInputPrefilterScale(settings);
			const std::uint32_t lowWidth = std::max(1u, static_cast<std::uint32_t>(std::lround(width * scale)));
			const std::uint32_t lowHeight = std::max(1u, static_cast<std::uint32_t>(std::lround(height * scale)));
			winrt::com_ptr<ID3D11Texture2D> sourceTexture;
			if (!source || FAILED(source->QueryInterface(sourceTexture.put())))
				return false;
			D3D11_TEXTURE2D_DESC sourceDesc{};
			sourceTexture->GetDesc(&sourceDesc);
			auto matches = [](Texture2D* texture, std::uint32_t expectedWidth, std::uint32_t expectedHeight,
				DXGI_FORMAT expectedFormat) {
				if (!texture)
					return false;
				D3D11_TEXTURE2D_DESC desc{};
				texture->resource->GetDesc(&desc);
				return desc.Width == expectedWidth && desc.Height == expectedHeight && desc.Format == expectedFormat;
			};
			if (!matches(halfResolutionColor.get(), lowWidth, lowHeight, sourceDesc.Format))
				halfResolutionColor = Upscaling::CreateTextureFromSource(source, lowWidth, lowHeight, false, true, true,
					"NeuralRendering::PrefilterLowColor");
			if (!matches(upscaledNeuralColor.get(), width, height, sourceDesc.Format))
				upscaledNeuralColor = Upscaling::CreateTextureFromSource(source, width, height, false, true, true,
					"NeuralRendering::PrefilterRoundtripColor");
			return halfResolutionColor && upscaledNeuralColor &&
				EnsureConstantBuffer(sizeof(DownsampleConstants), downsampleCB);
		}

		bool EnsureGuideResources(ID3D11Resource* motionSource, std::uint32_t width, std::uint32_t height)
		{
			winrt::com_ptr<ID3D11Texture2D> motionTexture;
			if (!motionSource || FAILED(motionSource->QueryInterface(motionTexture.put())))
				return false;
			D3D11_TEXTURE2D_DESC motionSourceDesc{};
			motionTexture->GetDesc(&motionSourceDesc);

			auto resourcesMatch = [&] {
				if (!resizedDepthGuide || !resizedMotionGuide)
					return false;
				D3D11_TEXTURE2D_DESC depthDesc{}, motionDesc{};
				resizedDepthGuide->resource->GetDesc(&depthDesc);
				resizedMotionGuide->resource->GetDesc(&motionDesc);
				return depthDesc.Width == width && depthDesc.Height == height && depthDesc.Format == DXGI_FORMAT_R32_FLOAT &&
					motionDesc.Width == width && motionDesc.Height == height && motionDesc.Format == motionSourceDesc.Format;
			};
			if (!resourcesMatch()) {
				D3D11_TEXTURE2D_DESC depthDesc{};
				depthDesc.Width = width;
				depthDesc.Height = height;
				depthDesc.MipLevels = 1;
				depthDesc.ArraySize = 1;
				depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
				depthDesc.SampleDesc.Count = 1;
				depthDesc.Usage = D3D11_USAGE_DEFAULT;
				depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				resizedDepthGuide = eastl::make_unique<Texture2D>(depthDesc, "NeuralRendering::ResizedDepthGuide");
				D3D11_SHADER_RESOURCE_VIEW_DESC depthSRVDesc{};
				depthSRVDesc.Format = depthDesc.Format;
				depthSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				depthSRVDesc.Texture2D.MipLevels = 1;
				resizedDepthGuide->CreateSRV(depthSRVDesc);
				D3D11_UNORDERED_ACCESS_VIEW_DESC depthUAVDesc{};
				depthUAVDesc.Format = depthDesc.Format;
				depthUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				resizedDepthGuide->CreateUAV(depthUAVDesc);

				resizedMotionGuide = Upscaling::CreateTextureFromSource(motionSource, width, height, false, true, true,
					"NeuralRendering::ResizedMotionGuide");
			}
			return resizedDepthGuide && resizedDepthGuide->srv && resizedDepthGuide->uav &&
				resizedMotionGuide && resizedMotionGuide->srv && resizedMotionGuide->uav &&
				EnsureConstantBuffer(sizeof(ResizeGuidesConstants), resizeGuidesCB);
		}

		bool EnsureDepthMotionPair(eastl::unique_ptr<Texture2D>& depth, eastl::unique_ptr<Texture2D>& motion,
			ID3D11Resource* motionSource, std::uint32_t width, std::uint32_t height, const char* depthName, const char* motionName)
		{
			winrt::com_ptr<ID3D11Texture2D> motionTexture;
			if (!motionSource || FAILED(motionSource->QueryInterface(motionTexture.put())))
				return false;
			D3D11_TEXTURE2D_DESC motionSourceDesc{};
			motionTexture->GetDesc(&motionSourceDesc);
			auto matches = [&] {
				if (!depth || !motion)
					return false;
				D3D11_TEXTURE2D_DESC depthDesc{}, motionDesc{};
				depth->resource->GetDesc(&depthDesc);
				motion->resource->GetDesc(&motionDesc);
				return depthDesc.Width == width && depthDesc.Height == height && depthDesc.Format == DXGI_FORMAT_R32_FLOAT &&
					motionDesc.Width == width && motionDesc.Height == height && motionDesc.Format == motionSourceDesc.Format;
			};
			if (!matches()) {
				D3D11_TEXTURE2D_DESC depthDesc{};
				depthDesc.Width = width;
				depthDesc.Height = height;
				depthDesc.MipLevels = 1;
				depthDesc.ArraySize = 1;
				depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
				depthDesc.SampleDesc.Count = 1;
				depthDesc.Usage = D3D11_USAGE_DEFAULT;
				depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				depth = eastl::make_unique<Texture2D>(depthDesc, depthName);
				D3D11_SHADER_RESOURCE_VIEW_DESC depthSRVDesc{};
				depthSRVDesc.Format = depthDesc.Format;
				depthSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				depthSRVDesc.Texture2D.MipLevels = 1;
				depth->CreateSRV(depthSRVDesc);
				D3D11_UNORDERED_ACCESS_VIEW_DESC depthUAVDesc{};
				depthUAVDesc.Format = depthDesc.Format;
				depthUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				depth->CreateUAV(depthUAVDesc);
				motion = Upscaling::CreateTextureFromSource(motionSource, width, height, false, true, true, motionName);
			}
			return depth && depth->srv && depth->uav && motion && motion->srv && motion->uav &&
				EnsureConstantBuffer(sizeof(ResizeGuidesConstants), resizeGuidesCB);
		}

		bool EnsureAct2MaskResources(ID3D11Resource* reactiveSource, ID3D11Resource* transparencySource,
			std::uint32_t width, std::uint32_t height)
		{
			auto matches = [](Texture2D* texture, std::uint32_t expectedWidth, std::uint32_t expectedHeight) {
				if (!texture)
					return false;
				D3D11_TEXTURE2D_DESC desc{};
				texture->resource->GetDesc(&desc);
				return desc.Width == expectedWidth && desc.Height == expectedHeight;
			};
			if (!matches(act2ReactiveMask.get(), width, height))
				act2ReactiveMask = Upscaling::CreateTextureFromSource(reactiveSource, width, height, false, true, true,
					"NeuralRendering::Act2ReactiveMask");
			if (!matches(act2TransparencyMask.get(), width, height))
				act2TransparencyMask = Upscaling::CreateTextureFromSource(transparencySource, width, height, false, true, true,
					"NeuralRendering::Act2TransparencyMask");
			if (!act2ReactiveMask || !act2TransparencyMask || !act2ReactiveMask->uav || !act2TransparencyMask->uav)
				return false;
			const UINT clear[4]{};
			globals::d3d::context->ClearUnorderedAccessViewUint(act2ReactiveMask->uav.get(), clear);
			globals::d3d::context->ClearUnorderedAccessViewUint(act2TransparencyMask->uav.get(), clear);
			return true;
		}

		bool ResizeGuidesTo(ID3D11DeviceContext* context, ID3D11ShaderResourceView* sourceDepth,
			ID3D11ShaderResourceView* sourceMotion, Texture2D* destDepth, Texture2D* destMotion,
			std::uint32_t sourceWidth, std::uint32_t sourceHeight,
			std::uint32_t targetWidth, std::uint32_t targetHeight, float motionVectorMultiplier,
			std::uint32_t guidanceWidth, std::uint32_t guidanceHeight, std::uint32_t filter)
		{
			CS_GPU_PASS("FlatPipeline::ResizeGuides");
			auto* shader = resizeGuidesCS.Get(
				L"Data\\Shaders\\Upscaling\\NeuralRendering\\ResizeGuidesCS.hlsl", {}, "cs_5_0",
				"main", "NeuralRendering::ResizeGuidesCS");
			if (!shader || !sourceDepth || !sourceMotion || !destDepth || !destMotion || !destDepth->uav || !destMotion->uav)
				return false;
			const ResizeGuidesConstants constants{
				sourceWidth, sourceHeight, targetWidth, targetHeight,
				guidanceWidth, guidanceHeight, filter, motionVectorMultiplier
			};
			if (!UpdateConstantBuffer(context, resizeGuidesCB.get(), constants))
				return false;

			context->CSSetShader(shader, nullptr, 0);
			ID3D11Buffer* buffers[]{ resizeGuidesCB.get() };
			context->CSSetConstantBuffers(0, 1, buffers);
			ID3D11ShaderResourceView* srvs[]{ sourceDepth, sourceMotion };
			context->CSSetShaderResources(0, 2, srvs);
			ID3D11UnorderedAccessView* uavs[]{ destDepth->uav.get(), destMotion->uav.get() };
			context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
			context->Dispatch((targetWidth + 7) / 8, (targetHeight + 7) / 8, 1);

			ID3D11Buffer* nullBuffer[]{ nullptr };
			ID3D11ShaderResourceView* nullSRVs[]{ nullptr, nullptr };
			ID3D11UnorderedAccessView* nullUAVs[]{ nullptr, nullptr };
			context->CSSetConstantBuffers(0, 1, nullBuffer);
			context->CSSetShaderResources(0, 2, nullSRVs);
			context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
			return true;
		}

		bool ResizeGuides(ID3D11DeviceContext* context, ID3D11ShaderResourceView* sourceDepth,
			ID3D11ShaderResourceView* sourceMotion, std::uint32_t sourceWidth, std::uint32_t sourceHeight,
			std::uint32_t targetWidth, std::uint32_t targetHeight, float motionVectorMultiplier,
			std::uint32_t guidanceWidth, std::uint32_t guidanceHeight, std::uint32_t filter)
		{
			return ResizeGuidesTo(context, sourceDepth, sourceMotion, resizedDepthGuide.get(), resizedMotionGuide.get(),
				sourceWidth, sourceHeight, targetWidth, targetHeight, motionVectorMultiplier,
				guidanceWidth, guidanceHeight, filter);
		}

		Tuning GetAct3Tuning(const FoveatedRender::Settings& settings);

		bool ApplyAct3(Upscaling& upscaling, FoveatedRender& foveated, ID3D11DeviceContext* context,
			ID3D11Resource* formatSource, const FoveatedRender::FlatPipelinePlan& plan,
			ID3D11Resource* motionR, ID3D11ShaderResourceView* depthRSRV, ID3D11ShaderResourceView* motionRSRV,
			ID3D11ShaderResourceView* guidanceSRV, std::uint32_t guidanceWidth, std::uint32_t guidanceHeight,
			ID3D11Resource* act2Rc, ID3D11Resource*& finalResource, ID3D11ShaderResourceView*& finalSRV, bool forceReset,
			std::uint32_t eyeIndex = 0, float motionScaleX = 0.0f, float motionScaleY = 0.0f)
		{
			if (!foveated.settings.act3Enabled)
				return true;
			const auto crop = FoveatedRender::MakeAct3Crop(plan.displayWidth, plan.displayHeight, foveated.settings.act3Scale);
			if (!crop.Valid() || !finalResource || !finalSRV || !depthRSRV || !motionR || !motionRSRV)
				return true;
			CS_GPU_PASS("FlatPipeline::ACT3");
			if (!EnsurePipelineTexture(act3Color, formatSource, crop.width, crop.height, "NeuralRendering::Act3Color") ||
				!EnsureDepthMotionPair(act3DepthFull, act3MotionFull, motionR, plan.displayWidth, plan.displayHeight,
					"NeuralRendering::Act3DepthD", "NeuralRendering::Act3MotionD") ||
				!EnsureDepthMotionPair(act3Depth, act3Motion, motionR, crop.width, crop.height,
					"NeuralRendering::Act3Depth", "NeuralRendering::Act3Motion") ||
				!ResizeGuidesTo(context, depthRSRV, motionRSRV, act3DepthFull.get(), act3MotionFull.get(),
					plan.engineWidth, plan.engineHeight, plan.displayWidth, plan.displayHeight, 1.0f,
					0, 0, static_cast<std::uint32_t>(plan.act1DepthDownsample)))
				return false;

			const bool ownGuidance = foveated.settings.act3GuidanceEnabled;
			ID3D11Resource* cropSource = (ownGuidance && act2Rc) ? act2Rc : finalResource;
			const D3D11_BOX colorBox{ crop.x, crop.y, 0, crop.x + crop.width, crop.y + crop.height, 1 };
			if (ownGuidance) {
				if (!EnsurePipelineTexture(act3Input, formatSource, crop.width, crop.height, "NeuralRendering::Act3Input"))
					return false;
				context->CopySubresourceRegion(act3Input->resource.get(), 0, 0, 0, 0, cropSource, 0, &colorBox);
				context->CopyResource(act3Color->resource.get(), act3Input->resource.get());
			} else {
				context->CopySubresourceRegion(act3Color->resource.get(), 0, 0, 0, 0, cropSource, 0, &colorBox);
			}
			if (!FoveatedRenderImpl::Ops::CopyDepthSubrect(act3DepthFull->srv.get(), act3Depth->uav.get(),
					crop.x, crop.y, 0, 0, crop.width, crop.height))
				return false;
			context->CopySubresourceRegion(act3Motion->resource.get(), 0, 0, 0, 0,
				act3MotionFull->resource.get(), 0, &colorBox);

			const float act3MvecX = motionScaleX > 0.0f ? motionScaleX : static_cast<float>(plan.engineWidth);
			const float act3MvecY = motionScaleY > 0.0f ? motionScaleY : static_cast<float>(plan.engineHeight);
			// Host slots 0/1 are per-eye NR1. ACT3 keeps 2/3 so N vs crop S never recreate Feature 18.
			const std::uint32_t act3Slot = eyeIndex + Runtime::kAct3FeatureSlotBase;
			if (!Renderer::Instance().Apply(globals::d3d::device, context, act3Slot,
					act3Color->resource.get(), act3Depth->resource.get(), act3Depth->srv.get(),
					act3Motion->resource.get(), crop.width, crop.height, crop.width, crop.height,
					act3MvecX, act3MvecY,
					GetAct3Tuning(foveated.settings), forceReset || foveated.settings.act3ResetEveryFrame))
				return false;

			ID3D11Resource* writebackSource = act3Color->resource.get();
			if (ownGuidance && guidanceSRV && guidanceWidth && guidanceHeight) {
				if (!EnsureConstantBuffer(sizeof(FusionConstants), fusionCB) ||
					!EnsureFloatTexture(act3Gain, crop.width, crop.height, "NeuralRendering::Act3Gain") ||
					!EnsureFloatTexture(act3EditLow, crop.width, crop.height, "NeuralRendering::Act3EditLow") ||
					!EnsurePipelineTexture(act3Fusion, formatSource, crop.width, crop.height, "NeuralRendering::Act3Fusion"))
					return false;
				ID3D11ShaderResourceView* act3LoSRV = act3Input ? act3Input->srv.get() : act3Color->srv.get();
				if (guideLowDisplay && guideLowDisplay->resource) {
					if (!EnsurePipelineTexture(act3GuideLow, formatSource, crop.width, crop.height,
							"NeuralRendering::Act3GuideLow"))
						return false;
					context->CopySubresourceRegion(act3GuideLow->resource.get(), 0, 0, 0, 0,
						guideLowDisplay->resource.get(), 0, &colorBox);
					act3LoSRV = act3GuideLow->srv.get();
				}
				const std::uint32_t cropPacked = PackCropOrigin(crop.x, crop.y);
				if (FitGuidanceGain(context, act3LoSRV, act3Color->srv.get(), act3Depth->srv.get(),
						crop.width, crop.height, crop.width, crop.height, foveated.settings,
						act3Gain.get(), act3EditLow.get(), FusionKnobSet::kAct3, cropPacked) &&
					ResolveNeuralRendering(context, guidanceSRV, act3Color->srv.get(), act3LoSRV,
						act3Depth->srv.get(), plan.displayWidth, plan.displayHeight, guidanceWidth, guidanceHeight,
						crop.width, crop.height, crop.width, crop.height, foveated.settings,
						act3Gain.get(), act3EditLow.get(), act3Fusion.get(), FusionKnobSet::kAct3, cropPacked))
					writebackSource = act3Fusion->resource.get();
				else
					logger::warn("[ACT3] Crop Guidance failed; writing raw ACT3 NR");
			}

			auto pipelineUav = [&](ID3D11Resource* resource) -> ID3D11UnorderedAccessView* {
				const eastl::unique_ptr<Texture2D>* textures[] = {
					&upscaledNeuralColor, &fusionOutput, &flatSharpenOutput, &flatAct1Output,
					&flatAct2Output, &halfResolutionColor, &act3Writeback
				};
				for (auto* slot : textures) {
					if (*slot && (*slot)->resource.get() == resource)
						return (*slot)->uav.get();
				}
				return nullptr;
			};
			auto* destUav = pipelineUav(finalResource);
			if (!destUav) {
				if (!EnsurePipelineTexture(act3Writeback, formatSource, plan.displayWidth, plan.displayHeight,
						"NeuralRendering::Act3Writeback"))
					return false;
				context->CopyResource(act3Writeback->resource.get(), finalResource);
				finalResource = act3Writeback->resource.get();
				finalSRV = act3Writeback->srv.get();
				destUav = act3Writeback->uav.get();
			}
			FoveatedRenderImpl::Ops::BlendSubrectToOutput(
				writebackSource, finalResource, destUav,
				crop.x, crop.y, crop.width, crop.height, 0,
				static_cast<std::uint32_t>(FoveatedRender::SubrectBlendMode::kFeather),
				foveated.settings.act3FeatherWidth, 0.0f);

			if (foveated.settings.act3Sharpen) {
				if (!EnsurePipelineTexture(act3SharpenOutput, formatSource, plan.displayWidth, plan.displayHeight,
						"NeuralRendering::Act3Sharpen") || !finalSRV)
					return false;
				upscaling.rcas.Initialize();
				if (upscaling.rcas.ApplySharpen(finalSRV, act3SharpenOutput->uav.get(), foveated.settings.act3Sharpness)) {
					finalResource = act3SharpenOutput->resource.get();
					finalSRV = act3SharpenOutput->srv.get();
				}
			}
			return true;
		}

		bool ResampleSpatial(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source,
			ID3D11UnorderedAccessView* destination, std::uint32_t sourceWidth, std::uint32_t sourceHeight,
			std::uint32_t destinationWidth, std::uint32_t destinationHeight,
			FoveatedRender::FlatAct2SrBackend backend)
		{
			const char* passName = backend == FoveatedRender::FlatAct2SrBackend::kTent ? "FlatPipeline::Tent" :
				backend == FoveatedRender::FlatAct2SrBackend::kMitchell ? "FlatPipeline::Mitchell" :
				backend == FoveatedRender::FlatAct2SrBackend::kEasu ? "FlatPipeline::EASU" : "FlatPipeline::Lanczos";
			CS_GPU_PASS_DYNAMIC(passName);
			ID3D11ComputeShader* shader = nullptr;
			switch (backend) {
			case FoveatedRender::FlatAct2SrBackend::kTent:
				shader = tentCS.Get(
					L"Data\\Shaders\\Upscaling\\NeuralRendering\\ResampleTentCS.hlsl", {}, "cs_5_0",
					"main", "NeuralRendering::ResampleTentCS");
				break;
			case FoveatedRender::FlatAct2SrBackend::kMitchell:
				shader = mitchellCS.Get(
					L"Data\\Shaders\\Upscaling\\NeuralRendering\\ResampleMitchellCS.hlsl", {}, "cs_5_0",
					"main", "NeuralRendering::ResampleMitchellCS");
				break;
			case FoveatedRender::FlatAct2SrBackend::kEasu:
				shader = easuCS.Get(
					L"Data\\Shaders\\Upscaling\\NeuralRendering\\ResampleEasuCS.hlsl", {}, "cs_5_0",
					"main", "NeuralRendering::ResampleEasuCS");
				break;
			default:
				shader = downsampleCS.Get(
					L"Data\\Shaders\\Upscaling\\NeuralRendering\\DownsampleColorCS.hlsl", {}, "cs_5_0",
					"main", "NeuralRendering::DownsampleColorCS");
				break;
			}
			if (!shader || !source || !destination)
				return false;
			const DownsampleConstants constants{ sourceWidth, sourceHeight, destinationWidth, destinationHeight };
			if (!UpdateConstantBuffer(context, downsampleCB.get(), constants))
				return false;

			context->CSSetShader(shader, nullptr, 0);
			ID3D11Buffer* buffers[]{ downsampleCB.get() };
			context->CSSetConstantBuffers(0, 1, buffers);
			ID3D11ShaderResourceView* srvs[]{ source };
			context->CSSetShaderResources(0, 1, srvs);
			ID3D11UnorderedAccessView* uavs[]{ destination };
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->Dispatch((destinationWidth + 7) / 8, (destinationHeight + 7) / 8, 1);

			ID3D11Buffer* nullBuffer[]{ nullptr };
			ID3D11ShaderResourceView* nullSRV[]{ nullptr };
			ID3D11UnorderedAccessView* nullUAV[]{ nullptr };
			context->CSSetConstantBuffers(0, 1, nullBuffer);
			context->CSSetShaderResources(0, 1, nullSRV);
			context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
			return true;
		}

		bool BuildGuideLowDisplay(ID3D11DeviceContext* context, const FoveatedRender::FlatPipelinePlan& plan)
		{
			if (!nrInputColor || !guideLowDisplay)
				return false;
			using Act2 = FoveatedRender::FlatAct2SrBackend;
			const bool spatialAct2 = plan.act2SrBackend == Act2::kLanczos || plan.act2SrBackend == Act2::kTent ||
				plan.act2SrBackend == Act2::kMitchell || plan.act2SrBackend == Act2::kEasu;
			if (plan.nrWidth == plan.displayWidth && plan.nrHeight == plan.displayHeight)
				context->CopyResource(guideLowDisplay->resource.get(), nrInputColor->resource.get());
			else if (!ResampleSpatial(context, nrInputColor->srv.get(), guideLowDisplay->uav.get(),
					plan.nrWidth, plan.nrHeight, plan.displayWidth, plan.displayHeight,
					spatialAct2 ? plan.act2SrBackend : Act2::kLanczos))
				return false;
			return true;
		}

		bool ResampleLanczos(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source,
			ID3D11UnorderedAccessView* destination, std::uint32_t sourceWidth, std::uint32_t sourceHeight,
			std::uint32_t destinationWidth, std::uint32_t destinationHeight)
		{
			return ResampleSpatial(context, source, destination, sourceWidth, sourceHeight,
				destinationWidth, destinationHeight, FoveatedRender::FlatAct2SrBackend::kLanczos);
		}

		bool DownsampleForNeuralRendering(ID3D11DeviceContext* context, std::uint32_t width, std::uint32_t height)
		{
			if (!color[0] || !halfResolutionColor)
				return false;
			D3D11_TEXTURE2D_DESC lowDesc{};
			halfResolutionColor->resource->GetDesc(&lowDesc);
			return ResampleLanczos(context, color[0]->srv.get(), halfResolutionColor->uav.get(),
				width, height, lowDesc.Width, lowDesc.Height);
		}

		bool UpscaleNeuralToDisplay(ID3D11DeviceContext* context,
			std::uint32_t inputWidth, std::uint32_t inputHeight,
			std::uint32_t outputWidth, std::uint32_t outputHeight, bool* usedNis = nullptr)
		{
			if (usedNis)
				*usedNis = false;
			if (!halfResolutionColor || !upscaledNeuralColor)
				return false;
			if (inputWidth == outputWidth && inputHeight == outputHeight) {
				context->CopyResource(upscaledNeuralColor->resource.get(), halfResolutionColor->resource.get());
				return true;
			}
			const float scaleX = static_cast<float>(inputWidth) / static_cast<float>(outputWidth);
			const float scaleY = static_cast<float>(inputHeight) / static_cast<float>(outputHeight);
			const bool nisScaleSupported = scaleX >= 0.5f && scaleX <= 1.0f && scaleY >= 0.5f && scaleY <= 1.0f;
			if (nisScaleSupported) {
				if (nisScaler.Apply(globals::d3d::device, context, halfResolutionColor->srv.get(),
						upscaledNeuralColor->uav.get(), inputWidth, inputHeight, outputWidth, outputHeight, 0.0f)) {
					if (usedNis)
						*usedNis = true;
					return true;
				}
				static bool runtimeFallbackLogged = false;
				if (!runtimeFallbackLogged) {
					logger::warn("[ACT2] NIS upscale failed at supported scale; falling back to HQ Lanczos");
					runtimeFallbackLogged = true;
				}
			}
			return ResampleLanczos(context, halfResolutionColor->srv.get(), upscaledNeuralColor->uav.get(),
				inputWidth, inputHeight, outputWidth, outputHeight);
		}

		FusionConstants MakeFusionConstants(std::uint32_t width, std::uint32_t height,
			std::uint32_t guidanceWidth, std::uint32_t guidanceHeight,
			std::uint32_t depthWidth, std::uint32_t depthHeight,
			std::uint32_t neuralWidth, std::uint32_t neuralHeight, const FoveatedRender::Settings& settings,
			FusionKnobSet knobs = FusionKnobSet::kPrimary, std::uint32_t cropPacked = 0)
		{
			const auto camera = Util::GetCameraData();
			const bool act3 = knobs == FusionKnobSet::kAct3;
			return FusionConstants{
				width, height, guidanceWidth, guidanceHeight,
				act3 ? settings.act3GuidanceLuma : settings.neuralRenderingFusionLuma,
				act3 ? settings.act3GuidanceChroma : settings.neuralRenderingFusionChroma,
				settings.neuralRenderingFusionEdgeProtection,
				settings.neuralRenderingFusionResidualLimit,
				act3 ? 0u : settings.fusionCompareMode,
				act3 ? 0u : settings.fusionDebugView,
				act3 ? settings.act3GuidancePrior : settings.fusionGainPrior,
				settings.flatPipelineFusionSoftness,
				depthWidth,
				depthHeight,
				act3 ? settings.act3GuidanceMaxGain : settings.flatPipelineFusionFrequencyGuidance,
				settings.flatPipelineFusionDepthGuidance,
				cropPacked,
				settings.fusionEdgeBlockGThresh,
				neuralWidth,
				neuralHeight,
				camera.x, camera.y, camera.z, camera.w,
				settings.fusionDepthDilate,
				act3 ? settings.act3GuidanceDetailGain : settings.fusionDetailGain,
				act3 ? settings.act3GuidanceToneLuma : settings.fusionToneGainLuma,
				act3 ? settings.act3GuidanceToneChroma : settings.fusionToneGainChroma,
				act3 ? settings.act3GuidanceDither : settings.fusionDither,
				settings.fusionFitSideSigma,
				act3 ? settings.act3GuidanceDirSharpen : settings.fusionDirectionalSharpen,
				settings.fusionSoftEdge,
			};
		}

		void DispatchFusionShader(ID3D11DeviceContext* context, ID3D11ComputeShader* shader,
			std::span<ID3D11ShaderResourceView* const> srvs, std::span<ID3D11UnorderedAccessView* const> uavs,
			std::uint32_t width, std::uint32_t height)
		{
			constexpr UINT kSrvSlots = 8;
			constexpr UINT kUavSlots = 2;
			ID3D11ShaderResourceView* boundSRVs[kSrvSlots]{};
			ID3D11UnorderedAccessView* boundUAVs[kUavSlots]{};
			ID3D11ShaderResourceView* nullSRVs[kSrvSlots]{};
			ID3D11UnorderedAccessView* nullUAVs[kUavSlots]{};
			for (std::size_t index = 0; index < srvs.size() && index < kSrvSlots; ++index)
				boundSRVs[index] = srvs[index];
			for (std::size_t index = 0; index < uavs.size() && index < kUavSlots; ++index)
				boundUAVs[index] = uavs[index];
			context->CSSetShader(shader, nullptr, 0);
			ID3D11Buffer* buffers[]{ fusionCB.get() };
			context->CSSetConstantBuffers(0, 1, buffers);
			context->CSSetShaderResources(0, kSrvSlots, boundSRVs);
			context->CSSetUnorderedAccessViews(0, kUavSlots, boundUAVs, nullptr);
			context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

			ID3D11Buffer* nullBuffer[]{ nullptr };
			context->CSSetConstantBuffers(0, 1, nullBuffer);
			context->CSSetShaderResources(0, kSrvSlots, nullSRVs);
			context->CSSetUnorderedAccessViews(0, kUavSlots, nullUAVs, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
		}

		/** @brief Fits per-channel NR gains at N; same-side samples only, a blended by ρ². */
		bool FitGuidanceGain(ID3D11DeviceContext* context, ID3D11ShaderResourceView* neuralInput,
			ID3D11ShaderResourceView* neuralColor, ID3D11ShaderResourceView* depthGuide,
			std::uint32_t neuralWidth, std::uint32_t neuralHeight, std::uint32_t depthWidth, std::uint32_t depthHeight,
			const FoveatedRender::Settings& settings,
			Texture2D* gainOut, Texture2D* editOut, FusionKnobSet knobs, std::uint32_t cropPacked)
		{
			CS_GPU_PASS("FlatPipeline::GuidanceFit");
			auto* shader = guidanceFitCS.Get(
				L"Data\\Shaders\\Upscaling\\NeuralRendering\\GuidanceFitCS.hlsl", {}, "cs_5_0",
				"main", "NeuralRendering::GuidanceFitCS");
			if (!gainOut)
				gainOut = guidanceGain.get();
			if (!editOut)
				editOut = guidanceEditLow.get();
			if (!shader || !neuralInput || !neuralColor || !gainOut || !editOut || !fusionCB)
				return false;
			const auto constants = MakeFusionConstants(0, 0, 0, 0, depthGuide ? depthWidth : 0u, depthGuide ? depthHeight : 0u,
				neuralWidth, neuralHeight, settings, knobs, cropPacked);
			if (!UpdateConstantBuffer(context, fusionCB.get(), constants))
				return false;
			ID3D11ShaderResourceView* const srvs[]{ neuralInput, neuralColor, depthGuide };
			ID3D11UnorderedAccessView* const uavs[]{ gainOut->uav.get(), editOut->uav.get() };
			DispatchFusionShader(context, shader, srvs, uavs, neuralWidth, neuralHeight);
			return true;
		}

		/** @brief Lifts RC@D by adding the fitted NR gain times the G detail above the N band. */
		bool ResolveNeuralRendering(ID3D11DeviceContext* context, ID3D11ShaderResourceView* referenceColor,
			ID3D11ShaderResourceView* neuralColor, ID3D11ShaderResourceView* guideLow,
			ID3D11ShaderResourceView* depthGuide,
			std::uint32_t width, std::uint32_t height, std::uint32_t guidanceWidth, std::uint32_t guidanceHeight,
			std::uint32_t neuralWidth, std::uint32_t neuralHeight, std::uint32_t depthWidth, std::uint32_t depthHeight,
			const FoveatedRender::Settings& settings,
			Texture2D* gain, Texture2D* editLow, Texture2D* output, FusionKnobSet knobs, std::uint32_t cropPacked)
		{
			CS_GPU_PASS("FlatPipeline::Guidance");
			auto* shader = fusionCS.Get(
				L"Data\\Shaders\\Upscaling\\NeuralRendering\\ResolveFusionCS.hlsl", {}, "cs_5_0",
				"main", "NeuralRendering::ResolveFusionCS");
			if (!gain)
				gain = guidanceGain.get();
			if (!editLow)
				editLow = guidanceEditLow.get();
			if (!output)
				output = fusionOutput.get();
			if (!shader || !referenceColor || !neuralColor || !guideLow || !gain || !editLow || !output)
				return false;
			const auto constants = MakeFusionConstants(width, height, guidanceWidth, guidanceHeight,
				depthGuide ? depthWidth : 0u, depthGuide ? depthHeight : 0u, neuralWidth, neuralHeight, settings,
				knobs, cropPacked);
			if (!UpdateConstantBuffer(context, fusionCB.get(), constants))
				return false;
			ID3D11ShaderResourceView* const srvs[]{ referenceColor, neuralColor, gain->srv.get(), guideLow,
				editLow->srv.get(), depthGuide };
			ID3D11UnorderedAccessView* const uavs[]{ output->uav.get() };
			const std::uint32_t dispatchWidth = knobs == FusionKnobSet::kAct3 ? neuralWidth : width;
			const std::uint32_t dispatchHeight = knobs == FusionKnobSet::kAct3 ? neuralHeight : height;
			DispatchFusionShader(context, shader, srvs, uavs, dispatchWidth, dispatchHeight);
			return true;
		}

		bool ApplyAct2Dlss(Upscaling& upscaling, ID3D11DeviceContext* context,
			ID3D11ShaderResourceView* sourceDepth, ID3D11ShaderResourceView* sourceMotion,
			std::uint32_t sourceGuideWidth, std::uint32_t sourceGuideHeight,
			std::uint32_t inputWidth, std::uint32_t inputHeight,
			std::uint32_t outputWidth, std::uint32_t outputHeight, float motionVectorMultiplier, bool forceReset,
			std::uint32_t guidanceWidth = 0, std::uint32_t guidanceHeight = 0)
		{
			if (!halfResolutionColor || !upscaledNeuralColor || !upscaling.reactiveMaskTexture ||
				!upscaling.transparencyCompositionMaskTexture ||
				!EnsureGuideResources(upscaling.motionVectorCopyTexture->resource.get(), inputWidth, inputHeight) ||
				!ResizeGuides(context, sourceDepth, sourceMotion, sourceGuideWidth, sourceGuideHeight,
					inputWidth, inputHeight, motionVectorMultiplier, guidanceWidth, guidanceHeight,
					upscaling.foveatedRender.settings.unifiedPipeline.act1DepthDownsample) ||
				!EnsureAct2MaskResources(upscaling.reactiveMaskTexture->resource.get(),
					upscaling.transparencyCompositionMaskTexture->resource.get(), inputWidth, inputHeight))
				return false;

			const sl::Extent extentIn{ 0, 0, inputWidth, inputHeight };
			const sl::Extent extentOut{ 0, 0, outputWidth, outputHeight };
			const sl::DLSSMode mode = Streamline::DLSSModeForQualityMode(
				upscaling.foveatedRender.settings.flatAct2QualityMode);
			const bool succeeded = Upscaling::streamline.EvaluateDLSS(Upscaling::streamline.flatAct2Viewport, 0,
				halfResolutionColor->resource.get(), upscaledNeuralColor->resource.get(),
				resizedDepthGuide->resource.get(), resizedMotionGuide->resource.get(),
				act2ReactiveMask->resource.get(), act2TransparencyMask->resource.get(),
				extentIn, extentOut, outputWidth, outputHeight, mode, false, forceReset,
				upscaling.settings.flatAct2JitterMode == 1,
				static_cast<float>(inputWidth) / static_cast<float>(sourceGuideWidth),
				static_cast<float>(inputHeight) / static_cast<float>(sourceGuideHeight),
				upscaling.foveatedRender.settings.flatAct2Preset,
				upscaling.foveatedRender.settings.flatAct2Sharpness);
			if (succeeded && !act2DlssLogged) {
				logger::info("[ACT2] DLSS-SR after NR active input={}x{} output={}x{} viewport=3 mode={}",
					inputWidth, inputHeight, outputWidth, outputHeight, magic_enum::enum_name(mode));
				act2DlssLogged = true;
			}
			if (succeeded)
				++act2BackendStats.dlssSucceeded;
			return succeeded;
		}

		bool ApplyAct2Fsr(Upscaling& upscaling, ID3D11DeviceContext* context,
			ID3D11ShaderResourceView* sourceDepth, ID3D11ShaderResourceView* sourceMotion,
			std::uint32_t sourceGuideWidth, std::uint32_t sourceGuideHeight,
			std::uint32_t inputWidth, std::uint32_t inputHeight,
			std::uint32_t outputWidth, std::uint32_t outputHeight,
			float motionVectorMultiplier, bool forceReset,
			std::uint32_t guidanceWidth = 0, std::uint32_t guidanceHeight = 0)
		{
			if (!halfResolutionColor || !upscaledNeuralColor || !upscaling.reactiveMaskTexture ||
				!upscaling.transparencyCompositionMaskTexture ||
				!EnsureGuideResources(upscaling.motionVectorCopyTexture->resource.get(), inputWidth, inputHeight) ||
				!ResizeGuides(context, sourceDepth, sourceMotion, sourceGuideWidth, sourceGuideHeight,
					inputWidth, inputHeight, motionVectorMultiplier, guidanceWidth, guidanceHeight,
					upscaling.foveatedRender.settings.unifiedPipeline.act1DepthDownsample) ||
				!EnsureAct2MaskResources(upscaling.reactiveMaskTexture->resource.get(),
					upscaling.transparencyCompositionMaskTexture->resource.get(), inputWidth, inputHeight))
				return false;

			const bool succeeded = act2FsrScaler.Apply(globals::d3d::device, context,
				halfResolutionColor->resource.get(), resizedDepthGuide->resource.get(), resizedMotionGuide->resource.get(),
				act2ReactiveMask->resource.get(), act2TransparencyMask->resource.get(), upscaledNeuralColor->resource.get(),
				inputWidth, inputHeight, outputWidth, outputHeight,
				static_cast<float>(sourceGuideWidth), static_cast<float>(sourceGuideHeight),
				upscaling.settings.flatAct2JitterMode == 0 ? upscaling.jitter.x * inputWidth / sourceGuideWidth : 0.0f,
				upscaling.settings.flatAct2JitterMode == 0 ? upscaling.jitter.y * inputHeight / sourceGuideHeight : 0.0f,
				upscaling.foveatedRender.settings.flatAct2Sharpness,
				forceReset);
			if (succeeded && !act2FsrLogged) {
				logger::info("[ACT2] FSR3 after NR active input={}x{} output={}x{} dedicatedContext=true",
					inputWidth, inputHeight, outputWidth, outputHeight);
				act2FsrLogged = true;
			}
			if (succeeded)
				++act2BackendStats.fsrSucceeded;
			return succeeded;
		}

		Tuning GetTuning(const FoveatedRender::Settings& settings)
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

		Tuning GetAct3Tuning(const FoveatedRender::Settings& settings)
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

		struct SixPassIsolated
		{
			ID3D11Resource* colorR = nullptr;
			ID3D11ShaderResourceView* colorRSRV = nullptr;
			ID3D11Resource* depthR = nullptr;
			ID3D11ShaderResourceView* depthRSRV = nullptr;
			ID3D11Resource* motionR = nullptr;
			ID3D11ShaderResourceView* motionRSRV = nullptr;
			ID3D11Resource* reactiveR = nullptr;
			ID3D11Resource* transparencyR = nullptr;
			ID3D11Resource* destD = nullptr;
			sl::ViewportHandle act1Viewport{ 2 };
			sl::ViewportHandle act2Viewport{ 3 };
			uint32_t eyeIndex = 0;
			float nrMotionScaleX = 0.0f;
			float nrMotionScaleY = 0.0f;
			float act3MotionScaleX = 0.0f;
			float act3MotionScaleY = 0.0f;
		};

		bool RunSixPassIsolated(Upscaling& upscaling, FoveatedRender& foveated,
			const FoveatedRender::FlatPipelinePlan& plan, ID3D11DeviceContext* context,
			ID3D11Resource* formatSource, const SixPassIsolated& in, bool planChanged,
			uint32_t frame)
		{
			using Act1 = FoveatedRender::FlatAct1Backend;
			using Act2 = FoveatedRender::FlatAct2SrBackend;
			if (!formatSource || !in.colorR || !in.colorRSRV || !in.depthR || !in.depthRSRV ||
				!in.motionR || !in.motionRSRV || !in.destD)
				return false;
			if (!EnsureFlatPipelineResources(formatSource, plan))
				return false;

			ID3D11ShaderResourceView* fallbackSRV = nullptr;
			ID3D11Resource* fallbackResource = nullptr;
			uint32_t fallbackWidth = plan.displayWidth;
			uint32_t fallbackHeight = plan.displayHeight;
			bool outputCommitted = false;
			const SKSE::stl::scope_exit resolveFallback([&]() noexcept {
				if (outputCommitted || !fallbackResource)
					return;
				if (fallbackWidth == plan.displayWidth && fallbackHeight == plan.displayHeight) {
					context->CopyResource(in.destD, fallbackResource);
				} else if (fallbackSRV &&
					EnsurePipelineTexture(upscaledNeuralColor, formatSource, plan.displayWidth, plan.displayHeight,
						"NeuralRendering::FallbackOutput") &&
					ResampleLanczos(context, fallbackSRV, upscaledNeuralColor->uav.get(),
						fallbackWidth, fallbackHeight, plan.displayWidth, plan.displayHeight)) {
					context->CopyResource(in.destD, upscaledNeuralColor->resource.get());
				}
			});

			ID3D11ShaderResourceView* act1SourceSRV = in.colorRSRV;
			ID3D11Resource* act1SourceResource = in.colorR;
			uint32_t act1SourceWidth = plan.engineWidth;
			uint32_t act1SourceHeight = plan.engineHeight;
			bool succeeded = true;

			if (plan.act1Backend != Act1::kBypass) {
				ScopedGpuPass act1Pass("FlatPipeline::ACT1");
				auto& streamline = Upscaling::streamline;
				succeeded = EnsurePipelineTexture(flatAct1Output, formatSource, plan.act1OutputWidth, plan.act1OutputHeight,
					"NeuralRendering::FlatPipelineAct1Output");
				if (!succeeded)
					return false;

				if (plan.act1Backend == Act1::kDLSS) {
					const sl::Extent inputExtent{ 0, 0, plan.engineWidth, plan.engineHeight };
					const sl::Extent outputExtent{ 0, 0, plan.act1OutputWidth, plan.act1OutputHeight };
					succeeded = streamline.EvaluateDLSS(in.act1Viewport, in.eyeIndex,
						in.colorR, flatAct1Output->resource.get(),
						in.depthR, in.motionR, in.reactiveR, in.transparencyR,
						inputExtent, outputExtent, plan.act1OutputWidth, plan.act1OutputHeight,
						Streamline::DLSSModeForQualityMode(plan.act1Quality), false, planChanged,
						foveated.settings.flatPipelineAct1Jitter == 1, 1.0f, 1.0f,
						plan.act1Preset, 0.0f);
				} else {
					succeeded = act1FsrScaler.Apply(globals::d3d::device, context,
						in.colorR, in.depthR, in.motionR, in.reactiveR,
						in.transparencyR, flatAct1Output->resource.get(),
						plan.engineWidth, plan.engineHeight, plan.act1OutputWidth, plan.act1OutputHeight,
						static_cast<float>(plan.engineWidth), static_cast<float>(plan.engineHeight),
						foveated.settings.flatPipelineAct1Jitter == 0 ? upscaling.jitter.x : 0.0f,
						foveated.settings.flatPipelineAct1Jitter == 0 ? upscaling.jitter.y : 0.0f,
						0.0f, planChanged);
				}
				if (!succeeded)
					return false;
				act1SourceSRV = flatAct1Output->srv.get();
				act1SourceResource = flatAct1Output->resource.get();
				act1SourceWidth = plan.act1OutputWidth;
				act1SourceHeight = plan.act1OutputHeight;
				fallbackSRV = act1SourceSRV;
				fallbackResource = act1SourceResource;
				fallbackWidth = act1SourceWidth;
				fallbackHeight = act1SourceHeight;
			}

			if (!foveated.settings.neuralRenderingEnabled) {
				ID3D11Resource* finalResource = act1SourceResource;
				ID3D11ShaderResourceView* finalSRV = act1SourceSRV;
				if (act1SourceWidth != plan.displayWidth || act1SourceHeight != plan.displayHeight) {
					if (!EnsurePipelineTexture(upscaledNeuralColor, formatSource, plan.displayWidth, plan.displayHeight,
							"NeuralRendering::UpscaledNeuralColor") ||
						!ResampleSpatial(context, act1SourceSRV, upscaledNeuralColor->uav.get(),
							act1SourceWidth, act1SourceHeight, plan.displayWidth, plan.displayHeight,
							plan.act1Downsample == FoveatedRender::ResampleFilter::kTent ?
								FoveatedRender::FlatAct2SrBackend::kTent :
								FoveatedRender::FlatAct2SrBackend::kLanczos))
						return false;
					finalResource = upscaledNeuralColor->resource.get();
					finalSRV = upscaledNeuralColor->srv.get();
				}
				ID3D11Resource* act2Rc = finalResource;
				const bool act3OwnGuidance = plan.act3Guidance;
				auto applyRcas = [&]() {
					if (!foveated.settings.flatPipelineSharpen)
						return true;
					if (!EnsurePipelineTexture(flatSharpenOutput, formatSource, plan.displayWidth, plan.displayHeight,
							"NeuralRendering::FlatPipelineSharpenOutput"))
						return false;
					upscaling.rcas.Initialize();
					if (upscaling.rcas.ApplySharpen(finalSRV, flatSharpenOutput->uav.get(), foveated.settings.flatPipelineSharpness)) {
						finalResource = flatSharpenOutput->resource.get();
						finalSRV = flatSharpenOutput->srv.get();
					}
					return true;
				};
				if (!act3OwnGuidance && !applyRcas())
					return false;
				if (!ApplyAct3(upscaling, foveated, context, formatSource, plan, in.motionR, in.depthRSRV, in.motionRSRV,
						act1SourceSRV, act1SourceWidth, act1SourceHeight, act2Rc, finalResource, finalSRV, planChanged,
						in.eyeIndex, in.act3MotionScaleX, in.act3MotionScaleY))
					logger::warn("[ACT3] Center NR failed; keeping ACT2 output");
				if (act3OwnGuidance && !applyRcas())
					return false;
				context->CopyResource(in.destD, finalResource);
				outputCommitted = true;
				lastAppliedFrame = frame;
				return true;
			}

			if (act1SourceWidth == plan.nrWidth && act1SourceHeight == plan.nrHeight)
				context->CopyResource(halfResolutionColor->resource.get(), act1SourceResource);
			else if (!ResampleSpatial(context, act1SourceSRV, halfResolutionColor->uav.get(),
					act1SourceWidth, act1SourceHeight, plan.nrWidth, plan.nrHeight,
					plan.act1Downsample == FoveatedRender::ResampleFilter::kTent ?
						FoveatedRender::FlatAct2SrBackend::kTent :
						FoveatedRender::FlatAct2SrBackend::kLanczos))
				return false;
			if (!EnsureGuideResources(in.motionR, plan.nrWidth, plan.nrHeight) ||
				!ResizeGuides(context, in.depthRSRV, in.motionRSRV,
					plan.engineWidth, plan.engineHeight, plan.nrWidth, plan.nrHeight,
					GetMotionVectorMultiplier(foveated.settings.neuralRenderingMotionVectorMode),
					act1SourceWidth, act1SourceHeight,
					static_cast<std::uint32_t>(plan.act1DepthDownsample)))
				return false;

			ID3D11Resource* finalResource = nullptr;
			ID3D11ShaderResourceView* finalSRV = nullptr;
			// NR writes in place; Guidance fits its gain against the exact input NR saw.
			if ((plan.fusion || plan.act3Guidance) && nrInputColor)
				context->CopyResource(nrInputColor->resource.get(), halfResolutionColor->resource.get());
			const float nrMvecX = in.nrMotionScaleX > 0.0f ? in.nrMotionScaleX : static_cast<float>(plan.engineWidth);
			const float nrMvecY = in.nrMotionScaleY > 0.0f ? in.nrMotionScaleY : static_cast<float>(plan.engineHeight);
			succeeded = Renderer::Instance().Apply(globals::d3d::device, context, in.eyeIndex,
				halfResolutionColor->resource.get(), resizedDepthGuide->resource.get(), resizedDepthGuide->srv.get(),
				resizedMotionGuide->resource.get(), plan.nrWidth, plan.nrHeight, plan.nrWidth, plan.nrHeight,
				nrMvecX, nrMvecY,
				GetTuning(foveated.settings), planChanged);
			if (!succeeded)
				return false;
			fallbackSRV = halfResolutionColor->srv.get();
			fallbackResource = halfResolutionColor->resource.get();
			fallbackWidth = plan.nrWidth;
			fallbackHeight = plan.nrHeight;

			if (plan.nrWidth == plan.displayWidth && plan.nrHeight == plan.displayHeight) {
				finalResource = halfResolutionColor->resource.get();
				finalSRV = halfResolutionColor->srv.get();
			} else if (plan.act2SrBackend == Act2::kDLSS || plan.act2SrBackend == Act2::kFSR3) {
				ID3D11Resource* act2Reactive = nullptr;
				ID3D11Resource* act2Transparency = nullptr;
				if (!EnsurePipelineTexture(flatAct2Output, formatSource, plan.act2OutputWidth, plan.act2OutputHeight,
						"NeuralRendering::FlatPipelineAct2Output"))
					return false;
				if (in.reactiveR && in.transparencyR) {
					if (!EnsureAct2MaskResources(in.reactiveR, in.transparencyR, plan.nrWidth, plan.nrHeight))
						return false;
					act2Reactive = act2ReactiveMask->resource.get();
					act2Transparency = act2TransparencyMask->resource.get();
				}
				if (plan.act2SrBackend == Act2::kDLSS) {
					const sl::Extent inputExtent{ 0, 0, plan.nrWidth, plan.nrHeight };
					const sl::Extent outputExtent{ 0, 0, plan.act2OutputWidth, plan.act2OutputHeight };
					succeeded = Upscaling::streamline.EvaluateDLSS(in.act2Viewport, in.eyeIndex,
						halfResolutionColor->resource.get(), flatAct2Output->resource.get(),
						resizedDepthGuide->resource.get(), resizedMotionGuide->resource.get(),
						act2Reactive, act2Transparency,
						inputExtent, outputExtent, plan.act2OutputWidth, plan.act2OutputHeight,
						Streamline::DLSSModeForQualityMode(plan.act2Quality), false, planChanged,
						foveated.settings.flatPipelineAct2Jitter == 1,
						static_cast<float>(plan.nrWidth) / static_cast<float>(plan.engineWidth),
						static_cast<float>(plan.nrHeight) / static_cast<float>(plan.engineHeight),
						foveated.settings.flatPipelineAct2Preset, 0.0f);
				} else {
					succeeded = act2FsrScaler.Apply(globals::d3d::device, context,
						halfResolutionColor->resource.get(), resizedDepthGuide->resource.get(), resizedMotionGuide->resource.get(),
						act2Reactive, act2Transparency, flatAct2Output->resource.get(),
						plan.nrWidth, plan.nrHeight, plan.act2OutputWidth, plan.act2OutputHeight,
						static_cast<float>(plan.engineWidth), static_cast<float>(plan.engineHeight),
						foveated.settings.flatPipelineAct2Jitter == 0 ? upscaling.jitter.x * plan.nrWidth / plan.engineWidth : 0.0f,
						foveated.settings.flatPipelineAct2Jitter == 0 ? upscaling.jitter.y * plan.nrHeight / plan.engineHeight : 0.0f,
						0.0f, planChanged);
				}
				if (!succeeded)
					return false;
				if (plan.act2SrBackend == Act2::kDLSS)
					++act2BackendStats.dlssSucceeded;
				else
					++act2BackendStats.fsrSucceeded;
				if (plan.act2OutputWidth != plan.displayWidth || plan.act2OutputHeight != plan.displayHeight) {
					if (!ResampleLanczos(context, flatAct2Output->srv.get(), upscaledNeuralColor->uav.get(),
							plan.act2OutputWidth, plan.act2OutputHeight, plan.displayWidth, plan.displayHeight))
						return false;
					finalResource = upscaledNeuralColor->resource.get();
					finalSRV = upscaledNeuralColor->srv.get();
				} else {
					finalResource = flatAct2Output->resource.get();
					finalSRV = flatAct2Output->srv.get();
				}
			} else if (plan.act2SrBackend == Act2::kNIS) {
				bool usedNis = false;
				if (!UpscaleNeuralToDisplay(context, plan.nrWidth, plan.nrHeight,
						plan.displayWidth, plan.displayHeight, &usedNis))
					return false;
				if (usedNis)
					++act2BackendStats.nisSucceeded;
				finalResource = upscaledNeuralColor->resource.get();
				finalSRV = upscaledNeuralColor->srv.get();
			} else {
				if (plan.nrWidth == plan.displayWidth && plan.nrHeight == plan.displayHeight)
					context->CopyResource(upscaledNeuralColor->resource.get(), halfResolutionColor->resource.get());
				else if (!ResampleSpatial(context, halfResolutionColor->srv.get(), upscaledNeuralColor->uav.get(),
						plan.nrWidth, plan.nrHeight, plan.displayWidth, plan.displayHeight, plan.act2SrBackend))
					return false;
				finalResource = upscaledNeuralColor->resource.get();
				finalSRV = upscaledNeuralColor->srv.get();
			}

			ID3D11Resource* act2Rc = finalResource;
			if (plan.act3Guidance && !plan.fusion && !BuildGuideLowDisplay(context, plan))
				logger::warn("[ACT3] ACT2 Lo lift failed; crop Guidance will use the RC crop");
			if (plan.fusion) {
				if (!finalSRV || !nrInputColor || !guideLowDisplay ||
					!BuildGuideLowDisplay(context, plan) ||
					!FitGuidanceGain(context, nrInputColor->srv.get(), halfResolutionColor->srv.get(), in.depthRSRV,
						plan.nrWidth, plan.nrHeight, plan.engineWidth, plan.engineHeight, foveated.settings) ||
					!ResolveNeuralRendering(context, act1SourceSRV, finalSRV, guideLowDisplay->srv.get(), in.depthRSRV,
						plan.displayWidth, plan.displayHeight, act1SourceWidth, act1SourceHeight,
						plan.nrWidth, plan.nrHeight, plan.engineWidth, plan.engineHeight, foveated.settings)) {
					static bool fusionFailLogged = false;
					if (!fusionFailLogged) {
						fusionFailLogged = true;
						logger::warn("[FlatPipeline] Fusion failed; keeping ACT2/NR output");
					}
				} else {
				if (bandCaptureRequested) {
					bandCaptureRequested = false;
					using Kind = ExperimentCapture::BandKind;
					const ExperimentCapture::BandTexture textures[]{
						{ "G", act1SourceResource, Kind::kColor },
						{ "I", nrInputColor->resource.get(), Kind::kColor },
						{ "N", halfResolutionColor->resource.get(), Kind::kColor },
						{ "RC", finalResource, Kind::kColor },
						{ "Lo", guideLowDisplay->resource.get(), Kind::kColor },
						{ "Gain", guidanceGain->resource.get(), Kind::kGain },
						{ "EditLow", guidanceEditLow->resource.get(), Kind::kSigned },
						{ "D", fusionOutput->resource.get(), Kind::kColor },
						{ "DepthR", in.depthR, Kind::kDepth },
						{ "DepthN", resizedDepthGuide ? resizedDepthGuide->resource.get() : nullptr, Kind::kDepth },
					};
					ExperimentCapture::SaveBands(textures, plan.engineWidth, plan.engineHeight);
				}
				finalResource = fusionOutput->resource.get();
				finalSRV = fusionOutput->srv.get();
				}
			}

			const bool act3OwnGuidance = plan.act3Guidance;
			auto applyRcas = [&]() {
				if (!foveated.settings.flatPipelineSharpen)
					return true;
				if (!EnsurePipelineTexture(flatSharpenOutput, formatSource, plan.displayWidth, plan.displayHeight,
						"NeuralRendering::FlatPipelineSharpenOutput"))
					return false;
				upscaling.rcas.Initialize();
				if (upscaling.rcas.ApplySharpen(finalSRV, flatSharpenOutput->uav.get(), foveated.settings.flatPipelineSharpness)) {
					finalResource = flatSharpenOutput->resource.get();
					finalSRV = flatSharpenOutput->srv.get();
				}
				return true;
			};
			if (!act3OwnGuidance && !applyRcas())
				return false;
			if (!ApplyAct3(upscaling, foveated, context, formatSource, plan, in.motionR, in.depthRSRV, in.motionRSRV,
					act1SourceSRV, act1SourceWidth, act1SourceHeight, act2Rc, finalResource, finalSRV, planChanged,
					in.eyeIndex, in.act3MotionScaleX, in.act3MotionScaleY))
				logger::warn("[ACT3] Center NR failed; keeping ACT2 output");
			if (act3OwnGuidance && !applyRcas())
				return false;

			context->CopyResource(in.destD, finalResource);
			outputCommitted = true;
			lastAppliedFrame = frame;
			logger::debug("[FlatPipeline] frame={} eye={} D={}x{} R={}x{} A1={}x{} N={}x{} NRout={}x{} A2={}x{}",
				frame, in.eyeIndex, plan.displayWidth, plan.displayHeight, plan.engineWidth, plan.engineHeight,
				plan.act1OutputWidth, plan.act1OutputHeight, plan.nrWidth, plan.nrHeight,
				plan.nrOutputWidth, plan.nrOutputHeight, plan.act2OutputWidth, plan.act2OutputHeight);
			return true;
		}

		bool ApplyFlatPipeline(Upscaling& upscaling, FoveatedRender& foveated)
		{
			using Act1 = FoveatedRender::FlatAct1Backend;
			using Act2 = FoveatedRender::FlatAct2SrBackend;

			const bool hdrConfigured = globals::features::hdrDisplay.loaded && globals::features::hdrDisplay.settings.enableHDR;
			const bool routeActive = (foveated.settings.neuralRenderingEnabled ||
				foveated.settings.flatPipelineSharpen) &&
				upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS &&
				!upscaling.IsFrameGenerationConfiguredForSession() && !hdrConfigured;
			if (!routeActive) {
				if (flatRouteWasActive)
					Reset();
				return false;
			}
			const bool routeActivated = !flatRouteWasActive;
			flatRouteWasActive = true;
			CS_GPU_PASS("FlatPipeline::Total");

			auto* renderer = globals::game::renderer;
			auto* context = globals::d3d::context;
			if (!renderer || !context || !globals::d3d::device || !upscaling.motionVectorCopyTexture ||
				!upscaling.reactiveMaskTexture || !upscaling.transparencyCompositionMaskTexture)
				return false;

			winrt::com_ptr<ID3D11Texture2D> framebufferHolder;
			auto& framebufferTarget = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
			auto* framebuffer = ResolveRenderTargetTexture(framebufferTarget, framebufferHolder);
			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			auto& rawMotion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
			if (!framebuffer || !depth.depthSRV || !rawMotion.SRV)
				return false;

			const uint32_t frame = globals::state ? globals::state->frameCount : 0;
			if (lastAttemptedFrame == frame)
				return lastAppliedFrame == frame;
			lastAttemptedFrame = frame;

			D3D11_TEXTURE2D_DESC displayDesc{};
			framebuffer->GetDesc(&displayDesc);
			const auto plan = foveated.BuildFlatPipelinePlan(displayDesc.Width, displayDesc.Height);
			if (!plan.displayWidth || !plan.displayHeight || !plan.engineWidth || !plan.engineHeight ||
				!plan.nrWidth || !plan.nrHeight)
				return false;
			auto hashCombine = [](std::uint64_t& seed, std::uint64_t value) {
				seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2);
			};
			std::uint64_t planSignature = 0;
			for (const std::uint64_t value : {
				static_cast<std::uint64_t>(plan.engineWidth), static_cast<std::uint64_t>(plan.engineHeight),
				static_cast<std::uint64_t>(plan.act1OutputWidth), static_cast<std::uint64_t>(plan.act1OutputHeight),
				static_cast<std::uint64_t>(plan.nrWidth), static_cast<std::uint64_t>(plan.nrHeight),
				static_cast<std::uint64_t>(plan.nrOutputWidth), static_cast<std::uint64_t>(plan.nrOutputHeight),
				static_cast<std::uint64_t>(plan.act2OutputWidth), static_cast<std::uint64_t>(plan.act2OutputHeight),
				static_cast<std::uint64_t>(plan.act1Backend), static_cast<std::uint64_t>(plan.act2SrBackend),
				static_cast<std::uint64_t>(plan.fusion),
				static_cast<std::uint64_t>(plan.act1Downsample),
				static_cast<std::uint64_t>(plan.act1DepthDownsample),
				static_cast<std::uint64_t>(plan.act1Quality),
				static_cast<std::uint64_t>(plan.act2Quality),
				static_cast<std::uint64_t>(plan.act1Dsr),
				static_cast<std::uint64_t>(plan.act1Preset),
				static_cast<std::uint64_t>(foveated.settings.flatPipelineAct1Jitter),
				static_cast<std::uint64_t>(foveated.settings.flatPipelineAct2Preset),
				static_cast<std::uint64_t>(foveated.settings.flatPipelineAct2Jitter),
				static_cast<std::uint64_t>(foveated.settings.neuralRenderingStyle),
				static_cast<std::uint64_t>(foveated.settings.neuralRenderingAutoMask),
				static_cast<std::uint64_t>(std::bit_cast<uint32_t>(foveated.settings.neuralRenderingIntensity)),
				static_cast<std::uint64_t>(std::bit_cast<uint32_t>(foveated.settings.neuralRenderingLocalTone)),
				static_cast<std::uint64_t>(std::bit_cast<uint32_t>(foveated.settings.neuralRenderingLocalStructure)),
				static_cast<std::uint64_t>(std::bit_cast<uint32_t>(foveated.settings.neuralRenderingSkinStructure)),
				static_cast<std::uint64_t>(foveated.settings.act3Enabled),
				static_cast<std::uint64_t>(std::bit_cast<uint32_t>(foveated.settings.act3Scale)) })
				hashCombine(planSignature, value);
			const bool planChanged = routeActivated || lastFlatPlanSignature != planSignature;
			if (planChanged) {
				Renderer::Instance().ResetHistory();
				Upscaling::streamline.DestroyDLSSResources();
				act1FsrScaler.Reset();
				act2FsrScaler.Reset();
				if (plan.act2SrBackend != Act2::kNIS)
					nisScaler.Reset();
				lastFlatPlanSignature = planSignature;
				logger::info("[FlatPipeline] Plan signature=0x{:016X} D={}x{} R={}x{} ACT1={} {}x{} N={}x{} ({:.2f}{}) ACT2={} {}x{} RCAS={}",
					planSignature, plan.displayWidth, plan.displayHeight, plan.engineWidth, plan.engineHeight,
					static_cast<uint>(plan.act1Backend), plan.act1OutputWidth, plan.act1OutputHeight,
					plan.nrWidth, plan.nrHeight,
					foveated.settings.flatPipelineNrNative ? 1.0f : foveated.settings.flatPipelineNrScale,
					foveated.settings.flatPipelineNrNative ? " native" : "",
					static_cast<uint>(plan.act2SrBackend), plan.act2OutputWidth, plan.act2OutputHeight,
					foveated.settings.flatPipelineSharpen);
			}

			ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* savedDSV = nullptr;
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
			const SKSE::stl::scope_exit restoreRenderTargets([&]() noexcept {
				context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
				for (auto*& rtv : savedRTVs)
					if (rtv) rtv->Release();
				if (savedDSV) savedDSV->Release();
			});
			context->OMSetRenderTargets(0, nullptr, nullptr);

			ID3D11ShaderResourceView* postSourceSRV = framebufferTarget.SRV;
			ID3D11Resource* postSourceResource = framebuffer;
			if (!postSourceSRV) {
				if (!EnsureColorResources(framebuffer, plan.displayWidth, plan.displayHeight))
					return false;
			context->CopyResource(color[0]->resource.get(), framebuffer);
				postSourceSRV = color[0]->srv.get();
				postSourceResource = color[0]->resource.get();
			}

			auto& streamline = Upscaling::streamline;
			SixPassIsolated isolated{
				postSourceResource, postSourceSRV,
				depth.texture, depth.depthSRV,
				upscaling.motionVectorCopyTexture->resource.get(), upscaling.motionVectorCopyTexture->srv.get(),
				upscaling.reactiveMaskTexture->resource.get(),
				upscaling.transparencyCompositionMaskTexture->resource.get(),
				framebuffer, streamline.flatPostInjectViewport, streamline.flatAct2Viewport, 0
			};
			if (plan.act1Backend != Act1::kBypass) {
				if (!streamline.PrepareFlatInputIsolation(context, framebuffer, depth.depthSRV,
						upscaling.motionVectorCopyTexture->resource.get(), upscaling.reactiveMaskTexture->resource.get(),
						upscaling.transparencyCompositionMaskTexture->resource.get(), plan.engineWidth, plan.engineHeight))
					return false;
				isolated.colorR = streamline.flatIsolatedColor->resource.get();
				isolated.colorRSRV = streamline.flatIsolatedColor->srv.get();
				isolated.depthR = streamline.flatIsolatedDepth->resource.get();
				isolated.depthRSRV = streamline.flatIsolatedDepth->srv.get();
				isolated.motionR = streamline.flatIsolatedMotionVectors->resource.get();
				isolated.motionRSRV = streamline.flatIsolatedMotionVectors->srv.get();
				isolated.reactiveR = streamline.flatIsolatedReactiveMask->resource.get();
				isolated.transparencyR = streamline.flatIsolatedTransparencyMask->resource.get();
			}
			return RunSixPassIsolated(upscaling, foveated, plan, context, framebuffer, isolated, planChanged, frame);
		}

		bool IsVrPostUnorm8(const D3D11_TEXTURE2D_DESC& desc)
		{
			return desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
				desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
				desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
				desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
		}

		ID3D11Texture2D* ResolveVrPostTemp(RE::BSGraphics::Renderer* renderer)
		{
			if (!renderer)
				return nullptr;
			auto& targets = renderer->GetRuntimeData().renderTargets;
			const RE::RENDER_TARGET preferred[] = {
				RE::RENDER_TARGETS::kIMAGESPACE_TEMP_COPY,
				RE::RENDER_TARGETS::kIMAGESPACE_TEMP_COPY2,
			};
			for (const auto slot : preferred) {
				auto* tex = static_cast<ID3D11Texture2D*>(targets[slot].texture);
				if (!tex)
					continue;
				D3D11_TEXTURE2D_DESC desc{};
				tex->GetDesc(&desc);
				if (desc.Width < 2 || !desc.Height || !IsVrPostUnorm8(desc))
					continue;
				if (!vrSixPassTempLogged) {
					vrSixPassTempLogged = true;
					logger::info("[VrSixPass] kTEMP rt={} {}x{} fmt={} ({})", static_cast<int>(slot),
						desc.Width, desc.Height, static_cast<int>(desc.Format),
						desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? "RGBA8" :
						desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? "RGB10A2" : "other");
				}
				return tex;
			}
			return nullptr;
		}

		bool ResolveVrEngineEye(RE::BSGraphics::Renderer* renderer, uint32_t& eyeW, uint32_t& eyeH)
		{
			eyeW = eyeH = 0;
			auto* tex = ResolveVrPostTemp(renderer);
			if (!tex)
				return false;
			D3D11_TEXTURE2D_DESC desc{};
			tex->GetDesc(&desc);
			if (desc.Width < 2 || !desc.Height)
				return false;
			eyeW = desc.Width / 2;
			eyeH = desc.Height;
			return true;
		}

		bool WritebackVrEngineBox(ID3D11DeviceContext* context, ID3D11Resource* destD,
			ID3D11ShaderResourceView* destDSRV, Texture2D* destR,
			uint32_t destW, uint32_t destH, uint32_t writeW, uint32_t writeH,
			ID3D11Resource* engineSbs, uint32_t dstX, uint32_t dstY, bool featherToEngine)
		{
			CS_GPU_PASS("NeuralRendering::VrOwnedCompose");
			ID3D11Resource* src = destD;
			if (destW != writeW || destH != writeH) {
				if (!destR || !destR->uav || !destDSRV)
					return false;
				if (!ResampleSpatial(context, destDSRV, destR->uav.get(), destW, destH, writeW, writeH,
						FoveatedRender::FlatAct2SrBackend::kLanczos))
					return false;
				src = destR->resource.get();
			} else if (featherToEngine && destR && destR->uav) {
				// Hard box copy leaves a join against unreconstructed engine; Guidance makes it visible.
				FoveatedRenderImpl::Ops::BlendSubrectToOutput(
					destD, destR->resource.get(), destR->uav.get(),
					0, 0, writeW, writeH, 0,
					static_cast<uint32_t>(FoveatedRender::SubrectBlendMode::kFeather),
					globals::features::upscaling.foveatedRender.settings.subrectFeatherWidth,
					0.0f);
				src = destR->resource.get();
			}
			const D3D11_BOX box{ 0, 0, 0, writeW, writeH, 1 };
			context->CopySubresourceRegion(engineSbs, 0, dstX, dstY, 0, src, 0, &box);
			return true;
		}

		// VR cannot shrink engine R. SRSS that only grows G will push G/R
		// out of the selected NGX mode (Ultra Perf wants ~3x, guidance is <=2x).
		void ReconcileVrAct1DlssPlan(FoveatedRender::FlatPipelinePlan& plan)
		{
			using Act1 = FoveatedRender::FlatAct1Backend;
			if (plan.act1Backend != Act1::kDLSS || !plan.engineWidth || !plan.engineHeight ||
				!plan.act1OutputWidth || !plan.act1OutputHeight)
				return;

			auto& streamline = Upscaling::streamline;
			const uint32_t renderW = plan.engineWidth;
			const uint32_t renderH = plan.engineHeight;
			const uint32_t guidanceW = plan.act1OutputWidth;
			const uint32_t guidanceH = plan.act1OutputHeight;
			const float actualRatio = static_cast<float>(guidanceW) / static_cast<float>(renderW);

			auto fits = [&](uint32_t quality, uint32_t inW, uint32_t inH, uint32_t& minW, uint32_t& minH,
							uint32_t& maxW, uint32_t& maxH) {
				if (!streamline.QueryDLSSRenderRange(quality, guidanceW, guidanceH, minW, minH, maxW, maxH))
					return std::abs(Upscaling::GetQualityModeRatio(quality) - actualRatio) <= 0.2f;
				return inW >= minW && inW <= maxW && inH >= minH && inH <= maxH;
			};

			uint32_t evalW = renderW;
			uint32_t evalH = renderH;
			uint32_t minW = 0, minH = 0, maxW = 0, maxH = 0;
			auto accept = [&](uint32_t quality) {
				if (fits(quality, evalW, evalH, minW, minH, maxW, maxH))
					return true;
				const uint32_t evenW = evalW & ~1u;
				const uint32_t evenH = evalH & ~1u;
				if (evenW && evenH && (evenW != evalW || evenH != evalH) &&
					fits(quality, evenW, evenH, minW, minH, maxW, maxH)) {
					evalW = evenW;
					evalH = evenH;
					return true;
				}
				return false;
			};

			if (!accept(plan.act1Quality)) {
				uint32_t best = UINT32_MAX;
				float bestDistance = 1.0e9f;
				uint32_t bestEvalW = evalW;
				uint32_t bestEvalH = evalH;
				for (uint32_t quality = 0; quality <= 4; ++quality) {
					evalW = renderW;
					evalH = renderH;
					if (!accept(quality))
						continue;
					const float distance = std::abs(Upscaling::GetQualityModeRatio(quality) - actualRatio);
					if (distance < bestDistance) {
						bestDistance = distance;
						best = quality;
						bestEvalW = evalW;
						bestEvalH = evalH;
					}
				}
				if (best != UINT32_MAX) {
					logger::info("[VrSixPass] ACT1 mode {} -> {} so R={}x{} stays in NGX range for G={}x{} (G/R={:.3f})",
						plan.act1Quality, best, renderW, renderH, guidanceW, guidanceH, actualRatio);
					plan.act1Quality = best;
					evalW = bestEvalW;
					evalH = bestEvalH;
				} else {
					logger::warn("[VrSixPass] ACT1 R={}x{} out of NGX range for G={}x{} mode={}; falling back to DLAA {}x{}",
						renderW, renderH, guidanceW, guidanceH, plan.act1Quality, plan.displayWidth, plan.displayHeight);
					plan.srss = false;
					plan.act1Quality = 0;
					plan.act1OutputWidth = plan.displayWidth;
					plan.act1OutputHeight = plan.displayHeight;
					plan.guidanceG = { plan.displayWidth, plan.displayHeight };
					plan.nrWidth = std::min(plan.act1OutputWidth, plan.nrWidth);
					plan.nrHeight = std::min(plan.act1OutputHeight, plan.nrHeight);
					plan.nrOutputWidth = plan.nrWidth;
					plan.nrOutputHeight = plan.nrHeight;
					plan.neuralN = { plan.nrWidth, plan.nrHeight };
					evalW = renderW;
					evalH = renderH;
				}
			}

			if (evalW != plan.engineWidth || evalH != plan.engineHeight) {
				static uint32_t lastLoggedW = 0, lastLoggedH = 0, lastEvalW = 0, lastEvalH = 0;
				if (lastLoggedW != plan.engineWidth || lastLoggedH != plan.engineHeight ||
					lastEvalW != evalW || lastEvalH != evalH) {
					logger::info("[VrSixPass] ACT1 evaluate extent {}x{} -> even {}x{} for NGX range",
						plan.engineWidth, plan.engineHeight, evalW, evalH);
					lastLoggedW = plan.engineWidth;
					lastLoggedH = plan.engineHeight;
					lastEvalW = evalW;
					lastEvalH = evalH;
				}
				plan.engineWidth = evalW;
				plan.engineHeight = evalH;
				plan.renderR = { evalW, evalH };
			}
		}

		bool ApplyVrSixPass(Upscaling& upscaling, FoveatedRender& foveated)
		{
			using Act2 = FoveatedRender::FlatAct2SrBackend;

			if (!upscaling.IsVrDlssPostInjectActive())
				return false;

			CS_GPU_PASS("NeuralRendering::VrOwnedSubmit");
			auto* renderer = globals::game::renderer;
			auto* context = globals::d3d::context;
			if (!renderer || !context || !globals::d3d::device)
				return false;

			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			auto& motionRT = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
			if (!depth.texture || !depth.depthSRV || !motionRT.texture)
				return false;

			const auto& leftUV = foveated.subrectController.GetUV();
			const auto& rightUV = foveated.subrectController.GetRightEyeUV();
			if (leftUV.w != rightUV.w || leftUV.h != rightUV.h)
				return false;

			auto* postTemp = ResolveVrPostTemp(renderer);
			uint32_t engineW = 0;
			uint32_t engineH = 0;
			if (!postTemp || !ResolveVrEngineEye(renderer, engineW, engineH))
				return false;

			const uint32_t displayW = engineW;
			const uint32_t displayH = engineH;
			const uint32_t subInW = std::max(1u, static_cast<uint32_t>(engineW * leftUV.w));
			const uint32_t subInH = std::max(1u, static_cast<uint32_t>(engineH * leftUV.h));
			const uint32_t subOutW = std::max(1u, static_cast<uint32_t>(displayW * leftUV.w));
			const uint32_t subOutH = std::max(1u, static_cast<uint32_t>(displayH * leftUV.h));
			const bool centerOnly = !leftUV.IsFullEye() || !rightUV.IsFullEye();
			auto plan = foveated.BuildFlatPipelinePlan(subOutW, subOutH, subInW, subInH);
			ReconcileVrAct1DlssPlan(plan);
			if (!plan.displayWidth || !plan.displayHeight || !plan.engineWidth || !plan.engineHeight ||
				!plan.nrWidth || !plan.nrHeight)
				return false;

			const uint32_t frame = globals::state ? globals::state->frameCount : 0;
			if (vrSixPassDoneFrame == frame)
				return true;
#ifdef ENABLE_SKYRIM_VR
			VrSubmitDlssg::NoteOwnedFrame(UINT32_MAX);
#endif

			const bool routeActivated = !vrSixPassWasActive;
			vrSixPassWasActive = true;
			flatRouteWasActive = true;
			auto hashCombine = [](std::uint64_t& seed, std::uint64_t value) {
				seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2);
			};
			std::uint64_t planSignature = centerOnly ? 4ull : 5ull;
			for (const std::uint64_t value : {
				static_cast<std::uint64_t>(plan.engineWidth), static_cast<std::uint64_t>(plan.engineHeight),
				static_cast<std::uint64_t>(plan.act1OutputWidth), static_cast<std::uint64_t>(plan.act1OutputHeight),
				static_cast<std::uint64_t>(plan.nrWidth), static_cast<std::uint64_t>(plan.nrHeight),
				static_cast<std::uint64_t>(plan.nrOutputWidth), static_cast<std::uint64_t>(plan.nrOutputHeight),
				static_cast<std::uint64_t>(plan.act2OutputWidth), static_cast<std::uint64_t>(plan.act2OutputHeight),
				static_cast<std::uint64_t>(plan.act1Backend), static_cast<std::uint64_t>(plan.act2SrBackend),
				static_cast<std::uint64_t>(plan.fusion),
				static_cast<std::uint64_t>(plan.act1Downsample),
				static_cast<std::uint64_t>(plan.act1DepthDownsample),
				static_cast<std::uint64_t>(plan.act1Quality),
				static_cast<std::uint64_t>(plan.act2Quality),
				static_cast<std::uint64_t>(plan.act1Dsr),
				static_cast<std::uint64_t>(plan.act1Preset),
				static_cast<std::uint64_t>(foveated.settings.flatPipelineAct1Jitter),
				static_cast<std::uint64_t>(foveated.settings.flatPipelineAct2Preset),
				static_cast<std::uint64_t>(foveated.settings.flatPipelineAct2Jitter),
				static_cast<std::uint64_t>(foveated.settings.neuralRenderingStyle),
				static_cast<std::uint64_t>(foveated.settings.neuralRenderingAutoMask),
				static_cast<std::uint64_t>(std::bit_cast<uint32_t>(foveated.settings.neuralRenderingIntensity)),
				static_cast<std::uint64_t>(subInW),
				static_cast<std::uint64_t>(subInH),
				static_cast<std::uint64_t>(subOutW),
				static_cast<std::uint64_t>(subOutH) })
				hashCombine(planSignature, value);
			const bool planChanged = routeActivated || lastFlatPlanSignature != planSignature;
			// Engine MV is per-eye UV. The R crop keeps those values; ResizeGuides does
			// not rescale them. Feature 18 does pixels = mvec * scale on the N texture.
			const float nrMotionScaleX = static_cast<float>(engineW) *
				static_cast<float>(plan.nrWidth) / static_cast<float>(subInW);
			const float nrMotionScaleY = static_cast<float>(engineH) *
				static_cast<float>(plan.nrHeight) / static_cast<float>(subInH);
			const float act3MotionScaleX = static_cast<float>(engineW);
			const float act3MotionScaleY = static_cast<float>(engineH);
			if (planChanged) {
				Renderer::Instance().ResetHistory();
				Upscaling::streamline.DestroyDLSSResources();
				act1FsrScaler.Reset();
				act2FsrScaler.Reset();
				if (plan.act2SrBackend != Act2::kNIS)
					nisScaler.Reset();
				lastFlatPlanSignature = planSignature;
				logger::info("[VrSixPass] Owned SBS plan=0x{:016X} center={} E={}x{} R={}x{} evalR={}x{} D={}x{} ACT1={}x{} N={}x{} act1Q={} srss={} fusion={} compare={} G/R={:.3f} nrMvec={:.2f}x{:.2f}",
					planSignature, centerOnly ? 1 : 0, engineW, engineH, subInW, subInH,
					plan.engineWidth, plan.engineHeight, subOutW, subOutH,
					plan.act1OutputWidth, plan.act1OutputHeight, plan.nrWidth, plan.nrHeight,
					plan.act1Quality, plan.srss ? 1 : 0, plan.fusion ? 1 : 0,
					foveated.settings.fusionCompareMode,
					static_cast<float>(plan.act1OutputWidth) / static_cast<float>(plan.engineWidth),
					nrMotionScaleX, nrMotionScaleY);
			}

			ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView* savedDSV = nullptr;
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
			const SKSE::stl::scope_exit restoreRenderTargets([&]() noexcept {
				context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
				for (auto*& rtv : savedRTVs)
					if (rtv) rtv->Release();
				if (savedDSV) savedDSV->Release();
			});
			context->OMSetRenderTargets(0, nullptr, nullptr);

			FoveatedRenderImpl::Ops::EnsureVRSubrectTextures(subInW, subInH, subOutW, subOutH,
				postTemp, motionRT.texture, nullptr, nullptr);

			const Util::Subrect::UVRegion* eyeUVs[2]{ &leftUV, &rightUV };
			auto& streamline = Upscaling::streamline;
			FoveatedRenderImpl::Bridge::foveatedEvaluating = true;
			const SKSE::stl::scope_exit clearFoveatedFlag([]() noexcept {
				FoveatedRenderImpl::Bridge::foveatedEvaluating = false;
			});
			bool eyeReady[2]{ false, false };
			for (uint32_t eye = 0; eye < 2; ++eye) {
				const auto& uv = *eyeUVs[eye];
				const uint32_t cropX = static_cast<uint32_t>(uv.x * engineW);
				const uint32_t cropY = static_cast<uint32_t>(uv.y * engineH);
				const uint32_t sbsX = (eye == 1 ? engineW : 0) + cropX;
				const D3D11_BOX sbsCrop{ sbsX, cropY, 0, sbsX + subInW, cropY + subInH, 1 };
				auto* colorIn = FoveatedRenderImpl::Core::vrSubrectColorIn[eye].get();
				auto* depthIn = FoveatedRenderImpl::Core::vrSubrectDepth[eye].get();
				auto* dest = FoveatedRenderImpl::Core::vrSubrectColorOut[eye].get();
				auto* motion = FoveatedRenderImpl::Core::vrSubrectMotionVectors[eye].get();
				if (!colorIn || !colorIn->srv || !colorIn->uav || !depthIn || !depthIn->srv || !dest ||
					!dest->srv || !motion || !motion->srv)
					return false;
				context->CopySubresourceRegion(colorIn->resource.get(), 0, 0, 0, 0, postTemp, 0, &sbsCrop);
				if (!FoveatedRenderImpl::Ops::CopyDepthSubrect(depth.depthSRV, depthIn->uav.get(),
						sbsX, cropY, 0, 0, subInW, subInH))
					return false;
				context->CopySubresourceRegion(motion->resource.get(), 0, 0, 0, 0, motionRT.texture, 0, &sbsCrop);
				if (cropY == 0 && subInH == engineH)
					upscaling.ClearHMDMask(colorIn->uav.get(), depth.depthSRV, subInW, subInH, sbsX, 0);

				const SixPassIsolated isolated{
					colorIn->resource.get(), colorIn->srv.get(),
					depthIn->resource.get(), depthIn->srv.get(),
					motion->resource.get(), motion->srv.get(),
					nullptr, nullptr,
					dest->resource.get(),
					eye == 0 ? streamline.viewport : streamline.viewportRight,
					eye == 0 ? streamline.flatPostInjectViewport : streamline.flatAct2Viewport,
					eye,
					nrMotionScaleX,
					nrMotionScaleY,
					act3MotionScaleX,
					act3MotionScaleY
				};
				if (!RunSixPassIsolated(upscaling, foveated, plan, context, postTemp, isolated, planChanged, frame)) {
					static bool isolatedFailLogged[2]{};
					if (!isolatedFailLogged[eye]) {
						isolatedFailLogged[eye] = true;
						logger::warn("[VrSixPass] Isolated pass failed eye={}; leaving engine Submit", eye);
					}
					continue;
				}
				if (planChanged)
					continue;

				if (!WritebackVrEngineBox(context, dest->resource.get(), dest->srv.get(), colorIn,
						subOutW, subOutH, subInW, subInH, postTemp, sbsX, cropY, centerOnly)) {
					static bool composeFailLogged[2]{};
					if (!composeFailLogged[eye]) {
						composeFailLogged[eye] = true;
						logger::warn("[VrSixPass] kTEMP writeback failed eye={}", eye);
					}
					continue;
				}
				eyeReady[eye] = true;
			}

			vrSixPassDoneFrame = frame;
			if (!vrSixPassLogged) {
				D3D11_TEXTURE2D_DESC postDesc{};
				postTemp->GetDesc(&postDesc);
				logger::info("[VrSixPass] Tonemap wrote kTEMP {}x{} E={}x{} cropR={}x{} cropD={}x{}",
					postDesc.Width, postDesc.Height, engineW, engineH, subInW, subInH, subOutW, subOutH);
				vrSixPassLogged = true;
			}
#ifdef ENABLE_SKYRIM_VR
			if (VrAriseTail::IsActive())
				VrAriseTail::PublishKTemp(postTemp);
			VrSubmitDlssg::RunSubmitTest(postTemp, upscaling.settings.vrSubmitTest);
#endif
			return true;
		}

		bool CaptureModel3Tail(Upscaling& upscaling)
		{
			using Act1 = FoveatedRender::FlatAct1Backend;
			using Act2 = FoveatedRender::FlatAct2SrBackend;
			auto* renderer = globals::game::renderer;
			auto* context = globals::d3d::context;
			if (!renderer || !context || !globals::state) {
				if (!model3CaptureFailureLogged)
					logger::error("[ARISE] Tail capture unavailable: renderer/context/state is missing");
				model3CaptureFailureLogged = true;
				return false;
			}
			const std::uint64_t frameId = globals::state->frameCount;
			if (lastModel3CaptureFrame == frameId)
				return true;
			lastModel3CaptureFrame = frameId;
			if (!globals::state->worldRenderedThisFrame || globals::state->isMainMenuOpen ||
				globals::state->isLoadingMenuOpen) {
				upscaling.dx12SwapChain.ClearAriseSceneCapture();
				lastModel3Eligible = false;
				lastModel3Frame = UINT64_MAX;
				return true;
			}
			auto& framebuffer = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			auto& rawMotion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
			winrt::com_ptr<ID3D11Texture2D> framebufferHolder;
			auto* framebufferTexture = ResolveRenderTargetTexture(framebuffer, framebufferHolder);
			ID3D11Resource* motionResource = upscaling.motionVectorCopyTexture &&
				upscaling.motionVectorCopyTexture->resource ? upscaling.motionVectorCopyTexture->resource.get() : rawMotion.texture;
			ID3D11ShaderResourceView* motionSRV = upscaling.motionVectorCopyTexture &&
				upscaling.motionVectorCopyTexture->srv ? upscaling.motionVectorCopyTexture->srv.get() : rawMotion.SRV;
			winrt::com_ptr<ID3D11Texture2D> motionTexture;
			if (!framebufferTexture || !depth.depthSRV || !motionResource || !motionSRV ||
				FAILED(motionResource->QueryInterface(IID_PPV_ARGS(motionTexture.put())))) {
				if (!model3CaptureFailureLogged)
					logger::error("[ARISE] Tail capture unavailable: framebuffer={} depth={} motion={} motionSRV={}",
						framebufferTexture != nullptr, depth.depthSRV != nullptr, motionResource != nullptr, motionSRV != nullptr);
				model3CaptureFailureLogged = true;
				return false;
			}
			D3D11_TEXTURE2D_DESC sceneDesc{};
			D3D11_TEXTURE2D_DESC motionDesc{};
			framebufferTexture->GetDesc(&sceneDesc);
			motionTexture->GetDesc(&motionDesc);
			const std::uint32_t displayWidth = upscaling.dx12SwapChain.PhysicalWidth();
			const std::uint32_t displayHeight = upscaling.dx12SwapChain.PhysicalHeight();
			const std::uint32_t sourceGuideWidth = upscaling.dx12SwapChain.LogicalWidth();
			const std::uint32_t sourceGuideHeight = upscaling.dx12SwapChain.LogicalHeight();
			if (sceneDesc.Width != sourceGuideWidth || sceneDesc.Height != sourceGuideHeight) {
				if (!model3CaptureFailureLogged)
					logger::error("[ARISE] Tail capture rejected: framebuffer={}x{} logical R={}x{}",
						sceneDesc.Width, sceneDesc.Height, sourceGuideWidth, sourceGuideHeight);
				model3CaptureFailureLogged = true;
				return false;
			}
			auto plan = upscaling.foveatedRender.BuildFlatPipelinePlan(
				displayWidth, displayHeight, sourceGuideWidth, sourceGuideHeight);
			const bool requested = upscaling.foveatedRender.settings.neuralRenderingEnabled;
			const bool unsupportedProvider =
				(plan.act1Backend != Act1::kBypass && plan.act1Backend != Act1::kDLSS) ||
				(requested && !plan.fusion && plan.act2SrBackend != Act2::kDLSS && plan.act2SrBackend != Act2::kLanczos &&
					plan.act2SrBackend != Act2::kTent && plan.act2SrBackend != Act2::kMitchell &&
					plan.act2SrBackend != Act2::kEasu);
			const bool eligible = !unsupportedProvider;
			if (requested && unsupportedProvider && !model3RestrictionLogged) {
				logger::warn("[ARISE] Tail provider unsupported for ACT1={} ACT2={}; Lanczos/passthrough fallback active",
					static_cast<uint>(plan.act1Backend), static_cast<uint>(plan.act2SrBackend));
				model3RestrictionLogged = true;
			} else if (!requested || !unsupportedProvider) {
				model3RestrictionLogged = false;
			}
			const bool becameEligible = eligible && !lastModel3Eligible;
			lastModel3Eligible = false;
			if (!eligible)
				lastModel3Frame = UINT64_MAX;
			const bool guidesReady = EnsureGuideResources(motionResource,
				sourceGuideWidth, sourceGuideHeight) &&
				ResizeGuides(context, depth.depthSRV, motionSRV,
					sourceGuideWidth, sourceGuideHeight, sourceGuideWidth, sourceGuideHeight, 1.0f,
					sourceGuideWidth, sourceGuideHeight, 0);
			if (!guidesReady) {
				if (!model3CaptureFailureLogged)
					logger::error("[ARISE] Tail capture unavailable: R guide preparation failed ({}x{})",
						sourceGuideWidth, sourceGuideHeight);
				model3CaptureFailureLogged = true;
				return false;
			}
			model3CaptureFailureLogged = false;
			auto hashCombine = [](std::uint64_t& seed, std::uint64_t value) {
				seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2);
			};
			std::uint64_t planSignature = 0;
			for (const std::uint64_t value : {
				static_cast<std::uint64_t>(sceneDesc.Width), static_cast<std::uint64_t>(sceneDesc.Height),
				static_cast<std::uint64_t>(sceneDesc.Format), static_cast<std::uint64_t>(motionDesc.Format),
				static_cast<std::uint64_t>(sourceGuideWidth), static_cast<std::uint64_t>(sourceGuideHeight),
				static_cast<std::uint64_t>(plan.engineWidth), static_cast<std::uint64_t>(plan.engineHeight),
				static_cast<std::uint64_t>(plan.displayWidth), static_cast<std::uint64_t>(plan.displayHeight),
				static_cast<std::uint64_t>(plan.act1OutputWidth), static_cast<std::uint64_t>(plan.act1OutputHeight),
				static_cast<std::uint64_t>(plan.nrWidth), static_cast<std::uint64_t>(plan.nrHeight),
				static_cast<std::uint64_t>(plan.act1Backend), static_cast<std::uint64_t>(plan.act2SrBackend),
				static_cast<std::uint64_t>(plan.fusion),
				static_cast<std::uint64_t>(plan.act1Downsample),
				static_cast<std::uint64_t>(plan.act1DepthDownsample),
				static_cast<std::uint64_t>(plan.act1Dsr),
				static_cast<std::uint64_t>(plan.act1Quality),
				static_cast<std::uint64_t>(plan.act1Preset),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.flatPipelineAct1Jitter),
				static_cast<std::uint64_t>(plan.act2Quality),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.flatPipelineAct2Preset),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.flatPipelineAct2Jitter),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.neuralRenderingEnabled),
				static_cast<std::uint64_t>(eligible),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.flatPipelineSharpen),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.flatPipelineSharpness)),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.flatPipelineFusionDirection),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.flatPipelineFusionDenoise),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.flatPipelineFusionEnhancedGuidance),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.flatPipelineFusionDenoiseStrength)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.flatPipelineFusionSoftness)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.flatPipelineFusionFrequencyGuidance)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.flatPipelineFusionDepthGuidance)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.flatPipelineFusionColorGuidance)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.neuralRenderingFusionLuma)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.neuralRenderingFusionChroma)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.neuralRenderingFusionEdgeProtection)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.neuralRenderingFusionResidualLimit)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionDepthDilate)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionGEdge)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionNEdge)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionEdgePower)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionHfLumaSuppress)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionHfTowardN)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionNcDilate)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionAlignG)),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.fusionCompareMode),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.fusionDebugView),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionToneGainLuma)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionToneGainChroma)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionDetailGain)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionDither)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionGainPrior)),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.fusionWedge),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.fusionWedgeConstCount),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionEdgeBlockGThresh)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionFitSideSigma)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionDirectionalSharpen)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.fusionSoftEdge)),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.neuralRenderingStyle),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.neuralRenderingAutoMask),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.neuralRenderingIntensity)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.neuralRenderingLocalTone)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.neuralRenderingLocalStructure)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.neuralRenderingSkinStructure)),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.act3Enabled),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.act3Scale)),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.act3Sharpen) })
				hashCombine(planSignature, value);
			const bool firstPlan = !hasModel3Plan;
			const bool planChanged = !firstPlan && planSignature != lastModel3PlanSignature;
			if (planChanged && !upscaling.dx12SwapChain.PrepareAriseGraphPlanChange()) {
				lastModel3Eligible = false;
				lastModel3Frame = UINT64_MAX;
				return false;
			}
			const bool reset = upscaling.foveatedRender.settings.neuralRenderingResetEveryFrame ||
				becameEligible ||
				lastModel3Frame == UINT64_MAX || frameId != lastModel3Frame + 1 ||
				planChanged;
			lastModel3Frame = eligible ? frameId : UINT64_MAX;
			lastModel3PlanSignature = planSignature;
			hasModel3Plan = true;
			lastModel3Eligible = eligible;
			sl::Constants cameraConstants{};
			cameraConstants.cameraAspectRatio = static_cast<float>(sceneDesc.Width) / sceneDesc.Height;
			cameraConstants.cameraFOV = Util::GetVerticalFOVRad();
			cameraConstants.cameraNear = *globals::game::cameraNear;
			cameraConstants.cameraFar = *globals::game::cameraFar;
			auto viewMatrix = globals::game::frameBufferCached.GetCameraViewInverse(0).Transpose();
			auto cameraViewToClip = globals::game::frameBufferCached.GetCameraProjUnjittered(0).Transpose();
			cameraConstants.cameraMotionIncluded = sl::Boolean::eTrue;
			cameraConstants.cameraPinholeOffset = { 0.0f, 0.0f };
			cameraConstants.cameraRight = { viewMatrix._11, viewMatrix._12, viewMatrix._13 };
			cameraConstants.cameraUp = { viewMatrix._21, viewMatrix._22, viewMatrix._23 };
			cameraConstants.cameraFwd = { viewMatrix._31, viewMatrix._32, viewMatrix._33 };
			cameraConstants.cameraPos = *(sl::float3*)&globals::game::frameBufferCached.GetCameraPosAdjust(0);
			cameraConstants.cameraViewToClip = *(sl::float4x4*)&cameraViewToClip;
			cameraConstants.depthInverted = sl::Boolean::eFalse;
			recalculateCameraMatrices(cameraConstants);
			cameraConstants.jitterOffset = { -upscaling.jitter.x, -upscaling.jitter.y };
			cameraConstants.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
			cameraConstants.mvecScale = { 1.0f, 1.0f };
			cameraConstants.motionVectors3D = sl::Boolean::eFalse;
			cameraConstants.motionVectorsInvalidValue = FLT_MIN;
			cameraConstants.orthographicProjection = sl::Boolean::eFalse;
			cameraConstants.motionVectorsDilated = sl::Boolean::eFalse;
			cameraConstants.motionVectorsJittered = sl::Boolean::eFalse;
			sl::Constants act1Constants = cameraConstants;
			if (upscaling.foveatedRender.settings.flatPipelineAct1Jitter != 0)
				act1Constants.jitterOffset = { 0.0f, 0.0f };
			sl::Constants act2Constants = cameraConstants;
			if (upscaling.foveatedRender.settings.flatPipelineAct2Jitter == 0) {
				act2Constants.jitterOffset.x *= static_cast<float>(plan.nrWidth) / sourceGuideWidth;
				act2Constants.jitterOffset.y *= static_cast<float>(plan.nrHeight) / sourceGuideHeight;
			} else
				act2Constants.jitterOffset = { 0.0f, 0.0f };
			if (firstPlan || planChanged) {
				logger::info("[ARISE] Model3 plan scene R={}x{} display D={}x{} N={}x{} ratio=({:.4f},{:.4f}) DRS=locked "
					"ACT1={} {}x{}->{}x{} quality={} preset={} jitter=({:.4f},{:.4f}) mvec=(1,1) "
					"ACT2={} {}x{}->{}x{} jitter=({:.4f},{:.4f}) mvec=(1,1)",
					sourceGuideWidth, sourceGuideHeight, displayWidth, displayHeight, plan.nrWidth, plan.nrHeight,
					static_cast<float>(sourceGuideWidth) / displayWidth,
					static_cast<float>(sourceGuideHeight) / displayHeight,
					static_cast<uint>(plan.act1Backend), sourceGuideWidth, sourceGuideHeight,
					plan.act1OutputWidth, plan.act1OutputHeight, plan.act1Quality,
					plan.act1Preset,
					act1Constants.jitterOffset.x, act1Constants.jitterOffset.y,
					static_cast<uint>(plan.act2SrBackend), plan.nrWidth, plan.nrHeight,
					plan.displayWidth, plan.displayHeight, act2Constants.jitterOffset.x, act2Constants.jitterOffset.y);
			}
			if (upscaling.foveatedRender.ariseDimensionDiagnostics.load(std::memory_order_acquire) &&
				(firstPlan || planChanged || frameId % 120 == 0)) {
				auto textureExtent = [](ID3D11Texture2D* texture) {
					D3D11_TEXTURE2D_DESC desc{};
					if (texture)
						texture->GetDesc(&desc);
					return std::pair{ desc.Width, desc.Height };
				};
				const auto mainExtent = textureExtent(
					renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture);
				const auto totalExtent = textureExtent(
					renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTOTAL].texture);
				logger::info("[ARISE DIM] frame={} kMAIN={}x{} kFRAMEBUFFER={}x{} kTOTAL={}x{} frontR={}x{} physicalD={}x{} ACT1={}x{} N={}x{} ACT2={}x{} jitterRaw=({:.4f},{:.4f}) jitterACT1=({:.4f},{:.4f}) jitterACT2=({:.4f},{:.4f})",
					frameId, mainExtent.first, mainExtent.second, sceneDesc.Width, sceneDesc.Height,
					totalExtent.first, totalExtent.second, sourceGuideWidth, sourceGuideHeight,
					displayWidth, displayHeight, plan.act1OutputWidth, plan.act1OutputHeight,
					plan.nrWidth, plan.nrHeight, plan.act2OutputWidth, plan.act2OutputHeight,
					upscaling.jitter.x, upscaling.jitter.y,
					act1Constants.jitterOffset.x, act1Constants.jitterOffset.y,
					act2Constants.jitterOffset.x, act2Constants.jitterOffset.y);
			}
			const auto act3Crop = FoveatedRender::MakeAct3Crop(plan.displayWidth, plan.displayHeight,
				upscaling.foveatedRender.settings.act3Scale);
			upscaling.dx12SwapChain.CaptureAriseScene(
				resizedDepthGuide->resource.get(), resizedMotionGuide->resource.get(), {
					.frameId = frameId,
					.guideWidth = sourceGuideWidth,
					.guideHeight = sourceGuideHeight,
					.plan = {
						.frameId = frameId,
						.sceneInputWidth = sourceGuideWidth,
						.sceneInputHeight = sourceGuideHeight,
						.engineWidth = sourceGuideWidth,
						.engineHeight = sourceGuideHeight,
						.displayWidth = plan.displayWidth,
						.displayHeight = plan.displayHeight,
						.act1OutputWidth = plan.act1OutputWidth,
						.act1OutputHeight = plan.act1OutputHeight,
						.nrWidth = plan.nrWidth,
						.nrHeight = plan.nrHeight,
						.act1 = static_cast<AriseReconstructionGraph::Act1Backend>(plan.act1Backend),
						.act2Sr = static_cast<AriseReconstructionGraph::Act2SrBackend>(plan.act2SrBackend),
						.fusionEnabled = plan.fusion,
						.act1Downsample = static_cast<std::uint32_t>(plan.act1Downsample),
						.act1DepthDownsample = static_cast<std::uint32_t>(plan.act1DepthDownsample),
						.act1Quality = plan.act1Quality,
						.act1Preset = plan.act1Preset,
						.act2Quality = plan.act2Quality,
						.act2Preset = upscaling.foveatedRender.settings.flatPipelineAct2Preset,
						.act1Camera = { act1Constants },
						.act2Camera = { act2Constants },
						.fusion = {
							.detailLuma = upscaling.foveatedRender.settings.neuralRenderingFusionLuma,
							.detailChroma = upscaling.foveatedRender.settings.neuralRenderingFusionChroma,
							.rdEdgeFloor = upscaling.foveatedRender.settings.neuralRenderingFusionEdgeProtection,
							.regularization = upscaling.foveatedRender.settings.neuralRenderingFusionResidualLimit,
							.compareMode = upscaling.foveatedRender.settings.fusionCompareMode,
							.debugView = upscaling.foveatedRender.settings.fusionDebugView,
							.gainPrior = upscaling.foveatedRender.settings.fusionGainPrior,
							.fitWindow = upscaling.foveatedRender.settings.flatPipelineFusionSoftness,
							.maxGain = upscaling.foveatedRender.settings.flatPipelineFusionFrequencyGuidance,
							.depthGuidance = upscaling.foveatedRender.settings.flatPipelineFusionDepthGuidance,
							.depthDilate = upscaling.foveatedRender.settings.fusionDepthDilate,
							.detailGain = upscaling.foveatedRender.settings.fusionDetailGain,
							.toneGainLuma = upscaling.foveatedRender.settings.fusionToneGainLuma,
							.toneGainChroma = upscaling.foveatedRender.settings.fusionToneGainChroma,
							.ditherStrength = upscaling.foveatedRender.settings.fusionDither,
							.edgeBlockMode = 0u,
							.edgeBlockGThresh = upscaling.foveatedRender.settings.fusionEdgeBlockGThresh,
							.edgeBlockSkip = upscaling.foveatedRender.settings.fusionFitSideSigma,
							.edgeBlockDilateD = upscaling.foveatedRender.settings.fusionDirectionalSharpen,
							.edgeBlockMix = upscaling.foveatedRender.settings.fusionSoftEdge,
						},
						.act3 = {
							.enabled = upscaling.foveatedRender.settings.act3Enabled,
							.scale = upscaling.foveatedRender.settings.act3Scale,
							.cropX = act3Crop.x,
							.cropY = act3Crop.y,
							.cropWidth = act3Crop.width,
							.cropHeight = act3Crop.height,
							.tuning = GetAct3Tuning(upscaling.foveatedRender.settings),
							.sharpen = upscaling.foveatedRender.settings.act3Sharpen,
							.sharpness = upscaling.foveatedRender.settings.act3Sharpness,
							.resetEveryFrame = upscaling.foveatedRender.settings.act3ResetEveryFrame,
							.featherWidth = upscaling.foveatedRender.settings.act3FeatherWidth,
							.ditherStrength = upscaling.foveatedRender.settings.act3DitherStrength,
							.guidanceEnabled = upscaling.foveatedRender.settings.act3GuidanceEnabled,
							.guidanceLuma = upscaling.foveatedRender.settings.act3GuidanceLuma,
							.guidanceChroma = upscaling.foveatedRender.settings.act3GuidanceChroma,
							.guidancePrior = upscaling.foveatedRender.settings.act3GuidancePrior,
							.guidanceMaxGain = upscaling.foveatedRender.settings.act3GuidanceMaxGain,
							.guidanceToneLuma = upscaling.foveatedRender.settings.act3GuidanceToneLuma,
							.guidanceToneChroma = upscaling.foveatedRender.settings.act3GuidanceToneChroma,
							.guidanceDetailGain = upscaling.foveatedRender.settings.act3GuidanceDetailGain,
							.guidanceDither = upscaling.foveatedRender.settings.act3GuidanceDither,
							.guidanceDirSharpen = upscaling.foveatedRender.settings.act3GuidanceDirSharpen,
						},
						.tuning = GetTuning(upscaling.foveatedRender.settings),
						.motionVectorScaleX = static_cast<float>(plan.nrWidth),
						.motionVectorScaleY = static_cast<float>(plan.nrHeight),
						.sharpness = upscaling.foveatedRender.settings.flatPipelineSharpness,
						.sharpen = plan.sharpen,
						.requested = requested,
						.eligible = eligible && guidesReady,
						.renderDocReplacement = upscaling.dx12SwapChain.IsRenderDocCompatibility(),
						.reset = reset,
					},
				});
			return true;
		}
	}

	bool ApplyFoveatedLdr()
	{
		auto& upscaling = globals::features::upscaling;
		auto& foveated = upscaling.foveatedRender;
		if (!globals::game::isVR && upscaling.dx12SwapChain.useArise)
			return CaptureModel3Tail(upscaling);
		if (!globals::game::isVR)
			return ApplyFlatPipeline(upscaling, foveated);
		if (!upscaling.IsVrDlssPostInjectActive() && vrSixPassWasActive) {
			vrSixPassWasActive = false;
			Reset();
		}
		return false;
	}

	bool ApplyVrAfterTonemap()
	{
		auto& upscaling = globals::features::upscaling;
		auto& foveated = upscaling.foveatedRender;
		if (!upscaling.IsVrDlssPostInjectActive()) {
			if (vrSixPassWasActive) {
				vrSixPassWasActive = false;
				Reset();
			}
			return false;
		}
		return ApplyVrSixPass(upscaling, foveated);
	}

	bool GetVrOwnedEyeSizes(uint32_t& renderW, uint32_t& renderH, uint32_t& displayW, uint32_t& displayH)
	{
		renderW = renderH = displayW = displayH = 0;
		if (ResolveVrEngineEye(globals::game::renderer, renderW, renderH)) {
			displayW = renderW;
			displayH = renderH;
			return true;
		}
		auto* renderer = globals::game::renderer;
		if (!renderer)
			return false;
		auto* total = static_cast<ID3D11Texture2D*>(
			renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTOTAL].texture);
		if (!total)
			return false;
		D3D11_TEXTURE2D_DESC desc{};
		total->GetDesc(&desc);
		if (desc.Width < 2 || !desc.Height)
			return false;
		renderW = displayW = desc.Width / 2;
		renderH = displayH = desc.Height;
		return true;
	}

	ID3D11Texture2D* GetVrOwnedComposite(ID3D11ShaderResourceView** outSrv)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || !vrSixPassWasActive)
			return nullptr;
		auto* tex = ResolveVrPostTemp(renderer);
		if (!tex)
			return nullptr;
		if (outSrv) {
			auto& targets = renderer->GetRuntimeData().renderTargets;
			*outSrv = nullptr;
			if (tex == static_cast<ID3D11Texture2D*>(targets[RE::RENDER_TARGETS::kIMAGESPACE_TEMP_COPY].texture))
				*outSrv = targets[RE::RENDER_TARGETS::kIMAGESPACE_TEMP_COPY].SRV;
			else if (tex == static_cast<ID3D11Texture2D*>(targets[RE::RENDER_TARGETS::kIMAGESPACE_TEMP_COPY2].texture))
				*outSrv = targets[RE::RENDER_TARGETS::kIMAGESPACE_TEMP_COPY2].SRV;
		}
		return tex;
	}

	ID3D11Texture2D* GetVrOwnedEye(uint32_t eye)
	{
		if (eye > 1 || !vrEyeColor[eye] || !vrEyeColor[eye]->resource)
			return nullptr;
		return static_cast<ID3D11Texture2D*>(vrEyeColor[eye]->resource.get());
	}

	Act2BackendStats GetAct2BackendStats()
	{
		return act2BackendStats;
	}

	void RequestBandCapture()
	{
		bandCaptureRequested = true;
	}

	void Reset()
	{
#ifdef ENABLE_SKYRIM_VR
		VrSubmitDlssg::NoteOwnedFrame(UINT32_MAX);
#endif
		ExperimentCapture::Reset();
		Renderer::Instance().Reset();
		color[0].reset();
		color[1].reset();
		halfResolutionColor.reset();
		upscaledNeuralColor.reset();
		fusionOutput.reset();
		nrInputColor.reset();
		guidanceGain.reset();
		guidanceEditLow.reset();
		guideLowDisplay.reset();
		resizedDepthGuide.reset();
		resizedMotionGuide.reset();
		act2ReactiveMask.reset();
		act2TransparencyMask.reset();
		flatAct1Output.reset();
		flatAct2Output.reset();
		flatFusionReference.reset();
		flatSharpenOutput.reset();
		act3Color.reset();
		act3Depth.reset();
		act3Motion.reset();
		act3DepthFull.reset();
		act3MotionFull.reset();
		act3SharpenOutput.reset();
		act3Writeback.reset();
		act3Input.reset();
		act3Fusion.reset();
		act3Gain.reset();
		act3EditLow.reset();
		act3GuideLow.reset();
		vrCompositeSbs.reset();
		vrEyeColor[0].reset();
		vrEyeColor[1].reset();
		nisScaler.Reset();
		act1FsrScaler.Reset();
		act2FsrScaler.Reset();
		downsampleCS.Reset();
		tentCS.Reset();
		mitchellCS.Reset();
		easuCS.Reset();
		fusionCS.Reset();
		guidanceFitCS.Reset();
		resizeGuidesCS.Reset();
		downsampleCB = nullptr;
		fusionCB = nullptr;
		resizeGuidesCB = nullptr;
		colorWidth = colorHeight = 0;
		colorFormat = DXGI_FORMAT_UNKNOWN;
		lastAppliedFrame = UINT32_MAX;
		lastAttemptedFrame = UINT32_MAX;
		writebackLogged = false;
		flatRouteWasActive = false;
		vrSixPassWasActive = false;
		vrSixPassLogged = false;
		vrSixPassTempLogged = false;
		vrSixPassDoneFrame = UINT32_MAX;
		lastFlatInputPrefilter = UINT32_MAX;
		lastFlatMotionVectorMode = UINT32_MAX;
		lastFlatResetEveryFrame = false;
		lastFlatBypassOutput = false;
		flatPostInjectNrFallbackLogged = false;
		act2DlssLogged = false;
		act2FsrLogged = false;
		lastFlatOutputMode = UINT32_MAX;
		lastFlatAct1JitterMode = UINT32_MAX;
		lastFlatAct2JitterMode = UINT32_MAX;
		lastFlatPlanSignature = 0;
		lastModel3PlanSignature = 0;
		hasModel3Plan = false;
		lastModel3Frame = UINT64_MAX;
		lastModel3CaptureFrame = UINT64_MAX;
		lastModel3Eligible = false;
		model3RestrictionLogged = false;
		model3CaptureFailureLogged = false;
		act2BackendStats = {};
	}
}
