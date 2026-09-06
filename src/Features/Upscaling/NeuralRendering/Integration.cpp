#include "Integration.h"

#include "Act2FsrScaler.h"
#include "NISScaler.h"
#include "Renderer.h"
#include "Features/HDRDisplay.h"
#include "Features/Upscaling.h"
#include "Features/Upscaling/FoveatedRender/Bridge.h"
#include "Features/Upscaling/FoveatedRender/Core.h"
#include "Globals.h"
#include "GpuPass.h"
#include "Utils/LazyShader.h"

#include <array>
#include <bit>

#include <d3d11.h>

namespace NeuralRendering
{
	namespace
	{
		eastl::unique_ptr<Texture2D> color[2];
		eastl::unique_ptr<Texture2D> halfResolutionColor;
		eastl::unique_ptr<Texture2D> upscaledNeuralColor;
		eastl::unique_ptr<Texture2D> fusionOutput;
		eastl::unique_ptr<Texture2D> resizedDepthGuide;
		eastl::unique_ptr<Texture2D> resizedMotionGuide;
		eastl::unique_ptr<Texture2D> act2ReactiveMask;
		eastl::unique_ptr<Texture2D> act2TransparencyMask;
		eastl::unique_ptr<Texture2D> flatAct1Output;
		eastl::unique_ptr<Texture2D> flatAct2Output;
		eastl::unique_ptr<Texture2D> flatFusionReference;
		eastl::unique_ptr<Texture2D> flatSharpenOutput;
		std::uint32_t colorWidth = 0;
		std::uint32_t colorHeight = 0;
		DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
		Util::LazyShader<ID3D11ComputeShader> downsampleCS;
		Util::LazyShader<ID3D11ComputeShader> fusionCS;
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
		bool flatFrameGenerationBlockLogged = false;
		bool flatHdrBlockLogged = false;
		bool flatPostInjectLogged = false;
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

		struct FusionConstants
		{
			std::uint32_t fullWidth;
			std::uint32_t fullHeight;
			std::uint32_t guidanceWidth;
			std::uint32_t guidanceHeight;
			float lumaStrength;
			float chromaStrength;
			float edgeProtection;
			float residualLimit;
			std::uint32_t direction;
			std::uint32_t denoiseMode;
			float denoiseStrength;
			float softness;
			std::uint32_t depthWidth;
			std::uint32_t depthHeight;
			float frequencyGuidance;
			float depthGuidance;
			std::uint32_t enhancedGuidance;
			float colorGuidance;
			float guidancePadding[2];
		};

		struct ResizeGuidesConstants
		{
			std::uint32_t sourceWidth;
			std::uint32_t sourceHeight;
			std::uint32_t targetWidth;
			std::uint32_t targetHeight;
			float motionVectorMultiplier;
			float padding[3];
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
			if (buffer)
				return true;
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = static_cast<UINT>((size + 15) & ~std::size_t(15));
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
			const bool needsDisplayIntermediate = plan.act2SrBackend == FoveatedRender::FlatAct2SrBackend::kNIS ||
				plan.act2SrBackend == FoveatedRender::FlatAct2SrBackend::kLanczos;
			if (needsDisplayIntermediate &&
				!matches(upscaledNeuralColor.get(), plan.displayWidth, plan.displayHeight, sourceDesc.Format))
				upscaledNeuralColor = Upscaling::CreateTextureFromSource(source, plan.displayWidth, plan.displayHeight,
					false, true, true, "NeuralRendering::UpscaledNeuralColor");
			if (!needsDisplayIntermediate)
				upscaledNeuralColor.reset();

			const bool needsFusion = plan.fusion;
			if (needsFusion && !matches(fusionOutput.get(), plan.displayWidth, plan.displayHeight, sourceDesc.Format))
				fusionOutput = Upscaling::CreateTextureFromSource(source, plan.displayWidth, plan.displayHeight,
					false, true, true, "NeuralRendering::FusionOutput");
			if (!needsFusion) {
				fusionOutput.reset();
				fusionCB = nullptr;
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
				(!needsFusion || (fusionOutput && fusionOutput->srv && fusionOutput->uav &&
					EnsureConstantBuffer(sizeof(FusionConstants), fusionCB))) &&
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

		bool ResizeGuides(ID3D11DeviceContext* context, ID3D11ShaderResourceView* sourceDepth,
			ID3D11ShaderResourceView* sourceMotion, std::uint32_t sourceWidth, std::uint32_t sourceHeight,
			std::uint32_t targetWidth, std::uint32_t targetHeight, float motionVectorMultiplier)
		{
			CS_GPU_PASS("FlatPipeline::ResizeGuides");
			auto* shader = resizeGuidesCS.Get(
				L"Data\\Shaders\\Upscaling\\NeuralRendering\\ResizeGuidesCS.hlsl", {}, "cs_5_0",
				"main", "NeuralRendering::ResizeGuidesCS");
			if (!shader || !sourceDepth || !sourceMotion || !resizedDepthGuide || !resizedMotionGuide)
				return false;
			const ResizeGuidesConstants constants{
				sourceWidth, sourceHeight, targetWidth, targetHeight, motionVectorMultiplier, {}
			};
			if (!UpdateConstantBuffer(context, resizeGuidesCB.get(), constants))
				return false;

			context->CSSetShader(shader, nullptr, 0);
			ID3D11Buffer* buffers[]{ resizeGuidesCB.get() };
			context->CSSetConstantBuffers(0, 1, buffers);
			ID3D11ShaderResourceView* srvs[]{ sourceDepth, sourceMotion };
			context->CSSetShaderResources(0, 2, srvs);
			ID3D11UnorderedAccessView* uavs[]{ resizedDepthGuide->uav.get(), resizedMotionGuide->uav.get() };
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

		bool ResampleLanczos(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source,
			ID3D11UnorderedAccessView* destination, std::uint32_t sourceWidth, std::uint32_t sourceHeight,
			std::uint32_t destinationWidth, std::uint32_t destinationHeight)
		{
			CS_GPU_PASS("FlatPipeline::Lanczos");
			auto* shader = downsampleCS.Get(
				L"Data\\Shaders\\Upscaling\\NeuralRendering\\DownsampleColorCS.hlsl", {}, "cs_5_0",
				"main", "NeuralRendering::DownsampleColorCS");
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

		bool ResolveNeuralRendering(ID3D11DeviceContext* context, ID3D11ShaderResourceView* referenceColor,
			ID3D11ShaderResourceView* neuralColor, ID3D11ShaderResourceView* depthGuide,
			std::uint32_t width, std::uint32_t height, std::uint32_t guidanceWidth, std::uint32_t guidanceHeight,
			std::uint32_t depthWidth, std::uint32_t depthHeight, const FoveatedRender::Settings& settings)
		{
			CS_GPU_PASS("FlatPipeline::Fusion");
			auto* shader = fusionCS.Get(
				L"Data\\Shaders\\Upscaling\\NeuralRendering\\ResolveFusionCS.hlsl", {}, "cs_5_0",
				"main", "NeuralRendering::ResolveFusionCS");
			if (!shader || !referenceColor || !neuralColor || !fusionOutput ||
				(settings.flatPipelineFusionEnhancedGuidance && !depthGuide))
				return false;
			const FusionConstants constants{
				width, height, guidanceWidth, guidanceHeight,
				settings.neuralRenderingFusionLuma,
				settings.neuralRenderingFusionChroma,
				settings.neuralRenderingFusionEdgeProtection,
				settings.neuralRenderingFusionResidualLimit,
				settings.flatPipelineFusionDirection,
				settings.flatPipelineFusionDenoise,
				settings.flatPipelineFusionDenoiseStrength,
				settings.flatPipelineFusionSoftness,
				depthWidth,
				depthHeight,
				settings.flatPipelineFusionFrequencyGuidance,
				settings.flatPipelineFusionDepthGuidance,
				settings.flatPipelineFusionEnhancedGuidance ? 1u : 0u,
				settings.flatPipelineFusionColorGuidance,
				{},
			};
			if (!UpdateConstantBuffer(context, fusionCB.get(), constants))
				return false;

			context->CSSetShader(shader, nullptr, 0);
			ID3D11Buffer* buffers[]{ fusionCB.get() };
			context->CSSetConstantBuffers(0, 1, buffers);
			ID3D11ShaderResourceView* srvs[]{ referenceColor, neuralColor,
				settings.flatPipelineFusionEnhancedGuidance ? depthGuide : nullptr };
			context->CSSetShaderResources(0, 3, srvs);
			ID3D11UnorderedAccessView* uavs[]{ fusionOutput->uav.get() };
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

			ID3D11Buffer* nullBuffer[]{ nullptr };
			ID3D11ShaderResourceView* nullSRVs[]{ nullptr, nullptr, nullptr };
			ID3D11UnorderedAccessView* nullUAV[]{ nullptr };
			context->CSSetConstantBuffers(0, 1, nullBuffer);
			context->CSSetShaderResources(0, 3, nullSRVs);
			context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
			return true;
		}

		bool ApplyAct2Dlss(Upscaling& upscaling, ID3D11DeviceContext* context,
			ID3D11ShaderResourceView* sourceDepth, ID3D11ShaderResourceView* sourceMotion,
			std::uint32_t sourceGuideWidth, std::uint32_t sourceGuideHeight,
			std::uint32_t inputWidth, std::uint32_t inputHeight,
			std::uint32_t outputWidth, std::uint32_t outputHeight, float motionVectorMultiplier, bool forceReset)
		{
			if (!halfResolutionColor || !upscaledNeuralColor || !upscaling.reactiveMaskTexture ||
				!upscaling.transparencyCompositionMaskTexture ||
				!EnsureGuideResources(upscaling.motionVectorCopyTexture->resource.get(), inputWidth, inputHeight) ||
				!ResizeGuides(context, sourceDepth, sourceMotion, sourceGuideWidth, sourceGuideHeight,
					inputWidth, inputHeight, motionVectorMultiplier) ||
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
			float motionVectorMultiplier, bool forceReset)
		{
			if (!halfResolutionColor || !upscaledNeuralColor || !upscaling.reactiveMaskTexture ||
				!upscaling.transparencyCompositionMaskTexture ||
				!EnsureGuideResources(upscaling.motionVectorCopyTexture->resource.get(), inputWidth, inputHeight) ||
				!ResizeGuides(context, sourceDepth, sourceMotion, sourceGuideWidth, sourceGuideHeight,
					inputWidth, inputHeight, motionVectorMultiplier) ||
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

		bool ApplyFlatPipeline(Upscaling& upscaling, FoveatedRender& foveated)
		{
			using Act1 = FoveatedRender::FlatAct1Backend;
			using Act2 = FoveatedRender::FlatAct2SrBackend;

			const bool hdrConfigured = globals::features::hdrDisplay.loaded && globals::features::hdrDisplay.settings.enableHDR;
			const bool routeActive = foveated.settings.neuralRenderingEnabled &&
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
				static_cast<std::uint64_t>(std::bit_cast<uint32_t>(foveated.settings.neuralRenderingSkinStructure)) })
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
				logger::info("[FlatPipeline] Plan signature=0x{:016X} D={}x{} R={}x{} ACT1={} {}x{} N={}x{} ({:.2f}) ACT2={} {}x{} RCAS={}",
					planSignature, plan.displayWidth, plan.displayHeight, plan.engineWidth, plan.engineHeight,
					static_cast<uint>(plan.act1Backend), plan.act1OutputWidth, plan.act1OutputHeight,
					plan.nrWidth, plan.nrHeight, foveated.settings.flatPipelineNrScale,
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

			if (!EnsureFlatPipelineResources(framebuffer, plan))
				return false;
			ID3D11ShaderResourceView* postSourceSRV = framebufferTarget.SRV;
			ID3D11Resource* postSourceResource = framebuffer;
			if (!postSourceSRV) {
				if (!EnsureColorResources(framebuffer, plan.displayWidth, plan.displayHeight))
					return false;
				context->CopyResource(color[0]->resource.get(), framebuffer);
				postSourceSRV = color[0]->srv.get();
				postSourceResource = color[0]->resource.get();
			}
			ID3D11ShaderResourceView* fallbackSRV = nullptr;
			ID3D11Resource* fallbackResource = nullptr;
			uint32_t fallbackWidth = plan.displayWidth;
			uint32_t fallbackHeight = plan.displayHeight;
			bool outputCommitted = false;
			const SKSE::stl::scope_exit resolveFallback([&]() noexcept {
				if (outputCommitted || !fallbackResource)
					return;
				if (fallbackWidth == plan.displayWidth && fallbackHeight == plan.displayHeight) {
					context->CopyResource(framebuffer, fallbackResource);
				} else if (fallbackSRV &&
					EnsurePipelineTexture(upscaledNeuralColor, framebuffer, plan.displayWidth, plan.displayHeight,
						"NeuralRendering::FallbackOutput") &&
					ResampleLanczos(context, fallbackSRV, upscaledNeuralColor->uav.get(),
						fallbackWidth, fallbackHeight, plan.displayWidth, plan.displayHeight)) {
					context->CopyResource(framebuffer, upscaledNeuralColor->resource.get());
				}
			});

			ID3D11ShaderResourceView* act1SourceSRV = postSourceSRV;
			ID3D11Resource* act1SourceResource = postSourceResource;
			uint32_t act1SourceWidth = plan.displayWidth;
			uint32_t act1SourceHeight = plan.displayHeight;
			bool succeeded = true;

			if (plan.act1Backend != Act1::kBypass) {
				ScopedGpuPass act1Pass("FlatPipeline::ACT1");
				auto& streamline = Upscaling::streamline;
				succeeded = streamline.PrepareFlatInputIsolation(context, framebuffer, depth.depthSRV,
					upscaling.motionVectorCopyTexture->resource.get(), upscaling.reactiveMaskTexture->resource.get(),
					upscaling.transparencyCompositionMaskTexture->resource.get(), plan.engineWidth, plan.engineHeight) &&
					EnsurePipelineTexture(flatAct1Output, framebuffer, plan.act1OutputWidth, plan.act1OutputHeight,
						"NeuralRendering::FlatPipelineAct1Output");
				if (!succeeded)
					return false;

				if (plan.act1Backend == Act1::kDLSS) {
					const sl::Extent inputExtent{ 0, 0, plan.engineWidth, plan.engineHeight };
					const sl::Extent outputExtent{ 0, 0, plan.act1OutputWidth, plan.act1OutputHeight };
					const uint quality = plan.act1Quality;
					const uint preset = plan.act1Preset;
					succeeded = streamline.EvaluateDLSS(streamline.flatPostInjectViewport, 0,
						streamline.flatIsolatedColor->resource.get(), flatAct1Output->resource.get(),
						streamline.flatIsolatedDepth->resource.get(), streamline.flatIsolatedMotionVectors->resource.get(),
						streamline.flatIsolatedReactiveMask->resource.get(), streamline.flatIsolatedTransparencyMask->resource.get(),
						inputExtent, outputExtent, plan.act1OutputWidth, plan.act1OutputHeight,
						Streamline::DLSSModeForQualityMode(quality), false, planChanged,
						foveated.settings.flatPipelineAct1Jitter == 1, 1.0f, 1.0f,
						preset, 0.0f);
				} else {
					succeeded = act1FsrScaler.Apply(globals::d3d::device, context,
						streamline.flatIsolatedColor->resource.get(), streamline.flatIsolatedDepth->resource.get(),
						streamline.flatIsolatedMotionVectors->resource.get(), streamline.flatIsolatedReactiveMask->resource.get(),
						streamline.flatIsolatedTransparencyMask->resource.get(), flatAct1Output->resource.get(),
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

			if (act1SourceWidth == plan.nrWidth && act1SourceHeight == plan.nrHeight)
				context->CopyResource(halfResolutionColor->resource.get(), act1SourceResource);
			else if (!ResampleLanczos(context, act1SourceSRV, halfResolutionColor->uav.get(),
					act1SourceWidth, act1SourceHeight, plan.nrWidth, plan.nrHeight))
				return false;
			if (!EnsureGuideResources(upscaling.motionVectorCopyTexture->resource.get(), plan.nrWidth, plan.nrHeight) ||
				!ResizeGuides(context, depth.depthSRV, upscaling.motionVectorCopyTexture->srv.get(),
					plan.engineWidth, plan.engineHeight, plan.nrWidth, plan.nrHeight, 1.0f))
				return false;

			ID3D11Resource* finalResource = nullptr;
			ID3D11ShaderResourceView* finalSRV = nullptr;
			succeeded = Renderer::Instance().Apply(globals::d3d::device, context, 0,
				halfResolutionColor->resource.get(), resizedDepthGuide->resource.get(), resizedDepthGuide->srv.get(),
				resizedMotionGuide->resource.get(), plan.nrWidth, plan.nrHeight, plan.nrWidth, plan.nrHeight,
				static_cast<float>(plan.engineWidth), static_cast<float>(plan.engineHeight),
				GetTuning(foveated.settings), planChanged);
			if (!succeeded)
				return false;
			fallbackSRV = halfResolutionColor->srv.get();
			fallbackResource = halfResolutionColor->resource.get();
			fallbackWidth = plan.nrWidth;
			fallbackHeight = plan.nrHeight;

			if (plan.act2SrBackend == Act2::kDLSS || plan.act2SrBackend == Act2::kFSR3) {
				if (!EnsurePipelineTexture(flatAct2Output, framebuffer, plan.act2OutputWidth, plan.act2OutputHeight,
						"NeuralRendering::FlatPipelineAct2Output") ||
					!EnsureAct2MaskResources(upscaling.reactiveMaskTexture->resource.get(),
						upscaling.transparencyCompositionMaskTexture->resource.get(), plan.nrWidth, plan.nrHeight))
					return false;
				if (plan.act2SrBackend == Act2::kDLSS) {
					const sl::Extent inputExtent{ 0, 0, plan.nrWidth, plan.nrHeight };
					const sl::Extent outputExtent{ 0, 0, plan.act2OutputWidth, plan.act2OutputHeight };
					succeeded = Upscaling::streamline.EvaluateDLSS(Upscaling::streamline.flatAct2Viewport, 0,
						halfResolutionColor->resource.get(), flatAct2Output->resource.get(),
						resizedDepthGuide->resource.get(), resizedMotionGuide->resource.get(),
						act2ReactiveMask->resource.get(), act2TransparencyMask->resource.get(),
						inputExtent, outputExtent, plan.act2OutputWidth, plan.act2OutputHeight,
						Streamline::DLSSModeForQualityMode(plan.act2Quality), false, planChanged,
						foveated.settings.flatPipelineAct2Jitter == 1,
						static_cast<float>(plan.nrWidth) / static_cast<float>(plan.engineWidth),
						static_cast<float>(plan.nrHeight) / static_cast<float>(plan.engineHeight),
						foveated.settings.flatPipelineAct2Preset, 0.0f);
				} else {
					succeeded = act2FsrScaler.Apply(globals::d3d::device, context,
						halfResolutionColor->resource.get(), resizedDepthGuide->resource.get(), resizedMotionGuide->resource.get(),
						act2ReactiveMask->resource.get(), act2TransparencyMask->resource.get(), flatAct2Output->resource.get(),
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
				else if (!ResampleLanczos(context, halfResolutionColor->srv.get(), upscaledNeuralColor->uav.get(),
						plan.nrWidth, plan.nrHeight, plan.displayWidth, plan.displayHeight))
					return false;
				finalResource = upscaledNeuralColor->resource.get();
				finalSRV = upscaledNeuralColor->srv.get();
			}

			if (plan.fusion) {
				if (!ResolveNeuralRendering(context, act1SourceSRV, finalSRV, resizedDepthGuide->srv.get(),
						plan.displayWidth, plan.displayHeight, act1SourceWidth, act1SourceHeight,
						plan.nrWidth, plan.nrHeight, foveated.settings))
					return false;
				finalResource = fusionOutput->resource.get();
				finalSRV = fusionOutput->srv.get();
			}

			if (foveated.settings.flatPipelineSharpen) {
				if (!EnsurePipelineTexture(flatSharpenOutput, framebuffer, plan.displayWidth, plan.displayHeight,
						"NeuralRendering::FlatPipelineSharpenOutput"))
					return false;
				upscaling.rcas.Initialize();
				if (upscaling.rcas.ApplySharpen(finalSRV, flatSharpenOutput->uav.get(), foveated.settings.flatPipelineSharpness))
					finalResource = flatSharpenOutput->resource.get();
			}

			context->CopyResource(framebuffer, finalResource);
			outputCommitted = true;
			lastAppliedFrame = frame;
			logger::debug("[FlatPipeline] frame={} D={}x{} R={}x{} A1={}x{} N={}x{} NRout={}x{} A2={}x{}",
				frame, plan.displayWidth, plan.displayHeight, plan.engineWidth, plan.engineHeight,
				plan.act1OutputWidth, plan.act1OutputHeight, plan.nrWidth, plan.nrHeight,
				plan.nrOutputWidth, plan.nrOutputHeight, plan.act2OutputWidth, plan.act2OutputHeight);
			return true;
		}

		bool ApplyFlatLdr(Upscaling& upscaling, FoveatedRender& foveated)
		{
			const bool frameGenerationConfigured = upscaling.IsFrameGenerationConfiguredForSession();
			const bool hdrConfigured = globals::features::hdrDisplay.loaded &&
				globals::features::hdrDisplay.settings.enableHDR;
			auto* renderer = globals::game::renderer;
			winrt::com_ptr<ID3D11Texture2D> framebufferHolder;
			ID3D11Texture2D* framebuffer = nullptr;
			if (renderer) {
				auto& target = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
				framebuffer = ResolveRenderTargetTexture(target, framebufferHolder);
			}
			const bool routeActive = upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS &&
				foveated.settings.neuralRenderingEnabled && !frameGenerationConfigured && !hdrConfigured;
			if (!routeActive) {
				if (foveated.settings.neuralRenderingEnabled && frameGenerationConfigured && !flatFrameGenerationBlockLogged) {
					logger::warn("[DLSSNR] Flat route blocked: disable Frame Generation and restart the game");
					flatFrameGenerationBlockLogged = true;
				}
				if (foveated.settings.neuralRenderingEnabled && hdrConfigured && !flatHdrBlockLogged) {
					logger::warn("[DLSSNR] Flat route blocked: HDR Display is not supported by the LDR integration");
					flatHdrBlockLogged = true;
				}
				if (flatRouteWasActive)
					Reset();
				return false;
			}
			flatRouteWasActive = true;
			const bool postInject = upscaling.IsFlatDlssPostInjectActive();
			const bool act1Bypass = upscaling.IsFlatAct1BypassActive();
			if (foveated.settings.neuralRenderingBypassOutput && !postInject) {
				lastFlatBypassOutput = true;
				return false;
			}

			const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
			if (lastAppliedFrame == frame || lastAttemptedFrame == frame)
				return true;
			auto* context = globals::d3d::context;
			if (!renderer || !context || !globals::d3d::device || !upscaling.motionVectorCopyTexture)
				return false;

			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			auto& rawMotion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
			if (!framebuffer || !depth.texture || !depth.depthSRV || !upscaling.motionVectorCopyTexture->resource ||
				!rawMotion.texture || !rawMotion.SRV)
				return false;

			D3D11_TEXTURE2D_DESC totalDesc{};
			D3D11_TEXTURE2D_DESC motionDesc{};
			framebuffer->GetDesc(&totalDesc);
			upscaling.motionVectorCopyTexture->resource->GetDesc(&motionDesc);
			if (!EnsureColorResources(framebuffer, totalDesc.Width, totalDesc.Height))
				return false;

			CS_GPU_PASS("NeuralRendering::FlatLdrBeforeUI");
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
			const std::uint32_t cachedRenderWidth = upscaling.lastDlssRenderWidth ? upscaling.lastDlssRenderWidth : motionDesc.Width;
			const std::uint32_t cachedRenderHeight = upscaling.lastDlssRenderHeight ? upscaling.lastDlssRenderHeight : motionDesc.Height;
			const std::uint32_t renderWidth = std::clamp(cachedRenderWidth, 1u, motionDesc.Width);
			const std::uint32_t renderHeight = std::clamp(cachedRenderHeight, 1u, motionDesc.Height);
			const std::uint32_t act1Mode = postInject ? 0u :
				upscaling.bootSnapshot.Boot(&Upscaling::Settings::flatAct1ResControl);
			const bool temporalUpscaleInput = postInject || act1Mode != 0 || upscaling.settings.qualityMode != 0;
			const std::uint32_t motionVectorMode = foveated.settings.neuralRenderingMotionVectorMode;
			ID3D11ShaderResourceView* sourceMotionSRV = motionVectorMode == 1 ? rawMotion.SRV :
				upscaling.motionVectorCopyTexture->srv.get();
			const float motionVectorMultiplier = GetMotionVectorMultiplier(motionVectorMode);
			const std::uint32_t outputMode = std::clamp(foveated.settings.neuralRenderingOutputMode, 1u, 3u);
			const bool diagnosticModeChanged = lastFlatInputPrefilter != foveated.settings.neuralRenderingInputPrefilter ||
				lastFlatMotionVectorMode != motionVectorMode ||
				lastFlatResetEveryFrame != foveated.settings.neuralRenderingResetEveryFrame ||
				lastFlatBypassOutput != foveated.settings.neuralRenderingBypassOutput ||
				lastFlatOutputMode != outputMode ||
				(foveated.settings.flatAct1Enabled && lastFlatAct1JitterMode != upscaling.settings.flatAct1JitterMode) ||
				lastFlatAct2JitterMode != upscaling.settings.flatAct2JitterMode;
			const bool forceReset = foveated.settings.neuralRenderingResetEveryFrame || diagnosticModeChanged;
			if (lastFlatOutputMode != outputMode) {
				static constexpr const char* outputNames[]{ "Fusion", "NIS", "DLSS-SR", "FSR3" };
				logger::info("[ACT2] Selected backend={} mode={} quality={} forceReset=true",
					outputNames[outputMode], outputMode, foveated.settings.flatAct2QualityMode);
			}
			lastAttemptedFrame = frame;

			if (postInject) {
				auto& streamline = Upscaling::streamline;
				if (!upscaling.reactiveMaskTexture || !upscaling.transparencyCompositionMaskTexture ||
					!streamline.PrepareFlatInputIsolation(context, framebuffer, depth.depthSRV,
						upscaling.motionVectorCopyTexture->resource.get(),
						upscaling.reactiveMaskTexture->resource.get(),
						upscaling.transparencyCompositionMaskTexture->resource.get(),
						renderWidth, renderHeight)) {
					logger::error("[PostInject] Failed to prepare exact LDR RenderRes inputs {}x{}", renderWidth, renderHeight);
					return false;
				}
				const sl::Extent extentIn{ 0, 0, renderWidth, renderHeight };
				const sl::Extent extentOut{ 0, 0, totalDesc.Width, totalDesc.Height };
				const bool srSucceeded = streamline.EvaluateDLSS(streamline.flatPostInjectViewport, 0,
					streamline.flatIsolatedColor->resource.get(), color[0]->resource.get(),
					streamline.flatIsolatedDepth->resource.get(), streamline.flatIsolatedMotionVectors->resource.get(),
					streamline.flatIsolatedReactiveMask->resource.get(), streamline.flatIsolatedTransparencyMask->resource.get(),
					extentIn, extentOut, totalDesc.Width, totalDesc.Height,
					Streamline::DLSSModeForQualityMode(foveated.settings.flatAct1QualityMode), false, diagnosticModeChanged,
					upscaling.settings.flatAct1JitterMode == 1, 1.0f, 1.0f,
					foveated.settings.flatAct1Preset, foveated.settings.flatAct1Sharpness);
				if (!srSucceeded) {
					logger::error("[PostInject] LDR DLSS-SR evaluate failed {}x{} -> {}x{}; preserving engine Post output",
						renderWidth, renderHeight, totalDesc.Width, totalDesc.Height);
					return false;
				}
				if (!flatPostInjectLogged) {
					logger::info("[PostInject] LDR DLSS-SR -> NR route active input={}x{} output={}x{} viewport=2 mode={}",
						renderWidth, renderHeight, totalDesc.Width, totalDesc.Height, upscaling.settings.qualityMode);
					flatPostInjectLogged = true;
				}
				if (foveated.settings.neuralRenderingBypassOutput) {
					context->CopyResource(framebuffer, color[0]->resource.get());
					lastAppliedFrame = frame;
					lastFlatBypassOutput = true;
					return true;
				}
			} else {
				context->CopyResource(color[0]->resource.get(), framebuffer);
			}

			bool succeeded = false;
			const float act2InputScale = 1.0f / Upscaling::GetQualityModeRatio(foveated.settings.flatAct2QualityMode);
			std::uint32_t nrWidth = std::max(1u, static_cast<std::uint32_t>(std::lround(totalDesc.Width * act2InputScale)));
			std::uint32_t nrHeight = std::max(1u, static_cast<std::uint32_t>(std::lround(totalDesc.Height * act2InputScale)));
			if (outputMode == 2 && foveated.settings.flatAct2QualityMode != 0) {
				upscaling.streamline.ClampToDLSSRenderRange(foveated.settings.flatAct2QualityMode,
					totalDesc.Width, totalDesc.Height, nrWidth, nrHeight);
			}
			if (EnsureFusionResources(framebuffer, totalDesc.Width, totalDesc.Height, nrWidth, nrHeight) &&
				DownsampleForNeuralRendering(context,
					act1Bypass ? renderWidth : totalDesc.Width,
					act1Bypass ? renderHeight : totalDesc.Height)) {
				D3D11_TEXTURE2D_DESC lowDesc{};
				halfResolutionColor->resource->GetDesc(&lowDesc);
				const std::uint32_t guideWidth = temporalUpscaleInput ? renderWidth : lowDesc.Width;
				const std::uint32_t guideHeight = temporalUpscaleInput ? renderHeight : lowDesc.Height;
				if (!EnsureGuideResources(upscaling.motionVectorCopyTexture->resource.get(), guideWidth, guideHeight) ||
					!ResizeGuides(context, depth.depthSRV, sourceMotionSRV,
						renderWidth, renderHeight, guideWidth, guideHeight, motionVectorMultiplier)) {
					return false;
				}
				succeeded = Renderer::Instance().Apply(globals::d3d::device, context, 0,
					halfResolutionColor->resource.get(), resizedDepthGuide->resource.get(), resizedDepthGuide->srv.get(),
					resizedMotionGuide->resource.get(), guideWidth, guideHeight,
					lowDesc.Width, lowDesc.Height, static_cast<float>(guideWidth),
					static_cast<float>(guideHeight), GetTuning(foveated.settings), forceReset);
				if (succeeded && outputMode == 2) {
					succeeded = ApplyAct2Dlss(upscaling, context, depth.depthSRV, sourceMotionSRV,
						renderWidth, renderHeight, lowDesc.Width, lowDesc.Height,
						totalDesc.Width, totalDesc.Height, motionVectorMultiplier, forceReset);
					if (succeeded)
						context->CopyResource(framebuffer, upscaledNeuralColor->resource.get());
				} else if (succeeded && outputMode == 3) {
					succeeded = ApplyAct2Fsr(upscaling, context, depth.depthSRV, sourceMotionSRV,
						renderWidth, renderHeight, lowDesc.Width, lowDesc.Height,
						totalDesc.Width, totalDesc.Height, motionVectorMultiplier, forceReset);
					if (succeeded)
						context->CopyResource(framebuffer, upscaledNeuralColor->resource.get());
				} else if (succeeded) {
					if (lowDesc.Width == totalDesc.Width && lowDesc.Height == totalDesc.Height) {
						context->CopyResource(framebuffer, halfResolutionColor->resource.get());
					} else {
						succeeded = nisScaler.Apply(globals::d3d::device, context,
							halfResolutionColor->srv.get(), upscaledNeuralColor->uav.get(), lowDesc.Width, lowDesc.Height,
							totalDesc.Width, totalDesc.Height, foveated.settings.neuralRenderingNisSharpness);
						if (succeeded)
							context->CopyResource(framebuffer, upscaledNeuralColor->resource.get());
					}
					if (succeeded)
						++act2BackendStats.nisSucceeded;
				}
			}
			if (succeeded) {
				lastFlatInputPrefilter = foveated.settings.neuralRenderingInputPrefilter;
				lastFlatMotionVectorMode = motionVectorMode;
				lastFlatResetEveryFrame = foveated.settings.neuralRenderingResetEveryFrame;
				lastFlatBypassOutput = foveated.settings.neuralRenderingBypassOutput;
				lastFlatOutputMode = outputMode;
				lastFlatAct1JitterMode = foveated.settings.flatAct1Enabled ?
					upscaling.settings.flatAct1JitterMode : UINT32_MAX;
				lastFlatAct2JitterMode = upscaling.settings.flatAct2JitterMode;
				lastAppliedFrame = frame;
				if (!writebackLogged) {
					if (foveated.settings.neuralRenderingHalfResolution) {
						D3D11_TEXTURE2D_DESC lowDesc{};
						halfResolutionColor->resource->GetDesc(&lowDesc);
						logger::info("[DLSSNR] Flat reduced-resolution resolve sourceGuides={}x{} nrGuides={}x{} nrColor={}x{} output={}x{} mvecScale={}x{} jitter=({:.4f},{:.4f}) mvMode={} mvMultiplier={:.2f} scale={} mode={} preset={} luma={:.2f} chroma={:.2f} edge={:.2f} limit={:.3f}",
							renderWidth, renderHeight, lowDesc.Width, lowDesc.Height, lowDesc.Width, lowDesc.Height,
							totalDesc.Width, totalDesc.Height, lowDesc.Width, lowDesc.Height,
							0.0f, 0.0f,
							motionVectorMode, motionVectorMultiplier,
							foveated.settings.neuralRenderingWorkingScale,
							foveated.settings.neuralRenderingOutputMode, foveated.settings.neuralRenderingFusionPreset,
							foveated.settings.neuralRenderingFusionLuma, foveated.settings.neuralRenderingFusionChroma,
							foveated.settings.neuralRenderingFusionEdgeProtection,
							foveated.settings.neuralRenderingFusionResidualLimit);
					} else {
						logger::info("[DLSSNR] Flat full-resolution output sourceGuides={}x{} nrGuides={}x{} color={}x{} mvecScale={}x{} jitter=(0,0) prefilter={} resetEveryFrame={} mvMode={} mvMultiplier={:.2f}",
							renderWidth, renderHeight, totalDesc.Width, totalDesc.Height,
							totalDesc.Width, totalDesc.Height, totalDesc.Width, totalDesc.Height,
							foveated.settings.neuralRenderingInputPrefilter,
							foveated.settings.neuralRenderingResetEveryFrame,
							motionVectorMode, motionVectorMultiplier);
					}
					writebackLogged = true;
				}
			}
			if (!succeeded && postInject) {
				context->CopyResource(framebuffer, color[0]->resource.get());
				lastAppliedFrame = frame;
				if (!flatPostInjectNrFallbackLogged) {
					logger::warn("[PostInject] NR/ACT2 failed after SR; writing the completed DLSS-SR output as fallback");
					flatPostInjectNrFallbackLogged = true;
				}
				return true;
			}

			return succeeded;
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
				(plan.act2SrBackend != Act2::kDLSS && plan.act2SrBackend != Act2::kLanczos);
			const bool eligible = requested && !unsupportedProvider;
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
					sourceGuideWidth, sourceGuideHeight, sourceGuideWidth, sourceGuideHeight, 1.0f);
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
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.neuralRenderingStyle),
				static_cast<std::uint64_t>(upscaling.foveatedRender.settings.neuralRenderingAutoMask),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.neuralRenderingIntensity)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.neuralRenderingLocalTone)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.neuralRenderingLocalStructure)),
				static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(upscaling.foveatedRender.settings.neuralRenderingSkinStructure)) })
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
						.act1Quality = plan.act1Quality,
						.act1Preset = plan.act1Preset,
						.act2Quality = plan.act2Quality,
						.act2Preset = upscaling.foveatedRender.settings.flatPipelineAct2Preset,
						.act1Camera = { act1Constants },
						.act2Camera = { act2Constants },
						.fusion = {
							.luma = upscaling.foveatedRender.settings.neuralRenderingFusionLuma,
							.chroma = upscaling.foveatedRender.settings.neuralRenderingFusionChroma,
							.edgeProtection = upscaling.foveatedRender.settings.neuralRenderingFusionEdgeProtection,
							.residualLimit = upscaling.foveatedRender.settings.neuralRenderingFusionResidualLimit,
							.direction = upscaling.foveatedRender.settings.flatPipelineFusionDirection,
							.denoise = upscaling.foveatedRender.settings.flatPipelineFusionDenoise,
							.denoiseStrength = upscaling.foveatedRender.settings.flatPipelineFusionDenoiseStrength,
							.softness = upscaling.foveatedRender.settings.flatPipelineFusionSoftness,
							.frequencyGuidance = upscaling.foveatedRender.settings.flatPipelineFusionFrequencyGuidance,
							.depthGuidance = upscaling.foveatedRender.settings.flatPipelineFusionDepthGuidance,
							.enhancedGuidance = upscaling.foveatedRender.settings.flatPipelineFusionEnhancedGuidance ? 1u : 0u,
							.colorGuidance = upscaling.foveatedRender.settings.flatPipelineFusionColorGuidance,
						},
						.tuning = GetTuning(upscaling.foveatedRender.settings),
						.motionVectorScaleX = static_cast<float>(plan.nrWidth),
						.motionVectorScaleY = static_cast<float>(plan.nrHeight),
						.sharpness = upscaling.foveatedRender.settings.flatPipelineSharpness,
						.sharpen = upscaling.foveatedRender.settings.flatPipelineSharpen,
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
		if (!globals::game::isVR || !FoveatedRenderImpl::Bridge::IsRouteActive() ||
			upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS ||
			foveated.GetDlssMode() != FoveatedRender::DlssMode::kDefault ||
			!foveated.settings.neuralRenderingEnabled || upscaling.IsFrameGenerationActive())
			return false;

		const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
		const std::uint32_t guideFrame = FoveatedRenderImpl::Core::neuralGuidesFrame;
		if (lastAppliedFrame == frame || (guideFrame != frame && !(frame > 0 && guideFrame == frame - 1)))
			return false;

		auto* renderer = globals::game::renderer;
		auto* context = globals::d3d::context;
		if (!renderer || !context || !globals::d3d::device ||
			!FoveatedRenderImpl::Core::vrSubrectDepth[0] || !FoveatedRenderImpl::Core::vrSubrectDepth[1] ||
			!FoveatedRenderImpl::Core::vrSubrectMotionVectors[0] || !FoveatedRenderImpl::Core::vrSubrectMotionVectors[1])
			return false;
		auto& total = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTOTAL];
		if (!total.texture)
			return false;

		D3D11_TEXTURE2D_DESC totalDesc{};
		total.texture->GetDesc(&totalDesc);
		const auto& leftUV = foveated.subrectController.GetUV();
		const auto& rightUV = foveated.subrectController.GetRightEyeUV();
		if (leftUV.w != rightUV.w || leftUV.h != rightUV.h)
			return false;
		const std::uint32_t eyeWidth = totalDesc.Width / 2;
		const std::uint32_t outWidth = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(eyeWidth * leftUV.w));
		const std::uint32_t outHeight = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(totalDesc.Height * leftUV.h));

		CS_GPU_PASS("NeuralRendering::FoveatedLdrBeforeUI");
		ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* savedDSV = nullptr;
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
		context->OMSetRenderTargets(0, nullptr, nullptr);

		const Util::Subrect::UVRegion* eyeUVs[2]{ &leftUV, &rightUV };
		std::array<Renderer::StereoEyeInput, 2> inputs{};
		for (std::uint32_t eye = 0; eye < 2; ++eye) {
			const auto& uv = *eyeUVs[eye];
			const std::uint32_t x = (eye ? eyeWidth : 0) + static_cast<std::uint32_t>(eyeWidth * uv.x);
			const std::uint32_t y = static_cast<std::uint32_t>(totalDesc.Height * uv.y);
			float motionScaleX = 1.0f;
			float motionScaleY = 1.0f;
			FoveatedRenderImpl::Bridge::ComputeMvecScale(eye, motionScaleX, motionScaleY);
			inputs[eye] = {
				.depth = FoveatedRenderImpl::Core::vrSubrectDepth[eye]->resource.get(),
				.depthSRV = FoveatedRenderImpl::Core::vrSubrectDepth[eye]->srv.get(),
				.motionVectors = FoveatedRenderImpl::Core::vrSubrectMotionVectors[eye]->resource.get(),
				.sourceX = x,
				.sourceY = y,
				.motionVectorScaleX = motionScaleX * FoveatedRenderImpl::Core::vrSubrectInW,
				.motionVectorScaleY = motionScaleY * FoveatedRenderImpl::Core::vrSubrectInH,
			};
		}
		const bool succeeded = Renderer::Instance().ApplyStereo(globals::d3d::device, context,
			total.texture, inputs, FoveatedRenderImpl::Core::vrSubrectInW, FoveatedRenderImpl::Core::vrSubrectInH,
			outWidth, outHeight, GetTuning(foveated.settings));
		if (succeeded) {
			lastAppliedFrame = frame;
			if (!writebackLogged) {
				logger::info("[DLSSNR] LDR output written before UI composite size={}x{} batchedAsync=true", outWidth, outHeight);
				writebackLogged = true;
			}
		}

		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
		for (auto*& rtv : savedRTVs)
			if (rtv) rtv->Release();
		if (savedDSV) savedDSV->Release();
		return succeeded;
	}

	Act2BackendStats GetAct2BackendStats()
	{
		return act2BackendStats;
	}

	void Reset()
	{
		Renderer::Instance().Reset();
		color[0].reset();
		color[1].reset();
		halfResolutionColor.reset();
		upscaledNeuralColor.reset();
		fusionOutput.reset();
		resizedDepthGuide.reset();
		resizedMotionGuide.reset();
		act2ReactiveMask.reset();
		act2TransparencyMask.reset();
		flatAct1Output.reset();
		flatAct2Output.reset();
		flatFusionReference.reset();
		flatSharpenOutput.reset();
		nisScaler.Reset();
		act1FsrScaler.Reset();
		act2FsrScaler.Reset();
		downsampleCS.Reset();
		fusionCS.Reset();
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
		flatFrameGenerationBlockLogged = false;
		flatHdrBlockLogged = false;
		flatPostInjectLogged = false;
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
		lastModel3Eligible = false;
		model3RestrictionLogged = false;
		act2BackendStats = {};
	}
}
