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
			std::uint32_t lowWidth;
			std::uint32_t lowHeight;
			float lumaStrength;
			float chromaStrength;
			float edgeProtection;
			float residualLimit;
			std::uint32_t outputMode;
			float padding[3];
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

		bool ResolveNeuralRendering(ID3D11DeviceContext* context, ID3D11ShaderResourceView* referenceColor,
			std::uint32_t width, std::uint32_t height, const FoveatedRender::Settings& settings)
		{
			auto* shader = fusionCS.Get(
				L"Data\\Shaders\\Upscaling\\NeuralRendering\\ResolveFusionCS.hlsl", {}, "cs_5_0",
				"main", "NeuralRendering::ResolveFusionCS");
			if (!shader || !referenceColor || !upscaledNeuralColor || !fusionOutput)
				return false;
			const FusionConstants constants{
				width, height, width, height,
				settings.neuralRenderingFusionLuma,
				settings.neuralRenderingFusionChroma,
				settings.neuralRenderingFusionEdgeProtection,
				settings.neuralRenderingFusionResidualLimit,
				settings.neuralRenderingOutputMode,
				{},
			};
			if (!UpdateConstantBuffer(context, fusionCB.get(), constants))
				return false;

			context->CSSetShader(shader, nullptr, 0);
			ID3D11Buffer* buffers[]{ fusionCB.get() };
			context->CSSetConstantBuffers(0, 1, buffers);
			ID3D11ShaderResourceView* srvs[]{ referenceColor, upscaledNeuralColor->srv.get() };
			context->CSSetShaderResources(0, 2, srvs);
			ID3D11UnorderedAccessView* uavs[]{ fusionOutput->uav.get() };
			context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
			context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

			ID3D11Buffer* nullBuffer[]{ nullptr };
			ID3D11ShaderResourceView* nullSRVs[]{ nullptr, nullptr };
			ID3D11UnorderedAccessView* nullUAV[]{ nullptr };
			context->CSSetConstantBuffers(0, 1, nullBuffer);
			context->CSSetShaderResources(0, 2, nullSRVs);
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
			using NrMode = FoveatedRender::FlatNrMode;
			using Act2 = FoveatedRender::FlatAct2Backend;

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
				static_cast<std::uint64_t>(plan.act1Backend), static_cast<std::uint64_t>(plan.nrMode),
				static_cast<std::uint64_t>(plan.act2Backend), static_cast<std::uint64_t>(plan.act1Dsr),
				static_cast<std::uint64_t>(plan.nrDsr), static_cast<std::uint64_t>(upscaling.settings.flatAct1JitterMode),
				static_cast<std::uint64_t>(upscaling.settings.flatAct2JitterMode) })
				hashCombine(planSignature, value);
			const bool planChanged = routeActivated || lastFlatPlanSignature != planSignature;
			if (planChanged) {
				Renderer::Instance().ResetHistory();
				Upscaling::streamline.DestroyDLSSResources();
				act1FsrScaler.Reset();
				act2FsrScaler.Reset();
				lastFlatPlanSignature = planSignature;
				logger::info("[FlatPipeline] Plan signature=0x{:016X} D={}x{} R={}x{} ACT1={} {}x{} N={} {}x{} NRout={}x{} ACT2={} {}x{} RCAS={}",
					planSignature, plan.displayWidth, plan.displayHeight, plan.engineWidth, plan.engineHeight,
					static_cast<uint>(plan.act1Backend), plan.act1OutputWidth, plan.act1OutputHeight,
					static_cast<uint>(plan.nrMode), plan.nrWidth, plan.nrHeight, plan.nrOutputWidth, plan.nrOutputHeight,
					static_cast<uint>(plan.act2Backend), plan.act2OutputWidth, plan.act2OutputHeight,
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

			if (!EnsureColorResources(framebuffer, plan.displayWidth, plan.displayHeight) ||
				!EnsureFusionResources(framebuffer, plan.displayWidth, plan.displayHeight, plan.nrWidth, plan.nrHeight))
				return false;
			context->CopyResource(color[0]->resource.get(), framebuffer);
			ID3D11ShaderResourceView* fallbackSRV = color[0]->srv.get();
			ID3D11Resource* fallbackResource = color[0]->resource.get();
			uint32_t fallbackWidth = plan.displayWidth;
			uint32_t fallbackHeight = plan.displayHeight;
			bool outputCommitted = false;
			const SKSE::stl::scope_exit resolveFallback([&]() noexcept {
				if (outputCommitted || !fallbackResource)
					return;
				if (fallbackWidth == plan.displayWidth && fallbackHeight == plan.displayHeight) {
					context->CopyResource(framebuffer, fallbackResource);
				} else if (fallbackSRV && upscaledNeuralColor &&
					ResampleLanczos(context, fallbackSRV, upscaledNeuralColor->uav.get(),
						fallbackWidth, fallbackHeight, plan.displayWidth, plan.displayHeight)) {
					context->CopyResource(framebuffer, upscaledNeuralColor->resource.get());
				}
			});

			ID3D11ShaderResourceView* act1SourceSRV = color[0]->srv.get();
			ID3D11Resource* act1SourceResource = color[0]->resource.get();
			uint32_t act1SourceWidth = plan.displayWidth;
			uint32_t act1SourceHeight = plan.displayHeight;
			bool succeeded = true;

			if (plan.act1Backend != Act1::kBypass) {
				auto& streamline = Upscaling::streamline;
				succeeded = streamline.PrepareFlatInputIsolation(context, framebuffer, depth.depthSRV,
					upscaling.motionVectorCopyTexture->resource.get(), upscaling.reactiveMaskTexture->resource.get(),
					upscaling.transparencyCompositionMaskTexture->resource.get(), plan.engineWidth, plan.engineHeight) &&
					EnsurePipelineTexture(flatAct1Output, framebuffer, plan.act1OutputWidth, plan.act1OutputHeight,
						"NeuralRendering::FlatPipelineAct1Output");
				if (!succeeded)
					return false;
					++act2BackendStats.dlssSucceeded;

				if (plan.act1Backend == Act1::kDLSS) {
					const sl::Extent inputExtent{ 0, 0, plan.engineWidth, plan.engineHeight };
					const sl::Extent outputExtent{ 0, 0, plan.act1OutputWidth, plan.act1OutputHeight };
					const uint quality = plan.act1Dsr ? 4u : foveated.settings.flatPipelineAct1Quality;
					succeeded = streamline.EvaluateDLSS(streamline.flatPostInjectViewport, 0,
						streamline.flatIsolatedColor->resource.get(), flatAct1Output->resource.get(),
						streamline.flatIsolatedDepth->resource.get(), streamline.flatIsolatedMotionVectors->resource.get(),
						streamline.flatIsolatedReactiveMask->resource.get(), streamline.flatIsolatedTransparencyMask->resource.get(),
						inputExtent, outputExtent, plan.act1OutputWidth, plan.act1OutputHeight,
						Streamline::DLSSModeForQualityMode(quality), false, planChanged,
						upscaling.settings.flatAct1JitterMode == 1);
				} else {
					succeeded = act1FsrScaler.Apply(globals::d3d::device, context,
						streamline.flatIsolatedColor->resource.get(), streamline.flatIsolatedDepth->resource.get(),
						streamline.flatIsolatedMotionVectors->resource.get(), streamline.flatIsolatedReactiveMask->resource.get(),
						streamline.flatIsolatedTransparencyMask->resource.get(), flatAct1Output->resource.get(),
						plan.engineWidth, plan.engineHeight, plan.act1OutputWidth, plan.act1OutputHeight,
						static_cast<float>(plan.engineWidth), static_cast<float>(plan.engineHeight),
						upscaling.settings.flatAct1JitterMode == 0 ? upscaling.jitter.x : 0.0f,
						upscaling.settings.flatAct1JitterMode == 0 ? upscaling.jitter.y : 0.0f,
						0.0f, planChanged);
				}
				if (!succeeded)
					return false;
					++act2BackendStats.fsrSucceeded;
				act1SourceSRV = flatAct1Output->srv.get();
				act1SourceResource = flatAct1Output->resource.get();
				act1SourceWidth = plan.act1OutputWidth;
				act1SourceHeight = plan.act1OutputHeight;
				fallbackSRV = act1SourceSRV;
				fallbackResource = act1SourceResource;
				fallbackWidth = act1SourceWidth;
				fallbackHeight = act1SourceHeight;
			}

			if (!ResampleLanczos(context, act1SourceSRV, halfResolutionColor->uav.get(),
					act1SourceWidth, act1SourceHeight, plan.nrWidth, plan.nrHeight))
				return false;
			if (!EnsureGuideResources(upscaling.motionVectorCopyTexture->resource.get(), plan.nrWidth, plan.nrHeight) ||
				!ResizeGuides(context, depth.depthSRV, upscaling.motionVectorCopyTexture->srv.get(),
					plan.engineWidth, plan.engineHeight, plan.nrWidth, plan.nrHeight, 1.0f))
				return false;

			ID3D11Resource* finalResource = nullptr;
			ID3D11ShaderResourceView* finalSRV = nullptr;
			if (plan.nrMode == NrMode::kNRSR) {
				if (!EnsurePipelineTexture(flatAct2Output, framebuffer, plan.displayWidth, plan.displayHeight,
						"NeuralRendering::FlatPipelineNrSrOutput"))
					return false;
				succeeded = Renderer::Instance().ApplyToOutput(globals::d3d::device, context, 0,
					halfResolutionColor->resource.get(), flatAct2Output->resource.get(),
					resizedDepthGuide->resource.get(), resizedDepthGuide->srv.get(), resizedMotionGuide->resource.get(),
					plan.nrWidth, plan.nrHeight, plan.nrWidth, plan.nrHeight,
					plan.displayWidth, plan.displayHeight, static_cast<float>(plan.engineWidth),
					static_cast<float>(plan.engineHeight), GetTuning(foveated.settings), planChanged);
				if (succeeded) {
					finalResource = flatAct2Output->resource.get();
					finalSRV = flatAct2Output->srv.get();
				}
			} else {
				succeeded = Renderer::Instance().Apply(globals::d3d::device, context, 0,
					halfResolutionColor->resource.get(), resizedDepthGuide->resource.get(), resizedDepthGuide->srv.get(),
					resizedMotionGuide->resource.get(), plan.nrWidth, plan.nrHeight, plan.nrWidth, plan.nrHeight,
					static_cast<float>(plan.engineWidth), static_cast<float>(plan.engineHeight),
					GetTuning(foveated.settings), planChanged);
			}
			if (!succeeded)
				return false;
			if (plan.nrMode == NrMode::kNR) {
				fallbackSRV = halfResolutionColor->srv.get();
				fallbackResource = halfResolutionColor->resource.get();
				fallbackWidth = plan.nrWidth;
				fallbackHeight = plan.nrHeight;
			}

			if (plan.nrMode == NrMode::kNR && (plan.act2Backend == Act2::kDLSS || plan.act2Backend == Act2::kFSR3)) {
				if (!EnsurePipelineTexture(flatAct2Output, framebuffer, plan.act2OutputWidth, plan.act2OutputHeight,
						"NeuralRendering::FlatPipelineAct2Output") ||
					!EnsureAct2MaskResources(upscaling.reactiveMaskTexture->resource.get(),
						upscaling.transparencyCompositionMaskTexture->resource.get(), plan.nrWidth, plan.nrHeight))
					return false;
				if (plan.act2Backend == Act2::kDLSS) {
					const sl::Extent inputExtent{ 0, 0, plan.nrWidth, plan.nrHeight };
					const sl::Extent outputExtent{ 0, 0, plan.act2OutputWidth, plan.act2OutputHeight };
					const uint quality = plan.nrDsr ? 4u : foveated.settings.flatPipelineNrQuality;
					succeeded = Upscaling::streamline.EvaluateDLSS(Upscaling::streamline.flatAct2Viewport, 0,
						halfResolutionColor->resource.get(), flatAct2Output->resource.get(),
						resizedDepthGuide->resource.get(), resizedMotionGuide->resource.get(),
						act2ReactiveMask->resource.get(), act2TransparencyMask->resource.get(),
						inputExtent, outputExtent, plan.act2OutputWidth, plan.act2OutputHeight,
						Streamline::DLSSModeForQualityMode(quality), false, planChanged,
						upscaling.settings.flatAct2JitterMode == 1,
						static_cast<float>(plan.nrWidth) / static_cast<float>(plan.engineWidth),
						static_cast<float>(plan.nrHeight) / static_cast<float>(plan.engineHeight));
				} else {
					succeeded = act2FsrScaler.Apply(globals::d3d::device, context,
						halfResolutionColor->resource.get(), resizedDepthGuide->resource.get(), resizedMotionGuide->resource.get(),
						act2ReactiveMask->resource.get(), act2TransparencyMask->resource.get(), flatAct2Output->resource.get(),
						plan.nrWidth, plan.nrHeight, plan.act2OutputWidth, plan.act2OutputHeight,
						static_cast<float>(plan.engineWidth), static_cast<float>(plan.engineHeight),
						upscaling.settings.flatAct2JitterMode == 0 ? upscaling.jitter.x * plan.nrWidth / plan.engineWidth : 0.0f,
						upscaling.settings.flatAct2JitterMode == 0 ? upscaling.jitter.y * plan.nrHeight / plan.engineHeight : 0.0f,
						0.0f, planChanged);
				}
				if (!succeeded)
					return false;
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
			} else if (plan.nrMode == NrMode::kNR && plan.act2Backend == Act2::kFusion) {
				ID3D11ShaderResourceView* fusionReferenceSRV = act1SourceSRV;
				if (act1SourceWidth != plan.displayWidth || act1SourceHeight != plan.displayHeight) {
					if (!EnsurePipelineTexture(flatFusionReference, framebuffer, plan.displayWidth, plan.displayHeight,
							"NeuralRendering::FlatPipelineFusionReference") ||
						!ResampleLanczos(context, act1SourceSRV, flatFusionReference->uav.get(),
							act1SourceWidth, act1SourceHeight, plan.displayWidth, plan.displayHeight))
						return false;
					fusionReferenceSRV = flatFusionReference->srv.get();
				}
				if (!nisScaler.Apply(globals::d3d::device, context, halfResolutionColor->srv.get(), upscaledNeuralColor->uav.get(),
						plan.nrWidth, plan.nrHeight, plan.displayWidth, plan.displayHeight, 0.0f) ||
					!ResolveNeuralRendering(context, fusionReferenceSRV,
						plan.displayWidth, plan.displayHeight, foveated.settings))
					return false;
				finalResource = fusionOutput->resource.get();
				finalSRV = fusionOutput->srv.get();
			} else if (plan.nrMode == NrMode::kNR && plan.act2Backend == Act2::kNIS) {
				if (!nisScaler.Apply(globals::d3d::device, context, halfResolutionColor->srv.get(), upscaledNeuralColor->uav.get(),
						plan.nrWidth, plan.nrHeight, plan.displayWidth, plan.displayHeight, 0.0f))
					return false;
				++act2BackendStats.nisSucceeded;
				finalResource = upscaledNeuralColor->resource.get();
				finalSRV = upscaledNeuralColor->srv.get();
			} else if (plan.nrMode == NrMode::kNR) {
				if (!ResampleLanczos(context, halfResolutionColor->srv.get(), upscaledNeuralColor->uav.get(),
						plan.nrWidth, plan.nrHeight, plan.displayWidth, plan.displayHeight))
					return false;
				finalResource = upscaledNeuralColor->resource.get();
				finalSRV = upscaledNeuralColor->srv.get();
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
	}

	bool ApplyFoveatedLdr()
	{
		auto& upscaling = globals::features::upscaling;
		auto& foveated = upscaling.foveatedRender;
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
		act2BackendStats = {};
	}
}
