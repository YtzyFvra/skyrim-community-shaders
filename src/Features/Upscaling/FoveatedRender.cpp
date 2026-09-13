#include "FoveatedRender.h"

#include "../../Globals.h"
#include "../../I18n/I18n.h"
#include "../../Utils/D3D.h"
#include "../../Utils/Subrect.h"
#include "../../Utils/UI.h"
#include "../FoveatedCommon.h"
#include "../Upscaling.h"
#include "FoveatedRender/Core.h"
#include "GpuPass.h"
#include "NeuralRendering/Integration.h"
#include "NeuralRendering/ArisePipeline.h"
#include "NeuralRendering/Renderer.h"
#include "VrSubmitDlssg.h"
#include "Profiler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#define I18N_KEY_PREFIX "feature.upscaling."

#define OS_FOV_SETTINGS_FIELD(X)                  \
	X(enabled)                                    \
	X(dlssMode)                                   \
	X(stretchMode)                                \
	X(peripheryBlurRadius)                        \
	X(debugVisualize)                             \
	X(peripheryAAMode)                            \
	X(peripheryTemporalAlpha)                     \
	X(subrectBlendMode)                           \
	X(subrectMaskMode)                            \
	X(subrectFeatherWidth)                        \
	X(subrectFalloffCurve)                        \
	X(subrectDitherStrength)                      \
	X(neuralRenderingEnabled)                     \
	X(ariseShadowScheduler)                       \
	X(ariseVisualValidation)                      \
	X(ariseWorkerNr)                              \
	X(ariseModel3)                                \
	X(singleThreadPerfMode)                       \
	X(ariseBufferCount)                           \
	X(flatPipelineAct1Backend)                    \
	X(flatPipelineAct1Quality)                    \
	X(flatPipelineAct1Preset)                     \
	X(flatPipelineAct1Jitter)                     \
	X(flatPipelineAct1Dsr)                        \
	X(flatPipelineNrScale)                        \
	X(flatPipelineNrNative)                       \
	X(flatPipelineAct2Backend)                    \
	X(unifiedPipeline)                            \
	X(flatPipelineAct2Preset)                     \
	X(flatPipelineAct2Jitter)                     \
	X(flatPipelineFusionDirection)                \
	X(flatPipelineFusionDenoise)                  \
	X(flatPipelineFusionDenoiseStrength)          \
	X(flatPipelineFusionSoftness)                 \
	X(flatPipelineFusionEnhancedGuidance)         \
	X(flatPipelineFusionFrequencyGuidance)        \
	X(flatPipelineFusionDepthGuidance)            \
	X(flatPipelineFusionColorGuidance)            \
	X(flatPipelineSharpen)                        \
	X(flatPipelineSharpness)                      \
	X(flatAct1Enabled)                            \
	X(flatAct1QualityMode)                        \
	X(flatAct1Preset)                             \
	X(flatAct1Sharpness)                          \
	X(flatAct2QualityMode)                        \
	X(flatAct2Preset)                             \
	X(flatAct2Sharpness)                          \
	X(neuralRenderingPreset)                      \
	X(neuralRenderingIntensity)                   \
	X(neuralRenderingLocalTone)                   \
	X(neuralRenderingLocalStructure)              \
	X(neuralRenderingSkinStructure)               \
	X(neuralRenderingStyle)                       \
	X(neuralRenderingAutoMask)                    \
	X(neuralRenderingUICorrection)                \
	X(neuralRenderingBypassOutput)                \
	X(neuralRenderingResetEveryFrame)             \
	X(neuralRenderingInputPrefilter)              \
	X(neuralRenderingMotionVectorMode)            \
	X(neuralRenderingHalfResolution)              \
	X(neuralRenderingWorkingScale)                \
	X(neuralRenderingOutputMode)                  \
	X(neuralRenderingNisSharpness)                \
	X(neuralRenderingFusionPreset)                \
	X(neuralRenderingFusionLuma)                  \
	X(neuralRenderingFusionChroma)                \
	X(neuralRenderingFusionEdgeProtection)        \
	X(neuralRenderingFusionResidualLimit)         \
	X(fusionDepthDilate)                          \
	X(fusionGEdge)                                \
	X(fusionNEdge)                                \
	X(fusionEdgePower)                            \
	X(fusionHfLumaSuppress)                       \
	X(fusionHfTowardN)                            \
	X(fusionNcDilate)                             \
	X(fusionAlignG)                               \
	X(fusionCompareMode)                          \
	X(fusionDebugView)                            \
	X(fusionToneGainLuma)                         \
	X(fusionToneGainChroma)                       \
	X(fusionDetailGain)                           \
	X(fusionDither)                               \
	X(fusionGainPrior)                            \
	X(fusionWedge)                                \
	X(fusionWedgeConstCount)                      \
	X(fusionEdgeBlockGThresh)                     \
	X(fusionFitSideSigma)                         \
	X(fusionDirectionalSharpen)                   \
	X(fusionSoftEdge)                             \
	X(act3Enabled)                                \
	X(act3Scale)                                  \
	X(act3Preset)                                 \
	X(act3Intensity)                              \
	X(act3LocalTone)                              \
	X(act3LocalStructure)                         \
	X(act3SkinStructure)                          \
	X(act3Style)                                  \
	X(act3AutoMask)                               \
	X(act3UICorrection)                           \
	X(act3ResetEveryFrame)                        \
	X(act3Sharpen)                                \
	X(act3Sharpness)                              \
	X(act3FeatherWidth)                           \
	X(act3DitherStrength)                         \
	X(act3GuidanceEnabled)                        \
	X(act3GuidanceLuma)                           \
	X(act3GuidanceChroma)                         \
	X(act3GuidancePrior)                          \
	X(act3GuidanceMaxGain)                        \
	X(act3GuidanceToneLuma)                       \
	X(act3GuidanceToneChroma)                     \
	X(act3GuidanceDetailGain)                     \
	X(act3GuidanceDither)                         \
	X(act3GuidanceDirSharpen)

void to_json(nlohmann::json& j, const FoveatedRender::Settings& s)
{
	j = nlohmann::json::object();
#define OS_FOV_WRITE(field) j[#field] = s.field;
	OS_FOV_SETTINGS_FIELD(OS_FOV_WRITE)
#undef OS_FOV_WRITE
}

void from_json(const nlohmann::json& j, FoveatedRender::Settings& s)
{
	const FoveatedRender::Settings defaults{};
#define OS_FOV_READ(field) s.field = j.value(#field, defaults.field);
	OS_FOV_SETTINGS_FIELD(OS_FOV_READ)
#undef OS_FOV_READ
}

#undef OS_FOV_SETTINGS_FIELD

// ============================================================================
// Lifecycle
// ============================================================================

void FoveatedRender::PostPostLoad()
{
	bootSnapshot.LatchIfNeeded(settings);
	unifiedBootSnapshot.LatchIfNeeded(settings.unifiedPipeline);

	// Opt into the stereo extension so the controller tracks a separate
	// right-eye UV (HMD nose-side overlap symmetry).
	subrectController.SetStereoEnabled(true);

	// Seed sensible foveal presets. Empty-case only — user edits persist.
	// "Center N%" presets are symmetric per eye (no rightUV → auto-mirror, which
	// for centered UVs produces an identical right-eye UV). "Nasal Convergence"
	// is asymmetric: left eye biased toward its right edge, right eye biased
	// toward its left edge — both targeting the nose-side region where HMD
	// binocular fusion is strongest, so DLSS reconstruction lands in the actual
	// stereo overlap zone rather than diverging left/right fields.
	subrectController.SeedDefaultPresets({
		{ .name = kPresetFullEye, .uv = { 0.0f, 0.0f, 1.0f, 1.0f } },
		{ .name = kPresetCenter75, .uv = { 0.125f, 0.125f, 0.75f, 0.75f } },
		{ .name = kPresetCenter50, .uv = { 0.25f, 0.25f, 0.5f, 0.5f } },
		{ .name = kPresetNasalConvergence50,
			.uv = { 0.5f, 0.25f, 0.5f, 0.5f },
			.rightUV = Util::Subrect::UVRegion{ 0.0f, 0.25f, 0.5f, 0.5f } },
		{ .name = kPresetNasalConvergence60,
			.uv = { 0.4f, 0.2f, 0.6f, 0.6f },
			.rightUV = Util::Subrect::UVRegion{ 0.0f, 0.2f, 0.6f, 0.6f } },
		{ .name = kPresetNasalConvergence70,
			.uv = { 0.3f, 0.15f, 0.7f, 0.7f },
			.rightUV = Util::Subrect::UVRegion{ 0.0f, 0.15f, 0.7f, 0.7f } },
	});
	// PostPostLoad runs after settings load, so a user with an older, shorter
	// persisted preset list (from before these names existed) still sees every
	// current preset in the DrawEditor dropdown, not just whichever ones they
	// happened to click as buttons.
	subrectController.MaterializeNewDefaults();
	NeuralRendering::ARISE::Pipeline::Instance().Install();
	VrSubmitDlssg::Install();
	NeuralRendering::ARISE::Pipeline::Instance().SetSlotCount(settings.ariseBufferCount);
	NeuralRendering::ARISE::Pipeline::Instance().SetVisualValidation(settings.ariseVisualValidation);
	NeuralRendering::ARISE::Pipeline::Instance().SetWorkerNrEnabled(settings.ariseWorkerNr);
	NeuralRendering::ARISE::Pipeline::Instance().SetEnabled(settings.ariseShadowScheduler);
	stl::write_vfunc<0x1, UICompositeRenderHook>(RE::VTABLE_BSImagespaceShaderCopyDynamicFetchDisabled[3]);
	if (globals::game::isVR) {
		stl::write_vfunc<0x1, HDRTonemapRenderHook>(RE::VTABLE_BSImagespaceShaderHDRTonemapBlendCinematic[3]);
		stl::write_vfunc<0x1, HDRTonemapFadeRenderHook>(RE::VTABLE_BSImagespaceShaderHDRTonemapBlendCinematicFade[3]);
	} else if (WantsLogicalPerfMode()) {
		// Title/loading blit + Map/ISHDR scopes. ESC/TAB stay unpatched — see docs/development/flat-arise-menu.md.
		stl::write_vfunc<0x1, MapRenderHook>(RE::VTABLE_BSImagespaceShaderMap[3]);
		stl::write_vfunc<0x1, WorldMapRenderHook>(RE::VTABLE_BSImagespaceShaderWorldMap[3]);
		stl::write_vfunc<0x1, WorldMapNoSkyBlurRenderHook>(RE::VTABLE_BSImagespaceShaderWorldMapNoSkyBlur[3]);
		stl::write_vfunc<0x1, HDRTonemapRenderHook>(RE::VTABLE_BSImagespaceShaderHDRTonemapBlendCinematic[3]);
		stl::write_vfunc<0x1, HDRTonemapFadeRenderHook>(RE::VTABLE_BSImagespaceShaderHDRTonemapBlendCinematicFade[3]);
		stl::detour_thunk<RendererUpdateViewPortHook>(REL::RelocationID(75455, 77240));
	}
	stl::write_vfunc<0x1, DistantBlurRenderHook>(RE::VTABLE_BSImagespaceShaderDistantBlur[3]);
	stl::write_vfunc<0x1, DistantBlurFoggedRenderHook>(RE::VTABLE_BSImagespaceShaderDistantBlurFogged[3]);
	stl::write_vfunc<0x1, DistantBlurMaskedFoggedRenderHook>(RE::VTABLE_BSImagespaceShaderDistantBlurMaskedFogged[3]);
}

void FoveatedRender::UICompositeRenderHook::thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	func(imageSpaceShader, shape, param);
}

namespace
{
	thread_local std::uint32_t logicalTargetRenderDepth = 0;

	class LogicalTargetRenderScope
	{
	public:
		LogicalTargetRenderScope() { ++logicalTargetRenderDepth; }
		~LogicalTargetRenderScope() { --logicalTargetRenderDepth; }
	};

	bool ViewportMatchesExtent(const D3D11_VIEWPORT& viewport, UINT width, UINT height)
	{
		return std::abs(viewport.Width - static_cast<float>(width)) <= 1.0f &&
		       std::abs(viewport.Height - static_cast<float>(height)) <= 1.0f;
	}

	bool TryLogicalViewportFromPhysicalLeak(D3D11_VIEWPORT& viewport)
	{
		auto& swapChain = globals::features::upscaling.dx12SwapChain;
		if (!swapChain.useArise ||
			(swapChain.PhysicalWidth() == swapChain.LogicalWidth() &&
				swapChain.PhysicalHeight() == swapChain.LogicalHeight()))
			return false;
		if (!ViewportMatchesExtent(viewport, swapChain.PhysicalWidth(), swapChain.PhysicalHeight()))
			return false;
		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;
		viewport.Width = static_cast<float>(swapChain.LogicalWidth());
		viewport.Height = static_cast<float>(swapChain.LogicalHeight());
		return true;
	}

	struct NamedExtent
	{
		const char* name = "other";
		UINT width = 0;
		UINT height = 0;
	};

	NamedExtent DescribeResource(ID3D11Resource* resource)
	{
		NamedExtent result;
		if (!resource)
			return { "null", 0, 0 };
		winrt::com_ptr<ID3D11Texture2D> texture;
		if (SUCCEEDED(resource->QueryInterface(texture.put()))) {
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			result.width = desc.Width;
			result.height = desc.Height;
		}
		auto& swapChain = globals::features::upscaling.dx12SwapChain;
		if (swapChain.swapChainBufferWrapped && resource == swapChain.swapChainBufferWrapped->resource11)
			result.name = "SwapChainBuffer";
		else if (swapChain.uiBufferWrapped && resource == swapChain.uiBufferWrapped->resource11)
			result.name = "UIBuffer";
		else if (auto* renderer = globals::game::renderer) {
			auto& targets = renderer->GetRuntimeData().renderTargets;
			const struct
			{
				RE::RENDER_TARGET id;
				const char* name;
			} slots[] = {
				{ RE::RENDER_TARGET::kFRAMEBUFFER, "kFRAMEBUFFER" },
				{ RE::RENDER_TARGET::kMENUBG, "kMENUBG" },
				{ RE::RENDER_TARGET::kMAIN, "kMAIN" },
				{ RE::RENDER_TARGET::kMAIN_COPY, "kMAIN_COPY" },
				{ RE::RENDER_TARGET::kIMAGESPACE_TEMP_COPY, "kIMAGESPACE_TEMP_COPY" },
				{ RE::RENDER_TARGET::kTOTAL, "kTOTAL" },
			};
			for (const auto& slot : slots) {
				if (targets[slot.id].texture == resource) {
					result.name = slot.name;
					break;
				}
			}
		}
		return result;
	}

	NamedExtent DescribeRTV(ID3D11RenderTargetView* view)
	{
		if (!view)
			return { "null", 0, 0 };
		winrt::com_ptr<ID3D11Resource> resource;
		view->GetResource(resource.put());
		return DescribeResource(resource.get());
	}

	NamedExtent DescribeEngineSlot(RE::RENDER_TARGET id)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer)
			return { "null", 0, 0 };
		auto& target = renderer->GetRuntimeData().renderTargets[id];
		if (target.texture)
			return DescribeResource(target.texture);
		return DescribeRTV(target.RTV);
	}

	void LogMenuPipelineImpl(const char* stage)
	{
		auto& upscaling = globals::features::upscaling;
		auto& swapChain = upscaling.dx12SwapChain;
		auto* state = globals::state;
		auto* context = globals::d3d::context;
		auto* viewport = globals::game::graphicsState;
		winrt::com_ptr<ID3D11RenderTargetView> destView;
		winrt::com_ptr<ID3D11ShaderResourceView> sourceView;
		if (context) {
			context->OMGetRenderTargets(1, destView.put(), nullptr);
			ID3D11ShaderResourceView* rawSource = nullptr;
			context->PSGetShaderResources(0, 1, &rawSource);
			sourceView.attach(rawSource);
		}
		const auto dest = DescribeRTV(destView.get());
		NamedExtent source{ "none", 0, 0 };
		if (sourceView) {
			winrt::com_ptr<ID3D11Resource> sourceResource;
			sourceView->GetResource(sourceResource.put());
			source = DescribeResource(sourceResource.get());
		}
		UINT viewportCount = 1;
		D3D11_VIEWPORT currentViewport{};
		if (context)
			context->RSGetViewports(&viewportCount, &currentViewport);
		const auto framebuffer = DescribeEngineSlot(RE::RENDER_TARGET::kFRAMEBUFFER);
		const auto menuBackground = DescribeEngineSlot(RE::RENDER_TARGET::kMENUBG);
		const auto mainColor = DescribeEngineSlot(RE::RENDER_TARGET::kMAIN);
		NamedExtent framebufferRTV{ "null", 0, 0 };
		if (auto* renderer = globals::game::renderer)
			framebufferRTV = DescribeRTV(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER].RTV);
		const bool uiBufferRedirect = swapChain.useArise ||
			(upscaling.ShouldUseFrameGenerationThisFrame() && !upscaling.UsesDLSSGFrameGen());
		logger::info(
			"[MENUCOPY] stage={} frame={} dest={} {}x{} src={} {}x{} vp={:.0f}x{:.0f} gfx={}x{} screenSize={:.0f}x{:.0f} R={}x{} D={}x{} fbTex={} {}x{} fbRTV={} {}x{} menuBG={} {}x{} main={} {}x{} fgAllow={} fgThis={} fgActive={} useArise={} useDLSSG={} uiBuf={} world={} mainMenu={} load={} paused={}",
			stage,
			state ? state->frameCount : 0u,
			dest.name, dest.width, dest.height,
			source.name, source.width, source.height,
			currentViewport.Width, currentViewport.Height,
			viewport ? viewport->screenWidth : 0u, viewport ? viewport->screenHeight : 0u,
			state ? state->screenSize.x : 0.0f, state ? state->screenSize.y : 0.0f,
			swapChain.LogicalWidth(), swapChain.LogicalHeight(),
			swapChain.PhysicalWidth(), swapChain.PhysicalHeight(),
			framebuffer.name, framebuffer.width, framebuffer.height,
			framebufferRTV.name, framebufferRTV.width, framebufferRTV.height,
			menuBackground.name, menuBackground.width, menuBackground.height,
			mainColor.name, mainColor.width, mainColor.height,
			upscaling.settings.frameGenerationAllowInMenus ? 1 : 0,
			upscaling.ShouldUseFrameGenerationThisFrame() ? 1 : 0,
			upscaling.IsFrameGenerationActive() ? 1 : 0,
			swapChain.useArise ? 1 : 0,
			swapChain.useDLSSG ? 1 : 0,
			uiBufferRedirect ? 1 : 0,
			state && state->worldRenderedThisFrame ? 1 : 0,
			state && state->isMainMenuOpen ? 1 : 0,
			state && state->isLoadingMenuOpen ? 1 : 0,
			state && state->IsPausedOrMenuOpen(globals::game::ui) ? 1 : 0);
	}

	ID3D11PixelShader* MenuBackgroundBlitPS()
	{
		static winrt::com_ptr<ID3D11PixelShader> blitPS;
		if (!blitPS) {
			blitPS.attach(static_cast<ID3D11PixelShader*>(
				Util::CompileShader(L"Data/Shaders/Upscaling/PerfMode/MenuBGBlitPS.hlsl",
					{ { "PSHADER", "" } }, "ps_5_0")));
		}
		return blitPS.get();
	}

	ID3D11SamplerState* MenuBackgroundBlitSampler()
	{
		static winrt::com_ptr<ID3D11SamplerState> linearSampler;
		if (!linearSampler && globals::d3d::device) {
			D3D11_SAMPLER_DESC samplerDesc{};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.MaxAnisotropy = 1;
			samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
			globals::d3d::device->CreateSamplerState(&samplerDesc, linearSampler.put());
		}
		return linearSampler.get();
	}

	void BlitSrvToSwapChain(ID3D11ShaderResourceView* source)
	{
		auto* context = globals::d3d::context;
		auto& swapChain = globals::features::upscaling.dx12SwapChain;
		auto* destination = swapChain.swapChainBufferWrapped ? swapChain.swapChainBufferWrapped->rtv : nullptr;
		auto* vertexShader = globals::features::upscaling.GetUpscaleVS();
		auto* pixelShader = MenuBackgroundBlitPS();
		auto* sampler = MenuBackgroundBlitSampler();
		D3D11_TEXTURE2D_DESC destinationDesc{};
		if (!context || !source || !destination || !vertexShader || !pixelShader || !sampler ||
			!Util::GetTexture2DDesc(destination, destinationDesc))
			return;

		CS_GPU_PASS("FoveatedRender::SwapChainBackgroundBlit");
		Util::FullscreenPassScope stateScope(context);
		D3D11_VIEWPORT viewport{};
		viewport.Width = static_cast<float>(destinationDesc.Width);
		viewport.Height = static_cast<float>(destinationDesc.Height);
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(vertexShader, nullptr, 0);
		context->PSSetShader(pixelShader, nullptr, 0);
		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		context->OMSetDepthStencilState(nullptr, 0);
		context->RSSetState(nullptr);
		ID3D11ShaderResourceView* views[] = { source };
		ID3D11SamplerState* samplers[] = { sampler };
		context->PSSetShaderResources(0, 1, views);
		context->PSSetSamplers(0, 1, samplers);
		context->OMSetRenderTargets(1, &destination, nullptr);
		context->Draw(3, 0);
	}

	bool LeavePausedMenuUnpatched()
	{
		auto* state = globals::state;
		if (!state || state->IsMainOrLoadingMenuOpen() || state->isMapMenuOpen)
			return false;
		if (state->isStatsMenuOpen)
			return true;
		auto* ui = globals::game::ui;
		return ui && ui->GameIsPaused();
	}

	template <class Hook>
	void RunImagespaceAtDestinationViewport(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
	{
		if (LeavePausedMenuUnpatched()) {
			Hook::func(imageSpaceShader, shape, param);
			return;
		}
		auto& foveated = globals::features::upscaling.foveatedRender;
		foveated.LogMenuPipeline("preCopy");
		{
			LogicalTargetRenderScope scope;
			Hook::func(imageSpaceShader, shape, param);
		}
		foveated.LogMenuPipeline("postCopy");
	}

	template <class Original>
	void RunDistantBlurAtLogicalTarget(Original& original, void* imageSpaceShader,
		RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
	{
		auto& upscaling = globals::features::upscaling;
		if (globals::game::isVR || !upscaling.dx12SwapChain.useArise) {
			original(imageSpaceShader, shape, param);
			return;
		}

		LogicalTargetRenderScope scope;
		original(imageSpaceShader, shape, param);
	}
}

void FoveatedRender::LogMenuPipeline(const char* stage) const
{
	if (!menuCopyDiagnostics.load(std::memory_order_acquire))
		return;
	LogMenuPipelineImpl(stage);
}

void FoveatedRender::BlitTitleBackgroundToSwapChain() const
{
	auto* state = globals::state;
	if (globals::game::isVR || !WantsLogicalPerfMode() || !state ||
		state->worldRenderedThisFrame || !state->IsMainOrLoadingMenuOpen())
		return;

	auto* context = globals::d3d::context;
	auto* renderer = globals::game::renderer;
	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	if (!context || !renderer || !swapChain.swapChainBufferWrapped || !swapChain.swapChainBufferWrapped->rtv)
		return;

	auto& menuBackground = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMENUBG];
	if (!menuBackground.SRV)
		return;
	BlitSrvToSwapChain(menuBackground.SRV);
	LogMenuPipeline("titleBlit");
}

void FoveatedRender::MapRenderHook::thunk(
	void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	RunImagespaceAtDestinationViewport<MapRenderHook>(imageSpaceShader, shape, param);
}

void FoveatedRender::WorldMapRenderHook::thunk(
	void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	RunImagespaceAtDestinationViewport<WorldMapRenderHook>(imageSpaceShader, shape, param);
}

void FoveatedRender::WorldMapNoSkyBlurRenderHook::thunk(
	void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	RunImagespaceAtDestinationViewport<WorldMapNoSkyBlurRenderHook>(imageSpaceShader, shape, param);
}

void FoveatedRender::HDRTonemapRenderHook::thunk(
	void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	if (globals::game::isVR) {
		func(imageSpaceShader, shape, param);
		NeuralRendering::ApplyVrAfterTonemap();
		return;
	}
	RunImagespaceAtDestinationViewport<HDRTonemapRenderHook>(imageSpaceShader, shape, param);
}

void FoveatedRender::HDRTonemapFadeRenderHook::thunk(
	void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	if (globals::game::isVR) {
		func(imageSpaceShader, shape, param);
		NeuralRendering::ApplyVrAfterTonemap();
		return;
	}
	RunImagespaceAtDestinationViewport<HDRTonemapFadeRenderHook>(imageSpaceShader, shape, param);
}

void FoveatedRender::RendererUpdateViewPortHook::thunk(
	RE::BSGraphics::Renderer* renderer, std::uint32_t width, std::uint32_t height, bool disableScale)
{
	auto& swapChain = globals::features::upscaling.dx12SwapChain;
	if (!globals::game::isVR && globals::features::upscaling.foveatedRender.WantsLogicalPerfMode() &&
		swapChain.useArise && !LeavePausedMenuUnpatched() &&
		width == swapChain.PhysicalWidth() && height == swapChain.PhysicalHeight()) {
		width = swapChain.LogicalWidth();
		height = swapChain.LogicalHeight();
	}
	func(renderer, width, height, disableScale);
}

void FoveatedRender::DistantBlurRenderHook::thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	RunDistantBlurAtLogicalTarget(func, imageSpaceShader, shape, param);
}

void FoveatedRender::DistantBlurFoggedRenderHook::thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	RunDistantBlurAtLogicalTarget(func, imageSpaceShader, shape, param);
}

void FoveatedRender::DistantBlurMaskedFoggedRenderHook::thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	RunDistantBlurAtLogicalTarget(func, imageSpaceShader, shape, param);
}

void FoveatedRender::ID3D11DeviceContext_RSSetViewportsHook::thunk(
	ID3D11DeviceContext* context, UINT viewportCount, const D3D11_VIEWPORT* viewports)
{
	if (!logicalTargetRenderDepth || viewportCount != 1 || !viewports) {
		func(context, viewportCount, viewports);
		return;
	}

	auto& foveated = globals::features::upscaling.foveatedRender;
	const bool logCopy = foveated.menuCopyDiagnostics.load(std::memory_order_acquire);

	winrt::com_ptr<ID3D11RenderTargetView> target;
	context->OMGetRenderTargets(1, target.put(), nullptr);
	if (!target) {
		D3D11_VIEWPORT corrected = viewports[0];
		const bool rewritten = TryLogicalViewportFromPhysicalLeak(corrected);
		if (logCopy)
			logger::info("[MENUCOPY] stage=setVP dest=none requested={:.0f}x{:.0f} rewritten={} final={:.0f}x{:.0f}",
				viewports[0].Width, viewports[0].Height, rewritten ? 1 : 0,
				rewritten ? corrected.Width : viewports[0].Width,
				rewritten ? corrected.Height : viewports[0].Height);
		if (rewritten) {
			func(context, 1, &corrected);
			return;
		}
		func(context, viewportCount, viewports);
		return;
	}

	D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
	target->GetDesc(&viewDesc);
	if (viewDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) {
		func(context, viewportCount, viewports);
		return;
	}

	winrt::com_ptr<ID3D11Resource> resource;
	target->GetResource(resource.put());
	winrt::com_ptr<ID3D11Texture2D> texture;
	if (!resource || FAILED(resource->QueryInterface(texture.put()))) {
		func(context, viewportCount, viewports);
		return;
	}

	D3D11_TEXTURE2D_DESC textureDesc{};
	texture->GetDesc(&textureDesc);
	D3D11_VIEWPORT corrected = viewports[0];
	corrected.TopLeftX = 0.0f;
	corrected.TopLeftY = 0.0f;
	corrected.Width = static_cast<float>(std::max(1u, textureDesc.Width >> viewDesc.Texture2D.MipSlice));
	corrected.Height = static_cast<float>(std::max(1u, textureDesc.Height >> viewDesc.Texture2D.MipSlice));
	if (logCopy) {
		const auto dest = DescribeResource(texture.get());
		logger::info("[MENUCOPY] stage=setVP dest={} {}x{} requested={:.0f}x{:.0f} rewritten={:.0f}x{:.0f}",
			dest.name, dest.width, dest.height,
			viewports[0].Width, viewports[0].Height, corrected.Width, corrected.Height);
	}
	func(context, 1, &corrected);
}

void FoveatedRender::InstallD3DHook(ID3D11DeviceContext* context)
{
	if (!context || globals::game::isVR || !WantsLogicalPerfMode())
		return;

	stl::detour_vfunc<44, ID3D11DeviceContext_RSSetViewportsHook>(context);
}

void FoveatedRender::ClearShaderCache()
{
	FoveatedRenderImpl::Core::ClearShaderCache();
	FoveatedRenderImpl::Core::ClearResources();
}

// ============================================================================
// Settings I/O — driven from Upscaling::Save/LoadSettings under a nested key
// ============================================================================

void FoveatedRender::SaveSettings(json& o_json)
{
	o_json = settings;
	o_json["unifiedPipeline"] = settings.unifiedPipeline;
	o_json["singleThreadPerfMode"] = settings.singleThreadPerfMode;
	subrectController.SaveSettings(o_json);
}

void FoveatedRender::LoadSettings(const json& o_json)
{
	settings = o_json;
	settings.singleThreadPerfMode = o_json.value("singleThreadPerfMode", false);
	if (const auto unified = o_json.find("unifiedPipeline"); unified != o_json.end() && unified->is_object()) {
		settings.unifiedPipeline = unified->get<UnifiedPipelineSettings>();
	} else {
		settings.unifiedPipeline.act1Srss = settings.flatPipelineAct1Dsr;
		settings.unifiedPipeline.guidanceScale = settings.flatPipelineAct1Dsr ? 2.0f : 1.0f;
		switch (static_cast<FlatAct2Backend>(std::min(settings.flatPipelineAct2Backend, static_cast<uint>(FlatAct2Backend::kEasu)))) {
		case FlatAct2Backend::kDLSS:
			settings.unifiedPipeline.act2SrBackend = static_cast<uint>(FlatAct2SrBackend::kDLSS);
			break;
		case FlatAct2Backend::kFSR3:
			settings.unifiedPipeline.act2SrBackend = static_cast<uint>(FlatAct2SrBackend::kFSR3);
			break;
		case FlatAct2Backend::kNIS:
			settings.unifiedPipeline.act2SrBackend = static_cast<uint>(FlatAct2SrBackend::kNIS);
			break;
		case FlatAct2Backend::kFusion:
			settings.unifiedPipeline.act2SrBackend = static_cast<uint>(FlatAct2SrBackend::kLanczos);
			settings.unifiedPipeline.fusionEnabled = true;
			break;
		case FlatAct2Backend::kMitchell:
			settings.unifiedPipeline.act2SrBackend = static_cast<uint>(FlatAct2SrBackend::kMitchell);
			break;
		case FlatAct2Backend::kEasu:
			settings.unifiedPipeline.act2SrBackend = static_cast<uint>(FlatAct2SrBackend::kEasu);
			break;
		case FlatAct2Backend::kLanczos:
			settings.unifiedPipeline.act2SrBackend = static_cast<uint>(FlatAct2SrBackend::kLanczos);
			break;
		}
	}
	// Util::Subrect::Controller::LoadSettings takes `const json&` (Subrect.h:68)
	// so no const_cast is needed — keeping it would imply mutation that never
	// happens.
	subrectController.LoadSettings(o_json);
	ClampSettings();
	NeuralRendering::ARISE::Pipeline::Instance().SetEnabled(false);
	NeuralRendering::ARISE::Pipeline::Instance().SetWorkerNrEnabled(false);
	NeuralRendering::ARISE::Pipeline::Instance().SetSlotCount(settings.ariseBufferCount);
	NeuralRendering::ARISE::Pipeline::Instance().SetVisualValidation(settings.ariseVisualValidation);
	NeuralRendering::ARISE::Pipeline::Instance().SetWorkerNrEnabled(settings.ariseWorkerNr);
	NeuralRendering::ARISE::Pipeline::Instance().SetEnabled(settings.ariseShadowScheduler);
}

void FoveatedRender::RestoreDefaultSettings()
{
	settings = {};
	ClampSettings();
	NeuralRendering::ARISE::Pipeline::Instance().SetVisualValidation(false);
	NeuralRendering::ARISE::Pipeline::Instance().SetWorkerNrEnabled(false);
	NeuralRendering::ARISE::Pipeline::Instance().SetEnabled(false);
	NeuralRendering::ARISE::Pipeline::Instance().SetSlotCount(settings.ariseBufferCount);
}

void FoveatedRender::ClampSettings()
{
	settings.enabled = std::min(settings.enabled, 1u);
	settings.dlssMode = std::min(settings.dlssMode, 1u);
	settings.stretchMode = std::min(settings.stretchMode, 2u);
	settings.debugVisualize = std::min(settings.debugVisualize, 1u);
	settings.peripheryAAMode = std::min(settings.peripheryAAMode, 1u);
	settings.subrectBlendMode = std::min(settings.subrectBlendMode, 2u);
	settings.subrectMaskMode = std::min(settings.subrectMaskMode, 1u);
	settings.peripheryBlurRadius = std::clamp(settings.peripheryBlurRadius, 0.5f, 4.0f);
	settings.peripheryTemporalAlpha = std::clamp(settings.peripheryTemporalAlpha, 0.05f, 0.5f);
	settings.subrectFeatherWidth = std::clamp(settings.subrectFeatherWidth, 2.0f, 128.0f);
	settings.subrectFalloffCurve = std::clamp(settings.subrectFalloffCurve, 0.5f, 2.0f);
	settings.subrectDitherStrength = std::clamp(settings.subrectDitherStrength, 0.0f, 2.0f);
	settings.neuralRenderingPreset = std::min(settings.neuralRenderingPreset, 4u);
	settings.ariseBufferCount = std::clamp(settings.ariseBufferCount, 2u, 3u);
	if (settings.singleThreadPerfMode)
		settings.ariseModel3 = false;
	if (WantsLogicalPerfMode()) {
		settings.ariseBufferCount = 3;
		bool migrated = false;
		if (settings.flatPipelineAct1Backend == static_cast<uint>(FlatAct1Backend::kFSR3)) {
			settings.flatPipelineAct1Backend = static_cast<uint>(FlatAct1Backend::kDLSS);
			migrated = true;
		}
		if (settings.flatPipelineAct2Backend == static_cast<uint>(FlatAct2Backend::kFSR3) ||
			settings.flatPipelineAct2Backend == static_cast<uint>(FlatAct2Backend::kNIS)) {
			settings.flatPipelineAct2Backend = static_cast<uint>(FlatAct2Backend::kDLSS);
			migrated = true;
		}
		static bool migrationLogged = false;
		if (migrated && !migrationLogged) {
			logger::info("[ARISE] Migrated legacy Model 3 FSR/NIS backend selection to DLSS");
			migrationLogged = true;
		}
	}
	settings.flatPipelineAct1Backend = std::min(settings.flatPipelineAct1Backend, 2u);
	settings.flatPipelineAct1Quality = std::min(settings.flatPipelineAct1Quality, 4u);
	settings.flatPipelineAct1Preset = std::min(settings.flatPipelineAct1Preset, 4u);
	settings.flatPipelineAct1Jitter = std::min(settings.flatPipelineAct1Jitter, 1u);
	auto& unified = settings.unifiedPipeline;
	unified.act1SrssQuality = std::clamp(unified.act1SrssQuality, 3u, 4u);
	if (unified.act1SrssQuality == 3)
		unified.act1SrssPreset = 2;
	else if (unified.act1SrssPreset != 3 && unified.act1SrssPreset != 4)
		unified.act1SrssPreset = 4;
	unified.guidanceScale = std::clamp(unified.guidanceScale, 1.0f, 2.0f);
	unified.act1Downsample = std::min(unified.act1Downsample, static_cast<uint>(ResampleFilter::kTent));
	unified.act1DepthDownsample = std::min(unified.act1DepthDownsample, static_cast<uint>(DepthResampleFilter::kLanczos));
	unified.act2SrBackend = std::min(unified.act2SrBackend, static_cast<uint>(FlatAct2SrBackend::kTent));
	unified.act2Upsample = std::min(unified.act2Upsample, static_cast<uint>(ResampleFilter::kTent));
	unified.fusionReconstruct = std::min(unified.fusionReconstruct, static_cast<uint>(FusionReconstruct::kLanczos));
	if (WantsLogicalPerfMode())
		settings.flatPipelineAct1Backend = static_cast<uint>(FlatAct1Backend::kDLSS);
	settings.flatPipelineAct1Dsr = false;
	settings.flatPipelineNrScale = std::clamp(settings.flatPipelineNrScale, 0.33f, 0.95f);
	settings.flatPipelineAct2Backend = std::min(settings.flatPipelineAct2Backend, static_cast<uint>(FlatAct2Backend::kEasu));
	settings.flatPipelineAct2Preset = std::min(settings.flatPipelineAct2Preset, 4u);
	settings.flatPipelineAct2Jitter = std::min(settings.flatPipelineAct2Jitter, 1u);
	settings.flatPipelineFusionDirection = std::min(settings.flatPipelineFusionDirection, 1u);
	settings.flatPipelineFusionDenoise = std::min(settings.flatPipelineFusionDenoise, 4u);
	settings.flatPipelineFusionDenoiseStrength = std::clamp(settings.flatPipelineFusionDenoiseStrength, 0.0f, 1.0f);
	settings.flatPipelineFusionSoftness = std::clamp(settings.flatPipelineFusionSoftness, 0.0f, 1.0f);
	settings.flatPipelineFusionFrequencyGuidance = std::clamp(settings.flatPipelineFusionFrequencyGuidance, 0.0f, 1.0f);
	settings.flatPipelineFusionDepthGuidance = std::clamp(settings.flatPipelineFusionDepthGuidance, 0.0f, 1.0f);
	settings.flatPipelineFusionColorGuidance = std::clamp(settings.flatPipelineFusionColorGuidance, 0.0f, 1.0f);
	settings.flatPipelineSharpness = std::clamp(settings.flatPipelineSharpness, 0.0f, 1.0f);
	settings.flatAct1QualityMode = std::min(settings.flatAct1QualityMode, 4u);
	settings.flatAct1Preset = std::min(settings.flatAct1Preset, 4u);
	settings.flatAct1Sharpness = std::clamp(settings.flatAct1Sharpness, 0.0f, 1.0f);
	settings.flatAct2QualityMode = std::min(settings.flatAct2QualityMode, 4u);
	settings.flatAct2Preset = std::min(settings.flatAct2Preset, 4u);
	settings.flatAct2Sharpness = std::clamp(settings.flatAct2Sharpness, 0.0f, 1.0f);
	settings.neuralRenderingIntensity = std::clamp(settings.neuralRenderingIntensity, 0.0f, 2.0f);
	settings.neuralRenderingLocalTone = std::clamp(settings.neuralRenderingLocalTone, 0.0f, 2.0f);
	settings.neuralRenderingLocalStructure = std::clamp(settings.neuralRenderingLocalStructure, 0.0f, 2.0f);
	settings.neuralRenderingSkinStructure = std::clamp(settings.neuralRenderingSkinStructure, 0.0f, 2.0f);
	settings.neuralRenderingStyle = std::min(settings.neuralRenderingStyle, 3u);
	settings.neuralRenderingInputPrefilter = std::min(settings.neuralRenderingInputPrefilter, 3u);
	settings.neuralRenderingMotionVectorMode = std::min(settings.neuralRenderingMotionVectorMode, 5u);
	settings.neuralRenderingWorkingScale = std::min(settings.neuralRenderingWorkingScale, 2u);
	settings.neuralRenderingOutputMode = std::min(settings.neuralRenderingOutputMode, 3u);
	settings.neuralRenderingNisSharpness = std::clamp(settings.neuralRenderingNisSharpness, 0.0f, 1.0f);
	settings.neuralRenderingFusionPreset = std::min(settings.neuralRenderingFusionPreset, 3u);
	settings.neuralRenderingFusionLuma = std::clamp(settings.neuralRenderingFusionLuma, 0.0f, 1.5f);
	settings.neuralRenderingFusionChroma = std::clamp(settings.neuralRenderingFusionChroma, 0.0f, 1.5f);
	settings.neuralRenderingFusionEdgeProtection = std::clamp(settings.neuralRenderingFusionEdgeProtection, 0.0f, 1.0f);
	settings.neuralRenderingFusionResidualLimit = std::clamp(settings.neuralRenderingFusionResidualLimit, 0.01f, 0.5f);
	settings.fusionDepthDilate = std::clamp(settings.fusionDepthDilate, 0.0f, 2.0f);
	settings.fusionGEdge = std::clamp(settings.fusionGEdge, 0.0f, 1.0f);
	settings.fusionNEdge = std::clamp(settings.fusionNEdge, 0.0f, 1.0f);
	settings.fusionEdgePower = std::clamp(settings.fusionEdgePower, 0.0f, 1.0f);
	settings.fusionHfLumaSuppress = std::clamp(settings.fusionHfLumaSuppress, 0.0f, 1.0f);
	settings.fusionHfTowardN = std::clamp(settings.fusionHfTowardN, 0.0f, 1.0f);
	settings.fusionNcDilate = std::clamp(settings.fusionNcDilate, 0.0f, 2.0f);
	settings.fusionAlignG = std::clamp(settings.fusionAlignG, 0.0f, 2.0f);
	settings.fusionCompareMode = std::min(settings.fusionCompareMode, 3u);
	settings.fusionToneGainLuma = std::clamp(settings.fusionToneGainLuma, 0.0f, 3.0f);
	settings.fusionToneGainChroma = std::clamp(settings.fusionToneGainChroma, 0.0f, 3.0f);
	settings.fusionDetailGain = std::clamp(settings.fusionDetailGain, 0.0f, 3.0f);
	settings.fusionDither = std::clamp(settings.fusionDither, 0.0f, 2.0f);
	settings.fusionGainPrior = std::clamp(settings.fusionGainPrior, 0.0f, 1.5f);
	settings.fusionEdgeBlockGThresh = std::clamp(settings.fusionEdgeBlockGThresh, 0.0f, 1.0f);
	settings.fusionFitSideSigma = std::clamp(settings.fusionFitSideSigma, 0.02f, 0.5f);
	settings.fusionDirectionalSharpen = std::clamp(settings.fusionDirectionalSharpen, 0.0f, 2.0f);
	settings.fusionSoftEdge = std::clamp(settings.fusionSoftEdge, 0.0f, 1.0f);
	settings.fusionDebugView = std::min(settings.fusionDebugView, 9u);
	settings.act3Scale = std::clamp(settings.act3Scale, 0.0f, 0.9f);
	settings.act3Preset = std::min(settings.act3Preset, 4u);
	settings.act3Intensity = std::clamp(settings.act3Intensity, 0.0f, 2.0f);
	settings.act3LocalTone = std::clamp(settings.act3LocalTone, 0.0f, 2.0f);
	settings.act3LocalStructure = std::clamp(settings.act3LocalStructure, 0.0f, 2.0f);
	settings.act3SkinStructure = std::clamp(settings.act3SkinStructure, 0.0f, 2.0f);
	settings.act3Style = std::min(settings.act3Style, 3u);
	settings.act3Sharpness = std::clamp(settings.act3Sharpness, 0.0f, 1.0f);
	settings.act3FeatherWidth = std::clamp(settings.act3FeatherWidth, 2.0f, 128.0f);
	settings.act3DitherStrength = std::clamp(settings.act3DitherStrength, 0.0f, 2.0f);
	settings.act3GuidanceLuma = std::clamp(settings.act3GuidanceLuma, 0.0f, 1.5f);
	settings.act3GuidanceChroma = std::clamp(settings.act3GuidanceChroma, 0.0f, 1.5f);
	settings.act3GuidancePrior = std::clamp(settings.act3GuidancePrior, 0.0f, 1.5f);
	settings.act3GuidanceMaxGain = std::clamp(settings.act3GuidanceMaxGain, 0.0f, 1.0f);
	settings.act3GuidanceToneLuma = std::clamp(settings.act3GuidanceToneLuma, 0.0f, 3.0f);
	settings.act3GuidanceToneChroma = std::clamp(settings.act3GuidanceToneChroma, 0.0f, 3.0f);
	settings.act3GuidanceDetailGain = std::clamp(settings.act3GuidanceDetailGain, 0.0f, 3.0f);
	settings.act3GuidanceDither = std::clamp(settings.act3GuidanceDither, 0.0f, 2.0f);
	settings.act3GuidanceDirSharpen = std::clamp(settings.act3GuidanceDirSharpen, 0.0f, 2.0f);
	// Preset clamping reads from Upscaling::Settings now.
	auto& sharedPreset = globals::features::upscaling.settings.presetDLSS;
	sharedPreset = std::min(sharedPreset, 5u);
	if (!IsPresetCompatibleWithMode(sharedPreset)) {
		sharedPreset = 3;  // Fall back to L
	}
}

FoveatedRender::Act3Crop FoveatedRender::MakeAct3Crop(uint32_t displayWidth, uint32_t displayHeight, float scale)
{
	Act3Crop crop{};
	scale = std::clamp(scale, 0.0f, 0.9f);
	if (scale < 0.01f || displayWidth < 8 || displayHeight < 8)
		return crop;
	auto evenSize = [](uint32_t value, uint32_t limit) {
		uint32_t size = value & ~1u;
		size = std::min(size, limit & ~1u);
		return std::max(8u, size);
	};
	crop.width = evenSize(static_cast<uint32_t>(std::lround(displayWidth * scale)), displayWidth);
	crop.height = evenSize(static_cast<uint32_t>(std::lround(displayHeight * scale)), displayHeight);
	if (crop.width > displayWidth || crop.height > displayHeight)
		return {};
	crop.x = (displayWidth - crop.width) / 2;
	crop.y = (displayHeight - crop.height) / 2;
	return crop;
}

FoveatedRender::FlatPipelinePlan FoveatedRender::BuildFlatPipelinePlan(
	uint32_t displayWidth, uint32_t displayHeight, uint32_t engineWidth, uint32_t engineHeight) const
{
	FlatPipelinePlan plan{};
	plan.displayWidth = displayWidth;
	plan.displayHeight = displayHeight;
	plan.displayD = { displayWidth, displayHeight };
	const bool model3AtBoot = BootUsesLogicalPerfMode();
	const auto unified = ActiveUnifiedPipelineSettings();
	const uint act1Backend = model3AtBoot ? bootSnapshot.Boot(&Settings::flatPipelineAct1Backend) : settings.flatPipelineAct1Backend;
	const uint act1QualitySetting = model3AtBoot ? bootSnapshot.Boot(&Settings::flatPipelineAct1Quality) : settings.flatPipelineAct1Quality;
	plan.act1Backend = model3AtBoot ? FlatAct1Backend::kDLSS : static_cast<FlatAct1Backend>(std::min(act1Backend, 2u));
	plan.act2SrBackend = static_cast<FlatAct2SrBackend>(std::min(
		unified.act2SrBackend, static_cast<uint>(FlatAct2SrBackend::kTent)));
	plan.fusion = unified.fusionEnabled;
	plan.act3Guidance = settings.act3Enabled && settings.act3GuidanceEnabled;
	if (plan.act2SrBackend == FlatAct2SrBackend::kDLSS)
		plan.act2Backend = FlatAct2Backend::kDLSS;
	else if (plan.act2SrBackend == FlatAct2SrBackend::kFSR3)
		plan.act2Backend = FlatAct2Backend::kFSR3;
	else if (plan.act2SrBackend == FlatAct2SrBackend::kNIS)
		plan.act2Backend = FlatAct2Backend::kNIS;
	else if (plan.act2SrBackend == FlatAct2SrBackend::kMitchell)
		plan.act2Backend = FlatAct2Backend::kMitchell;
	else if (plan.act2SrBackend == FlatAct2SrBackend::kEasu)
		plan.act2Backend = FlatAct2Backend::kEasu;
	else
		plan.act2Backend = FlatAct2Backend::kLanczos;
	plan.fusionReconstruct = static_cast<FusionReconstruct>(std::min(
		unified.fusionReconstruct, static_cast<uint>(FusionReconstruct::kLanczos)));
	plan.act1Downsample = static_cast<ResampleFilter>(unified.act1Downsample);
	plan.act1DepthDownsample = static_cast<DepthResampleFilter>(unified.act1DepthDownsample);
	plan.act2Upsample = static_cast<ResampleFilter>(unified.act2Upsample);
	plan.act1Dsr = !model3AtBoot && settings.flatPipelineAct1Dsr && plan.act1Backend != FlatAct1Backend::kBypass;
	plan.srss = unified.act1Srss && plan.act1Backend == FlatAct1Backend::kDLSS;
	plan.act1Quality = plan.srss ? unified.act1SrssQuality : act1QualitySetting;
	plan.act1Preset = plan.srss ? unified.act1SrssPreset : settings.flatPipelineAct1Preset;
	plan.sharpen = settings.flatPipelineSharpen;
	if (!displayWidth || !displayHeight)
		return plan;

	const auto scaled = [](uint32_t value, uint quality, bool dsr) {
		const float multiplier = dsr ? 2.0f : 1.0f;
		return std::max(1u, static_cast<uint32_t>(std::lround(
			value * multiplier / Upscaling::GetQualityModeRatio(std::min(quality, 4u)))));
	};

	if (engineWidth && engineHeight) {
		plan.engineWidth = engineWidth;
		plan.engineHeight = engineHeight;
		plan.act1OutputWidth = plan.act1Backend == FlatAct1Backend::kBypass ? plan.engineWidth :
			std::max(2u, static_cast<uint32_t>(std::lround(displayWidth *
				(plan.srss ? unified.guidanceScale : 1.0f))) & ~1u);
		plan.act1OutputHeight = plan.act1Backend == FlatAct1Backend::kBypass ? plan.engineHeight :
			std::max(2u, static_cast<uint32_t>(std::lround(displayHeight *
				(plan.srss ? unified.guidanceScale : 1.0f))) & ~1u);
	} else if (plan.act1Backend == FlatAct1Backend::kBypass) {
		plan.engineWidth = displayWidth;
		plan.engineHeight = displayHeight;
		plan.act1OutputWidth = displayWidth;
		plan.act1OutputHeight = displayHeight;
	} else {
		const uint act1Quality = plan.act1Dsr ? 4u : act1QualitySetting;
		plan.engineWidth = std::min(displayWidth, scaled(displayWidth, act1Quality, plan.act1Dsr));
		plan.engineHeight = std::min(displayHeight, scaled(displayHeight, act1Quality, plan.act1Dsr));
		plan.act1OutputWidth = displayWidth * (plan.act1Dsr ? 2u : 1u);
		plan.act1OutputHeight = displayHeight * (plan.act1Dsr ? 2u : 1u);
	}
	plan.renderR = { plan.engineWidth, plan.engineHeight };
	plan.guidanceG = { plan.act1OutputWidth, plan.act1OutputHeight };

	const float nrScale = settings.flatPipelineNrNative ? 1.0f : settings.flatPipelineNrScale;
	plan.nrWidth = std::min(plan.act1OutputWidth, std::max(1u, static_cast<uint32_t>(
		std::lround(displayWidth * nrScale))));
	plan.nrHeight = std::min(plan.act1OutputHeight, std::max(1u, static_cast<uint32_t>(
		std::lround(displayHeight * nrScale))));
	plan.nrOutputWidth = plan.nrWidth;
	plan.nrOutputHeight = plan.nrHeight;
	plan.neuralN = { plan.nrWidth, plan.nrHeight };
	plan.act2OutputWidth = displayWidth;
	plan.act2OutputHeight = displayHeight;
	if (plan.act2Backend == FlatAct2Backend::kDLSS &&
		(plan.nrWidth != displayWidth || plan.nrHeight != displayHeight)) {
		float closestScaleDistance = std::numeric_limits<float>::max();
		const float actualScale = static_cast<float>(plan.nrWidth) / static_cast<float>(displayWidth);
		for (uint candidate = 1; candidate <= 4; ++candidate) {
			const float candidateScale = 1.0f / Upscaling::GetQualityModeRatio(candidate);
			const float distance = std::abs(candidateScale - actualScale);
			if (distance < closestScaleDistance) {
				closestScaleDistance = distance;
				plan.act2Quality = candidate;
			}
		}
		struct DlssRangeCache
		{
			uint32_t quality = UINT32_MAX;
			uint32_t outputWidth = 0;
			uint32_t outputHeight = 0;
			uint32_t requestedWidth = 0;
			uint32_t requestedHeight = 0;
			uint32_t clampedWidth = 0;
			uint32_t clampedHeight = 0;
		};
		static DlssRangeCache rangeCache;
		if (Upscaling::streamline.slDLSSGetOptimalSettings) {
			if (rangeCache.quality != plan.act2Quality || rangeCache.outputWidth != displayWidth ||
				rangeCache.outputHeight != displayHeight || rangeCache.requestedWidth != plan.nrWidth ||
				rangeCache.requestedHeight != plan.nrHeight) {
				rangeCache = { plan.act2Quality, displayWidth, displayHeight, plan.nrWidth, plan.nrHeight,
					plan.nrWidth, plan.nrHeight };
				Upscaling::streamline.ClampToDLSSRenderRange(plan.act2Quality, displayWidth, displayHeight,
					rangeCache.clampedWidth, rangeCache.clampedHeight);
			}
			plan.nrWidth = rangeCache.clampedWidth;
			plan.nrHeight = rangeCache.clampedHeight;
		}
		plan.nrOutputWidth = plan.nrWidth;
		plan.nrOutputHeight = plan.nrHeight;
	}
	return plan;
}

// ============================================================================
// Activation + accessors
// ============================================================================

bool FoveatedRender::IsActive() const
{
	// Gate on DLSS/FSR being the *selected* method, not just available
	// (IsRuntimeSupported): otherwise foveation and its SSR consumer run under
	// TAA/None and the route derefs unallocated upscaler resources — the crash
	// seen under RenderDoc, whose DX12 swapchain disables DLSS.
	if (!enabledAtBoot || !IsRuntimeSupported())
		return false;
	const auto method = globals::features::upscaling.GetUpscaleMethod();
	if (method != Upscaling::UpscaleMethod::kDLSS && method != Upscaling::UpscaleMethod::kFSR)
		return false;

	// A Full Eye region pays the isolation/stretch overhead (snapshot copies, mask
	// clears, StretchDRS) for a subrect equal to the full frame -- no savings, real
	// cost. Skip the route so the standard full-frame path runs instead.
	const bool isFullEye = subrectController.GetUV().IsFullEye() && subrectController.GetRightEyeUV().IsFullEye();
	return !isFullEye;
}

bool FoveatedRender::ShouldForceVisualize() const
{
	using namespace std::chrono_literals;
	return std::chrono::steady_clock::now() - lastDragTime < 3s;
}

bool FoveatedRender::IsRuntimeSupported() const
{
	// FSR's host path has no adapter/runtime prerequisite, so VR alone is
	// sufficient to enable the option -- IsActive() is what actually gates
	// on the *selected* method (kDLSS/kFSR) being one the route supports.
	return globals::game::isVR;
}

FoveatedRender::DlssMode FoveatedRender::GetDlssMode() const
{
	if (globals::features::upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kFSR)
		return DlssMode::kDefault;
	return (DlssMode)std::min(settings.dlssMode, 1u);
}

FoveatedRender::FoveationProfile FoveatedRender::GetFoveationProfile() const
{
	FoveationProfile profile;
	if (!IsActive())
		return profile;

	const auto& leftUV = subrectController.GetUV();
	const auto& rightUV = subrectController.GetRightEyeUV();

	// Map the rectangular subrect onto the centered superellipse the mask helper expects: vertical
	// extent drives coverageScale (radiusY = coverageScale/2), the rect aspect drives the horizontal
	// stretch (radiusX = coverageScale * hScale/2). The mask carries one scale for both eyes (only
	// the center offset is per-eye), so size comes from the less-foveated (larger) extent of the two:
	// the center is the superset enclosing both eyes' full-quality regions, so neither eye's sharp
	// zone is ever foveated (min would shrink it below an eye's sharp region and foveate it). A
	// full eye therefore yields full coverage and disables foveation (the gate below).
	const float coverageH = std::max(leftUV.h, rightUV.h);
	const float coverageW = std::max(leftUV.w, rightUV.w);
	const float coverageScale = FoveatedCommon::ClampCenterScale(coverageH);

	// Availability keys off the clamped scale: if the larger eye rounds up to full coverage there is
	// nothing to foveate, leave the default (available == false).
	if (!FoveatedCommon::IsActiveCoverage(coverageScale))
		return profile;

	profile.available = true;
	profile.coverageScale = coverageScale;
	profile.centerHorizontalScale = FoveatedCommon::ClampCenterHorizontalScale(
		coverageH > 1e-4f ? coverageW / coverageH : 1.0f);
	profile.centerOffsets[0] = float2{ (leftUV.x + leftUV.w * 0.5f) - 0.5f, (leftUV.y + leftUV.h * 0.5f) - 0.5f };
	profile.centerOffsets[1] = float2{ (rightUV.x + rightUV.w * 0.5f) - 0.5f, (rightUV.y + rightUV.h * 0.5f) - 0.5f };
	return profile;
}

void FoveatedRender::LatchQualityMode()
{
	qualityModeAtBoot = std::clamp(globals::features::upscaling.settings.qualityMode, 1u, 4u);
}

uint FoveatedRender::GetActiveQualityMode() const
{
	return std::clamp(globals::features::upscaling.settings.qualityMode, 1u, 4u);
}

uint FoveatedRender::GetActivePresetDLSS() const
{
	return std::min(globals::features::upscaling.settings.presetDLSS, 5u);
}

float FoveatedRender::GetActiveSharpnessDLSS() const
{
	return std::clamp(globals::features::upscaling.settings.sharpnessDLSS, 0.0f, 1.0f);
}

float FoveatedRender::GetRenderScaleForQuality(uint qualityMode)
{
	return Upscaling::GetQualityModeRatio(qualityMode);
}

bool FoveatedRender::IsPresetCompatibleWithMode(uint presetIndex) const
{
	// Preset indices: 0=Default, 1=J, 2=K, 3=L, 4=M, 5=F
	// Faster mode: J(1) and K(2) are incompatible.
	if (GetDlssMode() == DlssMode::kFaster) {
		return presetIndex != 1 && presetIndex != 2;
	}
	return true;
}

void FoveatedRender::ClampPresetToMode()
{
	auto& sharedPreset = globals::features::upscaling.settings.presetDLSS;
	if (!IsPresetCompatibleWithMode(sharedPreset)) {
		sharedPreset = 3;  // Fall back to L
	}
}

// ============================================================================
// UI — FoveatedRender-specific knobs only. Quality / sharpness / preset /
// Streamline log level live on Upscaling's panel and apply to both DLSS paths.
// Called from Upscaling::DrawSettings inside a TreeNode.
// ============================================================================

void FoveatedRender::DrawEnable()
{
	ClampSettings();

	ImGui::TextWrapped("%s", T(TKEY("foveated_overview"),
								 "Foveated subrect upscaling: only the user-selected region gets full DLSS/FSR "
								 "upscaling, the periphery is cheaply stretched. Significant upscaler cost reduction "
								 "at the cost of peripheral sharpness. VR only."));

	const bool runtimeSupported = IsRuntimeSupported();
	if (!runtimeSupported) {
		settings.enabled = 0;
	}

	if (!runtimeSupported)
		ImGui::BeginDisabled();
	bool enabledBool = settings.enabled != 0;
	if (ImGui::Checkbox(T(TKEY("foveated_enable"), "Enable Foveated Upscaling (region source)"), &enabledBool)) {
		settings.enabled = enabledBool ? 1u : 0u;
		// Full Eye is a deliberate no-op (see IsActive()) -- enabling straight from
		// it would silently do nothing until the user finds the region picker below.
		if (enabledBool && subrectController.GetUV().IsFullEye() && subrectController.GetRightEyeUV().IsFullEye())
			subrectController.ApplyPresetByName(kPresetCenter75);
	}
	if (!runtimeSupported)
		ImGui::EndDisabled();

	Util::UI::DrawSettingDiff(bootSnapshot, settings, &Settings::enabled);

	if (enabledAtBoot) {
		const auto method = globals::features::upscaling.GetUpscaleMethod();
		const bool methodOk = method == Upscaling::UpscaleMethod::kDLSS || method == Upscaling::UpscaleMethod::kFSR;
		if (IsActive())
			Util::Text::WrappedInfo("%s", T(TKEY("foveated_active"), "Active: foveated subrect upscaling is enabled (skipped in the main and loading menus / on preflight failure)."));
		else if (!methodOk)
			Util::Text::Warning("%s", T(TKEY("foveated_standing_by"), "Standing by: only active while the Upscaling Method is DLSS or FSR. Inactive right now."));
		else
			Util::Text::Warning("%s", T(TKEY("foveated_standing_by_full_eye"), "Standing by: region is Full Eye (no crop) -- shrink it in Subrect Region below to see savings."));
	}

	if (!globals::game::isVR) {
		Util::Text::Warning("%s", T(TKEY("foveated_vr_only"), "VR only -- flat has no equivalent lens-driven periphery quality cliff to exploit."));
	}
}

const char* FoveatedRender::DlssModeName(DlssMode mode)
{
	return mode == DlssMode::kFaster ?
	           T(TKEY("foveated_dlss_mode_faster"), "Faster") :
	           T(TKEY("foveated_dlss_mode_default"), "Default");
}

const char* FoveatedRender::StretchModeName(StretchMode mode)
{
	switch (mode) {
	case StretchMode::kPoint:
		return T(TKEY("foveated_stretch_point"), "Point");
	case StretchMode::kGaussianBlur:
		return T(TKEY("foveated_stretch_gaussian"), "Gaussian Blur");
	default:
		return T(TKEY("foveated_stretch_bilinear"), "Bilinear");
	}
}

const char* FoveatedRender::PeripheryAAModeName(PeripheryAAMode mode)
{
	return mode == PeripheryAAMode::kTemporalSmooth ?
	           T(TKEY("foveated_periphery_aa_temporal"), "Temporal Smooth") :
	           T(TKEY("foveated_periphery_aa_none"), "None");
}

const char* FoveatedRender::SubrectBlendModeName(SubrectBlendMode mode)
{
	switch (mode) {
	case SubrectBlendMode::kFeather:
		return T(TKEY("foveated_blend_feather"), "Feather");
	case SubrectBlendMode::kDither:
		return T(TKEY("foveated_blend_dither"), "Dither");
	default:
		return T(TKEY("foveated_blend_hard_copy"), "Hard Copy");
	}
}

const char* FoveatedRender::SubrectMaskModeName(SubrectMaskMode mode)
{
	return mode == SubrectMaskMode::kOval ?
	           T(TKEY("foveated_mask_oval"), "Oval") :
	           T(TKEY("foveated_mask_rectangle"), "Rectangle");
}

void FoveatedRender::DrawLogicalPerfModeControls()
{
	if (globals::game::isVR) {
		ImGui::TextDisabled("%s", T(TKEY("pipeline_arise_vr_blocked"),
			"ARISE owns DXGI Present. VR submits through OpenVR. 8-pass runs after Post, then we Submit."));
		return;
	}
	ClampSettings();

	auto& upscaling = globals::features::upscaling;
	int mode = settings.ariseModel3 ? 2 : (settings.singleThreadPerfMode ? 1 : 0);
	if (ImGui::RadioButton(T(TKEY("pipeline_origin"), "Origin (engine D, viewport R)"), mode == 0)) {
		settings.ariseModel3 = false;
		settings.singleThreadPerfMode = false;
	}
	if (ImGui::RadioButton(T(TKEY("pipeline_single_thread"), "Single-thread PerfMode"), mode == 1)) {
		settings.ariseModel3 = false;
		settings.singleThreadPerfMode = true;
		settings.ariseBufferCount = 3;
	}
	if (ImGui::RadioButton(T(TKEY("pipeline_arise"), "ARISE Async PerfMode"), mode == 2)) {
		settings.ariseModel3 = true;
		settings.singleThreadPerfMode = false;
		settings.ariseBufferCount = 3;
	}
	Util::UI::RestartGatedAnnotate(bootSnapshot, settings, &Settings::ariseModel3,
		T(TKEY("pipeline_arise_restart"),
			"ARISE: three-slot logical Present tail. Restart required; HDR is not supported."));
	Util::UI::RestartGatedAnnotate(bootSnapshot, settings, &Settings::singleThreadPerfMode,
		T(TKEY("pipeline_single_thread_restart"),
			"Same logical R proxy and six-pass graph on the Present calling thread."));
	if (upscaling.dx12SwapChain.useArise)
		ImGui::TextDisabled("%s", upscaling.dx12SwapChain.IsAriseAsyncTail() ?
			T(TKEY("pipeline_active_arise"), "Active this session: ARISE asynchronous Tail") :
			T(TKEY("pipeline_active_single_thread"), "Active this session: Single-thread synchronous Tail"));
	else
		ImGui::TextDisabled("%s", T(TKEY("pipeline_active_origin"), "Active this session: Origin (engine RTs at D)"));
}

namespace
{
	void DrawArrow(ImDrawList* drawList, ImVec2 from, ImVec2 to, ImU32 color)
	{
		drawList->AddLine(from, to, color, 1.5f);
		const ImVec2 delta{ to.x - from.x, to.y - from.y };
		const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
		if (length < 1.0f)
			return;
		const ImVec2 dir{ delta.x / length, delta.y / length };
		const ImVec2 n{ -dir.y, dir.x };
		drawList->AddTriangleFilled(
			to,
			ImVec2(to.x - dir.x * 7.0f + n.x * 3.5f, to.y - dir.y * 7.0f + n.y * 3.5f),
			ImVec2(to.x - dir.x * 7.0f - n.x * 3.5f, to.y - dir.y * 7.0f - n.y * 3.5f),
			color);
	}

	void DrawSixPassDiagram(const FoveatedRender::FlatPipelinePlan& plan, bool fusionOn)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float width = std::max(ImGui::GetContentRegionAvail().x, 280.0f);
		const float height = 128.0f;
		ImGui::Dummy(ImVec2(width, height));

		auto area = [](FoveatedRender::Extent e) {
			return std::sqrt(std::max(1.0f, static_cast<float>(e.width) * static_cast<float>(e.height)));
		};
		const float maxArea = std::max({ area(plan.renderR), area(plan.guidanceG), area(plan.neuralN), area(plan.displayD), 1.0f });
		auto halfSize = [&](FoveatedRender::Extent e) {
			const float scale = plan.renderR.width ? area(e) / maxArea : 0.7f;
			return ImVec2(16.0f + 24.0f * scale, 11.0f + 16.0f * scale);
		};

		const ImU32 fill = ImGui::GetColorU32(ImGuiCol_FrameBg);
		const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
		const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
		const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
		const float rowY = origin.y + 42.0f;
		const float xs[4] = {
			origin.x + width * 0.12f, origin.x + width * 0.37f,
			origin.x + width * 0.62f, origin.x + width * 0.87f
		};
		const FoveatedRender::Extent extents[4] = { plan.renderR, plan.guidanceG, plan.neuralN, plan.displayD };
		const char* names[4] = { "R", "G", "N", "D" };
		ImVec2 halves[4];
		for (int i = 0; i < 4; ++i)
			halves[i] = halfSize(extents[i]);

		const char* arrowLabels[3] = { "SR", "DS", "UD" };
		for (int i = 0; i < 3; ++i) {
			const ImVec2 from{ xs[i] + halves[i].x, rowY };
			const ImVec2 to{ xs[i + 1] - halves[i + 1].x, rowY };
			DrawArrow(drawList, from, to, dim);
			const ImVec2 mid{ (from.x + to.x) * 0.5f, rowY - 12.0f };
			const ImVec2 labelSize = ImGui::CalcTextSize(arrowLabels[i]);
			drawList->AddText(ImVec2(mid.x - labelSize.x * 0.5f, mid.y - labelSize.y), dim, arrowLabels[i]);
		}

		for (int i = 0; i < 4; ++i) {
			const ImVec2 min{ xs[i] - halves[i].x, rowY - halves[i].y };
			const ImVec2 max{ xs[i] + halves[i].x, rowY + halves[i].y };
			drawList->AddRectFilled(min, max, fill, 4.0f);
			drawList->AddRect(min, max, border, 4.0f);
			const ImVec2 nameSize = ImGui::CalcTextSize(names[i]);
			drawList->AddText(ImVec2(xs[i] - nameSize.x * 0.5f, rowY - nameSize.y * 0.5f), text, names[i]);
			char sizeLabel[32];
			if (extents[i].width && extents[i].height)
				std::snprintf(sizeLabel, sizeof(sizeLabel), "%ux%u", extents[i].width, extents[i].height);
			else
				std::snprintf(sizeLabel, sizeof(sizeLabel), "--");
			const ImVec2 sizeText = ImGui::CalcTextSize(sizeLabel);
			drawList->AddText(ImVec2(xs[i] - sizeText.x * 0.5f, rowY + halves[i].y + 4.0f), dim, sizeLabel);
		}

		const ImVec2 nrFrom{ xs[2], rowY + halves[2].y + 18.0f };
		drawList->AddCircle(ImVec2(xs[2], rowY), halves[2].x + 5.0f, dim, 24, 1.0f);
		const ImVec2 nrLabel = ImGui::CalcTextSize("NR");
		drawList->AddText(ImVec2(xs[2] - nrLabel.x * 0.5f, nrFrom.y), dim, "NR");

		if (fusionOn) {
			const ImVec2 g{ xs[1], rowY + halves[1].y };
			const ImVec2 d{ xs[3], rowY + halves[3].y };
			const ImVec2 bend{ (g.x + d.x) * 0.5f, origin.y + height - 8.0f };
			const ImVec2 fusionPath[3]{ g, bend, d };
			drawList->AddPolyline(fusionPath, 3, dim, ImDrawFlags_None, 1.5f);
			const char* guidance = "Guidance";
			const ImVec2 guidanceSize = ImGui::CalcTextSize(guidance);
			drawList->AddText(ImVec2(bend.x - guidanceSize.x * 0.5f, bend.y - guidanceSize.y - 2.0f), dim, guidance);
		}
	}

	const char* SpatialUpsampleName(FoveatedRender::FlatAct2SrBackend backend)
	{
		switch (backend) {
		case FoveatedRender::FlatAct2SrBackend::kTent:
			return T(TKEY("fusion_reconstruct_tent"), "Tent");
		case FoveatedRender::FlatAct2SrBackend::kMitchell:
			return T(TKEY("act2_backend_mitchell"), "Mitchell");
		case FoveatedRender::FlatAct2SrBackend::kEasu:
			return T(TKEY("act2_backend_easu"), "EASU");
		default:
			return T(TKEY("act2_backend_lanczos"), "HQ Lanczos");
		}
	}

	void DrawSixPassOverview(const FoveatedRender::FlatPipelinePlan& plan, bool fusionOn, bool rcasOn)
	{
		const bool haveSize = plan.renderR.width && plan.guidanceG.width && plan.neuralN.width && plan.displayD.width;
		if (fusionOn && rcasOn)
			ImGui::TextDisabled(T(TKEY("six_pass_order_fusion_rcas"),
				"Execution: ACT1 SR -> ACT1 DS -> NR -> %s -> Guidance -> RCAS"), SpatialUpsampleName(plan.act2SrBackend));
		else if (fusionOn)
			ImGui::TextDisabled(T(TKEY("six_pass_order_fusion"),
				"Execution: ACT1 SR -> ACT1 DS -> NR -> %s -> Guidance"), SpatialUpsampleName(plan.act2SrBackend));
		else
			ImGui::TextDisabled(T(TKEY("six_pass_order_spatial"),
				"Execution: ACT1 SR -> ACT1 DS -> NR -> %s -> RCAS"), SpatialUpsampleName(plan.act2SrBackend));
		if (haveSize) {
			ImGui::TextDisabled(T(TKEY("six_pass_step_act1_sr"), "1. ACT1 SR   R %ux%u  ->  G %ux%u"),
				plan.renderR.width, plan.renderR.height, plan.guidanceG.width, plan.guidanceG.height);
			ImGui::TextDisabled(T(TKEY("six_pass_step_act1_ds"), "2. ACT1 DS   G %ux%u  ->  N %ux%u"),
				plan.guidanceG.width, plan.guidanceG.height, plan.neuralN.width, plan.neuralN.height);
			ImGui::TextDisabled(T(TKEY("six_pass_step_nr"), "3. NR        rebuild at N %ux%u"),
				plan.neuralN.width, plan.neuralN.height);
			ImGui::TextDisabled(T(TKEY("six_pass_step_spatial"), "4. %s      N %ux%u  ->  D %ux%u"),
				SpatialUpsampleName(plan.act2SrBackend),
				plan.neuralN.width, plan.neuralN.height, plan.displayD.width, plan.displayD.height);
			if (fusionOn)
				ImGui::TextDisabled(T(TKEY("six_pass_step_guidance"), "5. Guidance  RC@D + gain x (G %ux%u - low G)"),
					plan.guidanceG.width, plan.guidanceG.height);
			if (fusionOn && rcasOn)
				ImGui::TextDisabled(T(TKEY("six_pass_step_rcas_after_guidance"), "6. RCAS      sharpen D %ux%u"),
					plan.displayD.width, plan.displayD.height);
			else if (!fusionOn)
				ImGui::TextDisabled(T(TKEY("six_pass_step_rcas"), "5. RCAS      sharpen D %ux%u"),
					plan.displayD.width, plan.displayD.height);
		} else {
			ImGui::TextDisabled("%s", T(TKEY("six_pass_step_act1_sr_nosize"), "1. ACT1 SR   Scene R -> Guidance G"));
			ImGui::TextDisabled("%s", T(TKEY("six_pass_step_act1_ds_nosize"), "2. ACT1 DS   Guidance G -> Neural N"));
			ImGui::TextDisabled("%s", T(TKEY("six_pass_step_nr_nosize"), "3. NR        rebuild at Neural N"));
			ImGui::TextDisabled("%s", T(TKEY("six_pass_step_act2_sr_nosize"), "4. ACT2 UD   Neural N -> Display D"));
			if (fusionOn)
				ImGui::TextDisabled("%s", T(TKEY("six_pass_step_guidance_nosize"),
					"5. Guidance  RC@D + fitted gain x G detail above the N band"));
			if (fusionOn && rcasOn)
				ImGui::TextDisabled("%s", T(TKEY("six_pass_step_rcas_after_guidance_nosize"), "6. RCAS      sharpen Display D"));
			else if (!fusionOn)
				ImGui::TextDisabled("%s", T(TKEY("six_pass_step_rcas_nosize"), "5. RCAS      sharpen Display D"));
		}
		if (ImGui::TreeNode(T(TKEY("six_pass_diagram"), "Size and Guidance"))) {
			DrawSixPassDiagram(plan, fusionOn);
			ImGui::TextWrapped("%s", T(TKEY("six_pass_diagram_help"),
				"Box size follows each domain's pixel count. ACT1 SR grows R to G. ACT1 DS shrinks G to N for NR. ACT2 UD lifts N to RC@D. Guidance fits NR's gain at N and adds gain x G detail to RC@D."));
			ImGui::TreePop();
		}
	}
}

void FoveatedRender::DrawNeuralRenderingSettings(bool wrapHeader)
{
	ClampSettings();

	if (wrapHeader && !ImGui::CollapsingHeader(T(TKEY("neural_rendering_header"), "DLSS Neural Rendering"), ImGuiTreeNodeFlags_DefaultOpen))
		return;

	{
		auto& upscaling = globals::features::upscaling;
		const char* qualityModes[] = {
			T(TKEY("quality_dlaa"), "DLAA / Native"),
			T(TKEY("quality_quality"), "Quality"),
			T(TKEY("quality_balanced"), "Balanced"),
			T(TKEY("quality_performance"), "Performance"),
			T(TKEY("quality_ultra_performance"), "Ultra Performance")
		};
		const char* dlssPresets[] = {
			T(TKEY("dlss_preset_default"), "Default"),
			T(TKEY("dlss_preset_j"), "Preset J"),
			T(TKEY("dlss_preset_k"), "Preset K"),
			T(TKEY("dlss_preset_l"), "Preset L"),
			T(TKEY("dlss_preset_m"), "Preset M")
		};
		const char* jitterModes[] = {
			T(TKEY("jitter_actual"), "Actual Camera Jitter"),
			T(TKEY("jitter_zero"), "Zero")
		};
		const bool showSixPass = true;
		if (showSixPass) {
			const char* act1Backends[] = {
				T(TKEY("act1_backend_bypass"), "Bypass"),
				T(TKEY("act1_backend_dlss"), "DLSS"),
				T(TKEY("act1_backend_fsr3"), "FSR3")
			};
			const bool model3Preview = !globals::game::isVR && upscaling.dx12SwapChain.useArise;
			const bool vrSixPreview = globals::game::isVR;
			const auto& previewUV = subrectController.GetUV();
			uint32_t vrRenderW = 0, vrRenderH = 0, vrDisplayW = 0, vrDisplayH = 0;
			if (vrSixPreview)
				NeuralRendering::GetVrOwnedEyeSizes(vrRenderW, vrRenderH, vrDisplayW, vrDisplayH);
			const uint32_t vrPreviewInW = vrSixPreview ?
				std::max(1u, static_cast<uint32_t>(vrRenderW * previewUV.w)) : 0u;
			const uint32_t vrPreviewInH = vrSixPreview ?
				std::max(1u, static_cast<uint32_t>(vrRenderH * previewUV.h)) : 0u;
			const uint32_t vrPreviewOutW = vrSixPreview ?
				std::max(1u, static_cast<uint32_t>(vrDisplayW * previewUV.w)) : 0u;
			const uint32_t vrPreviewOutH = vrSixPreview ?
				std::max(1u, static_cast<uint32_t>(vrDisplayH * previewUV.h)) : 0u;
			const auto previewPlan = BuildFlatPipelinePlan(
				vrSixPreview ? vrPreviewOutW :
					(model3Preview ? upscaling.dx12SwapChain.PhysicalWidth() :
						(globals::state ? static_cast<uint32_t>(globals::state->screenSize.x) : 0u)),
				vrSixPreview ? vrPreviewOutH :
					(model3Preview ? upscaling.dx12SwapChain.PhysicalHeight() :
						(globals::state ? static_cast<uint32_t>(globals::state->screenSize.y) : 0u)),
				vrSixPreview ? vrPreviewInW :
					(model3Preview ? upscaling.dx12SwapChain.LogicalWidth() : 0u),
				vrSixPreview ? vrPreviewInH :
					(model3Preview ? upscaling.dx12SwapChain.LogicalHeight() : 0u));
			ImGui::TextDisabled("%s", globals::game::isVR ?
				T(TKEY("injection_point_vr"), "Injection Point: After Tonemap on kTEMP (KIT TS)") :
				T(TKEY("injection_point"), "Injection Point: PostInject LDR / Before UI (fixed)"));
			if (globals::game::isVR)
				ImGui::TextDisabled("%s", T(TKEY("vr_six_pass_note"),
					"Read/write kTEMP per eye, then engine blit to kTOTAL. Engine Submit is unchanged. No FG."));
			DrawSixPassOverview(previewPlan, settings.unifiedPipeline.fusionEnabled,
				settings.flatPipelineSharpen);
			ImGui::Separator();
			ImGui::TextUnformatted(T(TKEY("act1_sr_header"), "ACT1 SR - Scene R to Guidance G"));
			if (WantsLogicalPerfMode())
				ImGui::TextDisabled("%s", T(TKEY("act1_sr_backend_fixed"), "ACT1 SR Backend: DLSS (fixed for ARISE)"));
			else
				ImGui::Combo(T(TKEY("act1_sr_backend"), "ACT1 SR Backend"), reinterpret_cast<int*>(&settings.flatPipelineAct1Backend),
					act1Backends, IM_ARRAYSIZE(act1Backends));
			if (settings.flatPipelineAct1Backend != 0) {
				ImGui::Combo(WantsLogicalPerfMode() ?
					T(TKEY("engine_r_preset"), "Engine R Preset (Restart)") :
					T(TKEY("act1_quality"), "ACT1 Quality"),
					reinterpret_cast<int*>(&settings.flatPipelineAct1Quality), qualityModes, IM_ARRAYSIZE(qualityModes));
				if (WantsLogicalPerfMode()) {
					Util::UI::RestartGatedAnnotate(bootSnapshot, settings, &Settings::flatPipelineAct1Quality,
						T(TKEY("engine_r_preset_help"),
							"Defines the logical scene resolution allocated from physical display resolution."));
					if (bootSnapshot.HasPendingChange(settings, &Settings::flatPipelineAct1Quality)) {
						const auto activeQuality = std::min(
							bootSnapshot.Boot(&Settings::flatPipelineAct1Quality),
							static_cast<uint>(IM_ARRAYSIZE(qualityModes) - 1));
						ImGui::TextDisabled(T(TKEY("engine_r_preset_active"),
							"Active this session: %s (restart to apply selection)"), qualityModes[activeQuality]);
					}
				}
				if (settings.flatPipelineAct1Backend == static_cast<uint>(FlatAct1Backend::kDLSS)) {
					ImGui::Checkbox(T(TKEY("srss"), "Super Resolution Super Sampling (SRSS)"), &settings.unifiedPipeline.act1Srss);
					if (WantsLogicalPerfMode())
						Util::UI::RestartGatedAnnotate(unifiedBootSnapshot, settings.unifiedPipeline,
							&UnifiedPipelineSettings::act1Srss, T(TKEY("srss_restart"), "Changes the ACT1 Guidance G resource contract."));
					if (!settings.unifiedPipeline.act1Srss)
						ImGui::TextDisabled("%s", T(TKEY("srss_off"), "SRSS Off: Guidance G equals Display D"));
					if (settings.unifiedPipeline.act1Srss) {
						const char* srssModes[] = {
							T(TKEY("quality_performance"), "Performance"),
							T(TKEY("quality_ultra_performance"), "Ultra Performance")
						};
						int srssMode = static_cast<int>(settings.unifiedPipeline.act1SrssQuality - 3);
						if (ImGui::Combo(T(TKEY("srss_mode"), "SRSS Mode"), &srssMode, srssModes, IM_ARRAYSIZE(srssModes)))
							settings.unifiedPipeline.act1SrssQuality = static_cast<uint>(srssMode + 3);
						if (WantsLogicalPerfMode())
							Util::UI::RestartGatedAnnotate(unifiedBootSnapshot, settings.unifiedPipeline,
								&UnifiedPipelineSettings::act1SrssQuality);
						if (settings.unifiedPipeline.act1SrssQuality == 3) {
							ImGui::TextDisabled("%s", T(TKEY("srss_preset_k"), "SRSS Preset: Preset K (fixed)"));
						} else {
							const char* srssPresets[] = {
								T(TKEY("dlss_preset_l"), "Preset L"),
								T(TKEY("dlss_preset_m"), "Preset M")
							};
							int srssPreset = static_cast<int>(settings.unifiedPipeline.act1SrssPreset - 3);
							if (ImGui::Combo(T(TKEY("srss_preset"), "SRSS Preset"), &srssPreset, srssPresets, IM_ARRAYSIZE(srssPresets)))
								settings.unifiedPipeline.act1SrssPreset = static_cast<uint>(srssPreset + 3);
							if (WantsLogicalPerfMode())
								Util::UI::RestartGatedAnnotate(unifiedBootSnapshot, settings.unifiedPipeline,
									&UnifiedPipelineSettings::act1SrssPreset);
						}
						ImGui::SliderFloat(T(TKEY("guidance_scale"), "Guidance Scale (G/D)"), &settings.unifiedPipeline.guidanceScale,
							1.0f, 2.0f, "%.2fx");
						if (WantsLogicalPerfMode())
							Util::UI::RestartGatedAnnotate(unifiedBootSnapshot, settings.unifiedPipeline,
								&UnifiedPipelineSettings::guidanceScale);
						const auto active = ActiveUnifiedPipelineSettings();
						if (WantsLogicalPerfMode() &&
							(unifiedBootSnapshot.HasPendingChange(settings.unifiedPipeline, &UnifiedPipelineSettings::act1Srss) ||
								unifiedBootSnapshot.HasPendingChange(settings.unifiedPipeline, &UnifiedPipelineSettings::act1SrssQuality) ||
								unifiedBootSnapshot.HasPendingChange(settings.unifiedPipeline, &UnifiedPipelineSettings::act1SrssPreset) ||
								unifiedBootSnapshot.HasPendingChange(settings.unifiedPipeline, &UnifiedPipelineSettings::guidanceScale)))
							ImGui::TextDisabled(T(TKEY("srss_active_session"),
								"Active this session: %s, mode %u, preset %u, G/D %.2fx"),
								active.act1Srss ? T(TKEY("srss_on"), "SRSS On") : T(TKEY("srss_off_label"), "SRSS Off"),
								active.act1SrssQuality,
								active.act1SrssPreset, active.guidanceScale);
						ImGui::TextDisabled("%s", T(TKEY("srss_recommended"), "Recommended: Ultra Performance + Preset M"));
					} else {
						ImGui::Combo(T(TKEY("act1_dlss_preset"), "ACT1 DLSS Preset"), reinterpret_cast<int*>(&settings.flatPipelineAct1Preset),
							dlssPresets, IM_ARRAYSIZE(dlssPresets));
					}
				}
				ImGui::Combo(T(TKEY("act1_jitter"), "ACT1 Jitter Submission"), reinterpret_cast<int*>(&settings.flatPipelineAct1Jitter), jitterModes, IM_ARRAYSIZE(jitterModes));
			}
			ImGui::TextUnformatted(T(TKEY("act1_ds_header"), "ACT1 DS - Guidance G to Neural N"));
			{
				const char* downsampleFilters[] = {
					T(TKEY("act2_backend_lanczos"), "HQ Lanczos"),
					T(TKEY("fusion_reconstruct_tent"), "Tent")
				};
				ImGui::Combo(T(TKEY("act1_ds_method"), "ACT1 DS Color"),
					reinterpret_cast<int*>(&settings.unifiedPipeline.act1Downsample),
					downsampleFilters, IM_ARRAYSIZE(downsampleFilters));
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("act1_ds_method_tooltip"),
						"How G color is reduced to N before Neural Rendering. Independent of N to D."));
				}
				const char* depthFilters[] = {
					T(TKEY("act1_ds_depth_min2x2"), "Min 2x2"),
					T(TKEY("fusion_reconstruct_tent"), "Tent"),
					T(TKEY("act2_backend_lanczos"), "HQ Lanczos")
				};
				ImGui::Combo(T(TKEY("act1_ds_depth"), "ACT1 DS Depth"),
					reinterpret_cast<int*>(&settings.unifiedPipeline.act1DepthDownsample),
					depthFilters, IM_ARRAYSIZE(depthFilters));
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("act1_ds_depth_tooltip"),
						"How engine depth is reduced to N. Tent/Lanczos use the same G texel centers and kernel as color; Min 2x2 keeps nearest-surface on R."));
				}
			}
			ImGui::Separator();
		}

		const bool model3Route = !globals::game::isVR && upscaling.dx12SwapChain.useArise;
		const bool supportedRoute = model3Route ||
			(globals::features::upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS &&
				(globals::game::isVR || !globals::features::upscaling.IsFrameGenerationConfiguredForSession()));
		if (!supportedRoute) {
			if (!globals::game::isVR && globals::features::upscaling.IsFrameGenerationConfiguredForSession())
				Util::Text::Warning(T(TKEY("neural_rendering_disable_fg"),
					"Disable Frame Generation and restart the game before enabling DLSS Neural Rendering."));
			else
				Util::Text::Warning(T(TKEY("neural_rendering_unavailable"),
					"Requires DLSS."));
			ImGui::BeginDisabled();
		}
		ImGui::TextUnformatted(T(TKEY("neural_rendering"), "Neural Rendering"));
		ImGui::Checkbox(T(TKEY("neural_rendering_enable"), "Enable DLSS Neural Rendering"), &settings.neuralRenderingEnabled);

		if (settings.neuralRenderingEnabled) {
			if (showSixPass) {
				ImGui::Separator();
				ImGui::TextUnformatted(T(TKEY("nr_header"), "NR"));
				ImGui::Checkbox(T(TKEY("nr_native"), "NR 1:1 (no scale)"), &settings.flatPipelineNrNative);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("nr_native_tooltip"),
						"Run Neural Rendering at Display D. Skip NR downsample. ACT2 is identity; Guidance still sharpens RC@D if enabled."));
				}
				ImGui::BeginDisabled(settings.flatPipelineNrNative);
				ImGui::SliderFloat(T(TKEY("nr_resolution_scale"), "NR Resolution Scale"), &settings.flatPipelineNrScale, 0.33f, 0.95f, "%.2f");
				ImGui::EndDisabled();
				{
					const bool model3 = !globals::game::isVR && globals::features::upscaling.dx12SwapChain.useArise;
					const bool vrSix = globals::game::isVR;
					uint32_t vrRenderW = 0, vrRenderH = 0, vrDisplayW = 0, vrDisplayH = 0;
					if (vrSix)
						NeuralRendering::GetVrOwnedEyeSizes(vrRenderW, vrRenderH, vrDisplayW, vrDisplayH);
					const auto preview = BuildFlatPipelinePlan(
						vrSix ? vrDisplayW :
							(model3 ? globals::features::upscaling.dx12SwapChain.PhysicalWidth() :
								(globals::state ? static_cast<uint32_t>(globals::state->screenSize.x) : 0u)),
						vrSix ? vrDisplayH :
							(model3 ? globals::features::upscaling.dx12SwapChain.PhysicalHeight() :
								(globals::state ? static_cast<uint32_t>(globals::state->screenSize.y) : 0u)),
						vrSix ? vrRenderW :
							(model3 ? globals::features::upscaling.dx12SwapChain.LogicalWidth() : 0u),
						vrSix ? vrRenderH :
							(model3 ? globals::features::upscaling.dx12SwapChain.LogicalHeight() : 0u));
					const double displayPixels = static_cast<double>(preview.displayWidth) * preview.displayHeight;
					const double nrPixels = static_cast<double>(preview.nrWidth) * preview.nrHeight;
					ImGui::TextDisabled(T(TKEY("neural_n_percent"),
						"Neural N: %.1f%% of Display D pixels"),
						displayPixels > 0.0 ? nrPixels * 100.0 / displayPixels : 0.0);
				}
			}

			ImGui::Separator();
			ImGui::TextUnformatted(T(TKEY("nr_tuning"), "NR Tuning"));
			const char* presets[] = {
				T(TKEY("nr_preset_custom"), "Custom"),
				T(TKEY("nr_preset_balanced"), "Balanced"),
				T(TKEY("nr_preset_fabric"), "Fabric Detail"),
				T(TKEY("nr_preset_natural"), "Natural"),
				T(TKEY("nr_preset_strong"), "Strong")
			};
			int preset = static_cast<int>(settings.neuralRenderingPreset);
			if (ImGui::Combo(T(TKEY("neural_rendering_preset"), "Tuning Preset"), &preset, presets, IM_ARRAYSIZE(presets))) {
				settings.neuralRenderingPreset = static_cast<uint>(preset);
				switch (settings.neuralRenderingPreset) {
				case 1: settings.neuralRenderingIntensity = 1.0f; settings.neuralRenderingLocalTone = 1.0f; settings.neuralRenderingLocalStructure = 1.0f; settings.neuralRenderingSkinStructure = 1.0f; break;
				case 2: settings.neuralRenderingIntensity = 1.35f; settings.neuralRenderingLocalTone = 0.9f; settings.neuralRenderingLocalStructure = 1.6f; settings.neuralRenderingSkinStructure = 1.15f; break;
				case 3: settings.neuralRenderingIntensity = 0.8f; settings.neuralRenderingLocalTone = 0.75f; settings.neuralRenderingLocalStructure = 0.9f; settings.neuralRenderingSkinStructure = 0.9f; break;
				case 4: settings.neuralRenderingIntensity = 1.75f; settings.neuralRenderingLocalTone = 1.25f; settings.neuralRenderingLocalStructure = 1.5f; settings.neuralRenderingSkinStructure = 1.3f; break;
				default: break;
				}
			}
			bool custom = false;
			custom |= ImGui::SliderFloat(T(TKEY("neural_rendering_intensity"), "Intensity"), &settings.neuralRenderingIntensity, 0.0f, 2.0f, "%.2f");
			custom |= ImGui::SliderFloat(T(TKEY("neural_rendering_local_tone"), "Local Tone"), &settings.neuralRenderingLocalTone, 0.0f, 2.0f, "%.2f");
			custom |= ImGui::SliderFloat(T(TKEY("neural_rendering_local_structure"), "Local Structure"), &settings.neuralRenderingLocalStructure, 0.0f, 2.0f, "%.2f");
			custom |= ImGui::SliderFloat(T(TKEY("neural_rendering_skin_structure"), "Skin Structure"), &settings.neuralRenderingSkinStructure, 0.0f, 2.0f, "%.2f");
			const char* styles[] = {
				T(TKEY("nr_style_0"), "Style 0"),
				T(TKEY("nr_style_1"), "Style 1"),
				T(TKEY("nr_style_2"), "Style 2"),
				T(TKEY("nr_style_3"), "Style 3")
			};
			int style = static_cast<int>(settings.neuralRenderingStyle);
			if (ImGui::Combo(T(TKEY("neural_rendering_style"), "Style"), &style, styles, IM_ARRAYSIZE(styles))) {
				settings.neuralRenderingStyle = static_cast<uint>(style);
				custom = true;
			}
			custom |= ImGui::Checkbox(T(TKEY("neural_rendering_auto_mask"), "Automatic Mask"), &settings.neuralRenderingAutoMask);
			custom |= ImGui::Checkbox(T(TKEY("neural_rendering_ui_correction"), "UI Correction"), &settings.neuralRenderingUICorrection);
			if (custom)
				settings.neuralRenderingPreset = 0;

			if (showSixPass) {
				{
					ImGui::Separator();
					ImGui::TextUnformatted(T(TKEY("n_to_d_header"), "N to D"));
					const char* displayMethods[] = {
						T(TKEY("act2_backend_lanczos"), "HQ Lanczos"),
						T(TKEY("fusion_reconstruct_tent"), "Tent"),
						T(TKEY("act2_backend_mitchell"), "Mitchell"),
						T(TKEY("act2_backend_easu"), "EASU")
					};
					int displayMethod =
						settings.unifiedPipeline.act2SrBackend == static_cast<uint>(FlatAct2SrBackend::kTent) ? 1 :
						settings.unifiedPipeline.act2SrBackend == static_cast<uint>(FlatAct2SrBackend::kMitchell) ? 2 :
						settings.unifiedPipeline.act2SrBackend == static_cast<uint>(FlatAct2SrBackend::kEasu) ? 3 : 0;
					if (ImGui::Combo(T(TKEY("n_to_d_method"), "N to D Method"), &displayMethod, displayMethods, IM_ARRAYSIZE(displayMethods))) {
						settings.unifiedPipeline.act2SrBackend = displayMethod == 1 ?
							static_cast<uint>(FlatAct2SrBackend::kTent) :
							displayMethod == 2 ? static_cast<uint>(FlatAct2SrBackend::kMitchell) :
							displayMethod == 3 ? static_cast<uint>(FlatAct2SrBackend::kEasu) :
							static_cast<uint>(FlatAct2SrBackend::kLanczos);
					}
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T(TKEY("n_to_d_tooltip"),
							"ACT2 UD only. Mitchell and Tent stay soft. Guidance is a later pass that sharpens RC@D; it is not an upsampler."));
					}
					ImGui::TextDisabled("%s", T(TKEY("n_to_d_spatial_help"),
						"Soft upsample of native N to RC@D. RD follows this same UD implicitly."));
					ImGui::Checkbox(T(TKEY("fusion_after_act2"), "Guidance after ACT2"), &settings.unifiedPipeline.fusionEnabled);
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T(TKEY("fusion_after_act2_tooltip"),
							"After ACT2, add gain x (G - UD(NR input)) to RC@D. Gain is fitted from NR's own input/output at N, so G lends detail but no tone. RCAS can still run after this."));
					}
					if (settings.unifiedPipeline.fusionEnabled) {
						ImGui::TextDisabled("%s", T(TKEY("fusion_is_act2"),
							"Fits NR as a local affine map of its own input at N. D = RC + a x (G - UD(NR input)). Same-side fit; a blends to prior by ρ². RD only drops cross-cliff samples."));
						if (ImGui::CollapsingHeader(T(TKEY("fusion_grp_fit"), "Guidance · Fit"), ImGuiTreeNodeFlags_DefaultOpen)) {
							ImGui::SliderFloat(T(TKEY("fusion_softness"), "Fit Window"), &settings.flatPipelineFusionSoftness, 0.0f, 1.0f, "%.2f");
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("%s", T(TKEY("fusion_softness_tooltip"),
									"Regression window on the N grid. Below 0.5 = 3x3, above = 5x5. Wider is smoother gain, less local."));
							}
							ImGui::SliderFloat(T(TKEY("fusion_residual"), "Regularization"), &settings.neuralRenderingFusionResidualLimit, 0.01f, 0.5f, "%.2f");
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("%s", T(TKEY("fusion_residual_tooltip"),
									"Epsilon in gain = cov / (var + eps). Higher damps gain in flat, low-variance areas; lower trusts every small correlation."));
							}
							ImGui::SliderFloat(T(TKEY("fusion_residual_gain"), "Max Gain"), &settings.flatPipelineFusionFrequencyGuidance, 0.0f, 1.0f, "%.2f");
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("%s", T(TKEY("fusion_frequency_tooltip"),
									"Clip for the fitted gain: 0 = 1x, 1 = 3x. Pansharpening keeps this at 2-3 to avoid over-enhancement."));
							}
							ImGui::SliderFloat(T(TKEY("fusion_gain_prior"), "Gain Prior"), &settings.fusionGainPrior, 0.0f, 1.5f, "%.2f");
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("%s", T(TKEY("fusion_gain_prior_tooltip"),
									"gain = (cov + eps x prior) / (var + eps). Where the NR input had no variance the model never saw the structure; 1 lets G restore it, 0 treats NR's silence as a decision."));
							}
							ImGui::SliderFloat(T(TKEY("fusion_side_sigma"), "Side Sigma"), &settings.fusionFitSideSigma, 0.02f, 0.5f, "%.2f");
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("%s", T(TKEY("fusion_side_sigma_tooltip"),
									"Same-side regression: samples across an RD cliff are dropped, then weighted by luma proximity. Lower = stricter."));
							}
						}
						if (ImGui::CollapsingHeader(T(TKEY("fusion_grp_depth"), "Guidance · Depth"), ImGuiTreeNodeFlags_DefaultOpen)) {
							ImGui::SliderFloat(T(TKEY("fusion_depth"), "Depth Band"), &settings.flatPipelineFusionDepthGuidance, 0.0f, 1.0f, "%.2f");
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("%s", T(TKEY("fusion_depth_tooltip"),
									"RD cliff threshold on the engine grid. Never use ND. Higher finds weaker Z splits."));
							}
						}
						if (ImGui::CollapsingHeader(T(TKEY("fusion_grp_hf"), "Guidance · Inject"), ImGuiTreeNodeFlags_DefaultOpen)) {
							ImGui::SliderFloat(T(TKEY("fusion_luma"), "Detail Luma"), &settings.neuralRenderingFusionLuma, 0.0f, 1.5f, "%.2f");
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("%s", T(TKEY("fusion_luma_tooltip"),
									"Scale on the luma part of gain x detail. 1 reproduces the fitted model."));
							}
							ImGui::SliderFloat(T(TKEY("fusion_chroma"), "Detail Chroma"), &settings.neuralRenderingFusionChroma, 0.0f, 1.5f, "%.2f");
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("%s", T(TKEY("fusion_chroma_tooltip"),
									"Scale on the chroma part of gain x detail. Lower keeps NR's hue where G adds color texture."));
							}
						}
						if (ImGui::CollapsingHeader(T(TKEY("fusion_grp_bands"), "Guidance · NR Bands"), ImGuiTreeNodeFlags_DefaultOpen)) {
							ImGui::TextDisabled("%s", T(TKEY("fusion_bands_help"),
								"NR edit E = RC - UD(NR input). Low band = 5x5 mean at N (tone, shadows); high band = the rest (NR's own texture). 1 = as NR made it."));
							ImGui::SliderFloat(T(TKEY("fusion_tone_gain_luma"), "NR Tone Gain (Luma)"), &settings.fusionToneGainLuma, 0.0f, 3.0f, "%.2f");
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("%s", T(TKEY("fusion_tone_gain_luma_tooltip"),
									"Scales the luma of NR's low-band edit. Above 1 makes soft low-resolution shading decisions bolder; it cannot narrow them."));
							}
							ImGui::SliderFloat(T(TKEY("fusion_tone_gain_chroma"), "NR Tone Gain (Chroma)"), &settings.fusionToneGainChroma, 0.0f, 3.0f, "%.2f");
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("%s", T(TKEY("fusion_tone_gain_chroma_tooltip"),
									"Scales the chroma of NR's low-band edit. NR hue shifts are usually the part to keep light."));
							}
							ImGui::SliderFloat(T(TKEY("fusion_detail_gain"), "NR Detail Gain"), &settings.fusionDetailGain, 0.0f, 3.0f, "%.2f");
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("%s", T(TKEY("fusion_detail_gain_tooltip"),
									"Scales NR's high-band edit: texture the model added that G does not have. Crisper, never finer than the N grid."));
							}
							ImGui::SliderFloat(T(TKEY("fusion_dir_sharpen"), "E_hf Directional Sharpen"), &settings.fusionDirectionalSharpen, 0.0f, 2.0f, "%.2f");
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("%s", T(TKEY("fusion_dir_sharpen_tooltip"),
									"Unsharp NR's own high band along the G gradient. 0 is off. Does not invent frequencies above N."));
							}
							ImGui::SliderFloat(T(TKEY("fusion_dither"), "Band Dither"), &settings.fusionDither, 0.0f, 2.0f, "%.2f");
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("%s", T(TKEY("fusion_dither_tooltip"),
									"Triangular dither in 8-bit steps, active only while a band gain is not 1. Hides the banding amplified low-band edits expose."));
							}
						}
						if (ImGui::CollapsingHeader(T(TKEY("fusion_grp_compare"), "Guidance · Compare"), ImGuiTreeNodeFlags_DefaultOpen)) {
							const char* compareModes[] = {
								T(TKEY("fusion_compare_fitted"), "Fitted gain"),
								T(TKEY("fusion_compare_unit"), "Force gain = 1"),
								T(TKEY("fusion_compare_split"), "Split: fitted | gain 1"),
								T(TKEY("fusion_compare_split_swap"), "Split: gain 1 | fitted")
							};
							ImGui::Combo(T(TKEY("fusion_compare_mode"), "Gain A/B"), reinterpret_cast<int*>(&settings.fusionCompareMode), compareModes, IM_ARRAYSIZE(compareModes));
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("%s", T(TKEY("fusion_compare_tooltip"),
									"Gain 1 adds G's detail band at unit strength everywhere (the OptiScaler matched-residual form). Fitted gain scales it by how much contrast NR kept locally."));
							}
							const char* debugViews[] = {
								T(TKEY("fusion_debug_off"), "Off"),
								T(TKEY("fusion_debug_gain"), "Gain map (0.5 = 1x)"),
								T(TKEY("fusion_debug_detail"), "G detail band"),
								T(TKEY("fusion_debug_edit"), "NR edit"),
								T(TKEY("fusion_debug_edit_low"), "NR edit: low band"),
								T(TKEY("fusion_debug_edit_high"), "NR edit: high band"),
								T(TKEY("fusion_debug_rd"), "RD mask"),
								T(TKEY("fusion_debug_hard_band"), "Energy-gap w"),
								T(TKEY("fusion_debug_soft_band"), "(unused)"),
								T(TKEY("fusion_debug_alias_w"), "Fit ρ²")
							};
							ImGui::Combo(T(TKEY("fusion_debug_view"), "Debug View"), reinterpret_cast<int*>(&settings.fusionDebugView), debugViews, IM_ARRAYSIZE(debugViews));
							if (ImGui::Button(T(TKEY("fusion_export_bands"), "Export bands (G / I / N / RC / Lo / Gain / D)")))
								NeuralRendering::RequestBandCapture();
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("%s", T(TKEY("fusion_export_bands_tooltip"),
									"Writes raw float32 dumps and a manifest to Data/SKSE/Plugins/CommunityShaders/PrivateNR for tools/nr_band_analysis.py. D3D11 six-pass only."));
							}
						}
					}
				}
			}
		}
		if (!supportedRoute)
			ImGui::EndDisabled();
		if (showSixPass) {
			globals::features::upscaling.DrawFinalSharpenSettings();
			ImGui::Separator();
			ImGui::TextUnformatted(T(TKEY("act3_header"), "ACT3"));
			ImGui::Checkbox(T(TKEY("act3_enable"), "ACT3 Center NR"), &settings.act3Enabled);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("act3_enable_tooltip"),
					"After Final Sharpen, crop a fixed center of D, run a second NR, and write it back with a feathered mix at the crop edge."));
			}
			if (settings.act3Enabled) {
				ImGui::SliderFloat(T(TKEY("act3_scale"), "Center Scale"), &settings.act3Scale, 0.0f, 0.9f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("act3_scale_tooltip"),
						"Fixed center crop. 0.50 = half the full D width and height. 0 skips ACT3."));
				}
				ImGui::SliderFloat(T(TKEY("act3_feather"), "Writeback Feather"), &settings.act3FeatherWidth, 2.0f, 128.0f, "%.0f px");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("act3_feather_tooltip"),
						"Smoothstep band at the crop edge. Outer edge is first-pass D, inner edge is ACT3."));
				}
				ImGui::Checkbox(T(TKEY("act3_guidance"), "ACT3 Guidance"), &settings.act3GuidanceEnabled);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", T(TKEY("act3_guidance_tooltip"),
						"Same Guidance equation as ACT2, only on the ACT3 crop: G and ACT2 Lo stay shared, RC is ACT3 NR, knobs are this set. Crops the pre-Guidance RC so the center is not first-guided twice. Compare and debug stay off."));
				}
				if (settings.act3GuidanceEnabled) {
					ImGui::TextDisabled("%s", T(TKEY("act3_guidance_help"),
						"Background keeps the first Guidance. The crop runs ACT3 NR then this Guidance. The feather band lerps from first-pass D (outer) to ACT3 (inner)."));
					ImGui::SliderFloat(T(TKEY("act3_guidance_luma"), "ACT3 Detail Luma"), &settings.act3GuidanceLuma, 0.0f, 1.5f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T(TKEY("fusion_luma_tooltip"),
							"Scale on the luma part of gain x detail. 1 reproduces the fitted model."));
					}
					ImGui::SliderFloat(T(TKEY("act3_guidance_chroma"), "ACT3 Detail Chroma"), &settings.act3GuidanceChroma, 0.0f, 1.5f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T(TKEY("fusion_chroma_tooltip"),
							"Scale on the chroma part of gain x detail. Lower keeps NR's hue where G adds color texture."));
					}
					ImGui::SliderFloat(T(TKEY("act3_guidance_prior"), "ACT3 Gain Prior"), &settings.act3GuidancePrior, 0.0f, 1.5f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T(TKEY("fusion_gain_prior_tooltip"),
							"gain = (cov + eps x prior) / (var + eps). Where the NR input had no variance the model never saw the structure; 1 lets G restore it, 0 treats NR's silence as a decision."));
					}
					ImGui::SliderFloat(T(TKEY("act3_guidance_max_gain"), "ACT3 Max Gain"), &settings.act3GuidanceMaxGain, 0.0f, 1.0f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T(TKEY("fusion_frequency_tooltip"),
							"Clip for the fitted gain: 0 = 1x, 1 = 3x. Pansharpening keeps this at 2-3 to avoid over-enhancement."));
					}
					ImGui::SliderFloat(T(TKEY("act3_guidance_tone_luma"), "ACT3 NR Tone Gain (Luma)"), &settings.act3GuidanceToneLuma, 0.0f, 3.0f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T(TKEY("fusion_tone_gain_luma_tooltip"),
							"Scales the luma of NR's low-band edit. Above 1 makes soft low-resolution shading decisions bolder; it cannot narrow them."));
					}
					ImGui::SliderFloat(T(TKEY("act3_guidance_tone_chroma"), "ACT3 NR Tone Gain (Chroma)"), &settings.act3GuidanceToneChroma, 0.0f, 3.0f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T(TKEY("fusion_tone_gain_chroma_tooltip"),
							"Scales the chroma of NR's low-band edit. NR hue shifts are usually the part to keep light."));
					}
					ImGui::SliderFloat(T(TKEY("act3_guidance_detail_gain"), "ACT3 NR Detail Gain"), &settings.act3GuidanceDetailGain, 0.0f, 3.0f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T(TKEY("fusion_detail_gain_tooltip"),
							"Scales NR's high-band edit: texture the model added that G does not have. Crisper, never finer than the N grid."));
					}
					ImGui::SliderFloat(T(TKEY("act3_guidance_dir_sharpen"), "ACT3 E_hf Directional Sharpen"), &settings.act3GuidanceDirSharpen, 0.0f, 2.0f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T(TKEY("fusion_dir_sharpen_tooltip"),
							"Unsharp NR's own high band along the G gradient. 0 is off. Does not invent frequencies above N."));
					}
					ImGui::SliderFloat(T(TKEY("act3_guidance_dither"), "ACT3 Band Dither"), &settings.act3GuidanceDither, 0.0f, 2.0f, "%.2f");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("%s", T(TKEY("fusion_dither_tooltip"),
							"Triangular dither in 8-bit steps, active only while a band gain is not 1. Hides the banding amplified low-band edits expose."));
					}
				}
				const char* act3Presets[] = {
					T(TKEY("nr_preset_custom"), "Custom"),
					T(TKEY("nr_preset_balanced"), "Balanced"),
					T(TKEY("nr_preset_fabric"), "Fabric Detail"),
					T(TKEY("nr_preset_natural"), "Natural"),
					T(TKEY("nr_preset_strong"), "Strong")
				};
				int act3Preset = static_cast<int>(settings.act3Preset);
				if (ImGui::Combo(T(TKEY("act3_preset"), "ACT3 Tuning Preset"), &act3Preset, act3Presets, IM_ARRAYSIZE(act3Presets))) {
					settings.act3Preset = static_cast<uint>(act3Preset);
					switch (settings.act3Preset) {
					case 1: settings.act3Intensity = 1.0f; settings.act3LocalTone = 1.0f; settings.act3LocalStructure = 1.0f; settings.act3SkinStructure = 1.0f; break;
					case 2: settings.act3Intensity = 1.35f; settings.act3LocalTone = 0.9f; settings.act3LocalStructure = 1.6f; settings.act3SkinStructure = 1.15f; break;
					case 3: settings.act3Intensity = 0.8f; settings.act3LocalTone = 0.75f; settings.act3LocalStructure = 0.9f; settings.act3SkinStructure = 0.9f; break;
					case 4: settings.act3Intensity = 1.75f; settings.act3LocalTone = 1.25f; settings.act3LocalStructure = 1.5f; settings.act3SkinStructure = 1.3f; break;
					default: break;
					}
				}
				bool act3Custom = false;
				act3Custom |= ImGui::SliderFloat(T(TKEY("act3_intensity"), "ACT3 Intensity"), &settings.act3Intensity, 0.0f, 2.0f, "%.2f");
				act3Custom |= ImGui::SliderFloat(T(TKEY("act3_local_tone"), "ACT3 Local Tone"), &settings.act3LocalTone, 0.0f, 2.0f, "%.2f");
				act3Custom |= ImGui::SliderFloat(T(TKEY("act3_local_structure"), "ACT3 Local Structure"), &settings.act3LocalStructure, 0.0f, 2.0f, "%.2f");
				act3Custom |= ImGui::SliderFloat(T(TKEY("act3_skin_structure"), "ACT3 Skin Structure"), &settings.act3SkinStructure, 0.0f, 2.0f, "%.2f");
				const char* act3Styles[] = {
					T(TKEY("nr_style_0"), "Style 0"),
					T(TKEY("nr_style_1"), "Style 1"),
					T(TKEY("nr_style_2"), "Style 2"),
					T(TKEY("nr_style_3"), "Style 3")
				};
				int act3Style = static_cast<int>(settings.act3Style);
				if (ImGui::Combo(T(TKEY("act3_style"), "ACT3 Style"), &act3Style, act3Styles, IM_ARRAYSIZE(act3Styles))) {
					settings.act3Style = static_cast<uint>(act3Style);
					act3Custom = true;
				}
				act3Custom |= ImGui::Checkbox(T(TKEY("act3_auto_mask"), "ACT3 Automatic Mask"), &settings.act3AutoMask);
				act3Custom |= ImGui::Checkbox(T(TKEY("act3_ui_correction"), "ACT3 UI Correction"), &settings.act3UICorrection);
				ImGui::Checkbox(T(TKEY("act3_reset"), "ACT3 Reset Every Frame"), &settings.act3ResetEveryFrame);
				ImGui::Checkbox(T(TKEY("act3_sharpen"), "ACT3 Fullscreen RCAS"), &settings.act3Sharpen);
				if (settings.act3Sharpen)
					ImGui::SliderFloat(T(TKEY("act3_sharpness"), "ACT3 RCAS Sharpness"), &settings.act3Sharpness, 0.0f, 1.0f, "%.2f");
				if (act3Custom)
					settings.act3Preset = 0;
			}
		}
		if (!supportedRoute)
			ImGui::BeginDisabled();

		if (settings.neuralRenderingEnabled) {
			if (ImGui::TreeNodeEx(T(TKEY("diagnostics"), "Diagnostics"))) {
				if (globals::game::isVR)
					ImGui::TextDisabled("%s", T(TKEY("pipeline_arise_vr_blocked"),
						"ARISE owns DXGI Present. VR submits through OpenVR. 8-pass runs after Post, then we Submit."));
				if (!globals::game::isVR) {
					if (ImGui::Checkbox(T(TKEY("pipeline_arise"), "ARISE Async PerfMode"), &settings.ariseModel3) && settings.ariseModel3) {
						settings.singleThreadPerfMode = false;
						settings.ariseBufferCount = 3;
					}
					Util::UI::RestartGatedAnnotate(bootSnapshot, settings, &Settings::ariseModel3,
						T(TKEY("pipeline_arise_restart"),
							"ARISE: three-slot logical Present tail. Restart required; HDR is not supported."));
					if (ImGui::Checkbox(T(TKEY("pipeline_single_thread"), "Single-thread PerfMode"), &settings.singleThreadPerfMode) && settings.singleThreadPerfMode) {
						settings.ariseModel3 = false;
						settings.ariseBufferCount = 3;
					}
					Util::UI::RestartGatedAnnotate(bootSnapshot, settings, &Settings::singleThreadPerfMode,
						T(TKEY("pipeline_single_thread_restart"),
							"Same logical R proxy and six-pass graph on the Present calling thread."));
					if (upscaling.dx12SwapChain.useArise)
						ImGui::TextDisabled(T(TKEY("diag_active_execution"), "Active execution: %s"),
							upscaling.dx12SwapChain.IsAriseAsyncTail() ?
								T(TKEY("diag_tail_async"), "ARISE asynchronous Tail") :
								T(TKEY("diag_tail_sync"), "Single-thread Present Tail"));
					ImGui::BeginDisabled(!upscaling.dx12SwapChain.useArise);
					auto runtimeToggle = [](const char* label, std::atomic_bool& value) {
						bool enabled = value.load(std::memory_order_relaxed);
						if (!ImGui::Checkbox(label, &enabled))
							return false;
						value.store(enabled, std::memory_order_release);
						return true;
					};
					runtimeToggle(T(TKEY("diag_dimension"), "Model 3 Dimension Diagnostics"), ariseDimensionDiagnostics);
					runtimeToggle(T(TKEY("diag_menu_copy"), "Menu ISCopy Diagnostics"), menuCopyDiagnostics);
					runtimeToggle(T(TKEY("diag_disable_ui"), "Disable Model 3 UI Composite"), ariseDisableUiComposite);
					runtimeToggle(T(TKEY("diag_force_sync"), "Force Synchronous Model 3"), ariseForceSynchronous);
					const bool graphToggleChanged = runtimeToggle(T(TKEY("diag_bypass_graph"), "Bypass Model 3 Graph"), ariseBypassGraph);
					if (graphToggleChanged && upscaling.dx12SwapChain.useArise &&
						!ariseBypassGraph.load(std::memory_order_acquire))
						upscaling.dx12SwapChain.PrepareAriseGraphPlanChange();
					ImGui::EndDisabled();
				}
				if (!globals::game::isVR && ImGui::Checkbox(T(TKEY("diag_worker_nr"), "ARISE Feature 18 Worker"), &settings.ariseWorkerNr))
					NeuralRendering::ARISE::Pipeline::Instance().SetWorkerNrEnabled(settings.ariseWorkerNr);
				if (ImGui::Checkbox(T(TKEY("diag_shadow_scheduler"), "ARISE Shadow Scheduler"), &settings.ariseShadowScheduler))
					NeuralRendering::ARISE::Pipeline::Instance().SetEnabled(settings.ariseShadowScheduler);
				ImGui::BeginDisabled(settings.ariseShadowScheduler || settings.ariseWorkerNr || WantsLogicalPerfMode());
				const char* ariseBufferCounts[] = {
					T(TKEY("diag_slots_2"), "2 Slots"),
					T(TKEY("diag_slots_3"), "3 Slots")
				};
				int ariseBufferIndex = static_cast<int>(settings.ariseBufferCount - 2);
				if (ImGui::Combo(T(TKEY("diag_frame_slots"), "ARISE Frame Slots"), &ariseBufferIndex, ariseBufferCounts, IM_ARRAYSIZE(ariseBufferCounts))) {
					settings.ariseBufferCount = static_cast<uint>(ariseBufferIndex + 2);
					NeuralRendering::ARISE::Pipeline::Instance().SetSlotCount(settings.ariseBufferCount);
				}
				ImGui::EndDisabled();
				if (settings.ariseShadowScheduler &&
					ImGui::Checkbox(T(TKEY("diag_visual_validation"), "ARISE Visual Validation"), &settings.ariseVisualValidation))
					NeuralRendering::ARISE::Pipeline::Instance().SetVisualValidation(settings.ariseVisualValidation);
				const auto ariseStats = NeuralRendering::ARISE::Pipeline::Instance().GetStatistics();
				ImGui::TextDisabled(T(TKEY("diag_arise_stats"),
					"ARISE: Flat %llu | VR %llu | consumed %llu | saturated %llu (%.3f%%)"),
					static_cast<unsigned long long>(ariseStats.observedFlatPresents),
					static_cast<unsigned long long>(ariseStats.observedVrSubmits),
					static_cast<unsigned long long>(ariseStats.consumedSubmits),
					static_cast<unsigned long long>(ariseStats.saturatedSubmits), ariseStats.saturationRate);
				ImGui::TextDisabled(T(TKEY("diag_arise_queue"),
					"ARISE Queue: %u/%u | high-water %u | saturation streak %u (max %u)"),
					ariseStats.queuedSlots, ariseStats.slotCount, ariseStats.queueHighWater,
					ariseStats.currentSaturationStreak, ariseStats.maxSaturationStreak);
				ImGui::TextDisabled(T(TKEY("diag_arise_color"),
					"ARISE Color: D3D11 copies %llu | D3D12 submits %llu | completes %llu | visual %llu | failures %llu"),
					static_cast<unsigned long long>(ariseStats.colorCopies),
					static_cast<unsigned long long>(ariseStats.gpuSubmits),
					static_cast<unsigned long long>(ariseStats.gpuCompletes),
					static_cast<unsigned long long>(ariseStats.visualPresents),
					static_cast<unsigned long long>(ariseStats.interopFailures));
				ImGui::TextDisabled(T(TKEY("diag_arise_worker"),
					"ARISE Worker NR: queued %llu | succeeded %llu | fallback %llu | failed %llu"),
					static_cast<unsigned long long>(ariseStats.workerNrQueued),
					static_cast<unsigned long long>(ariseStats.workerNrSucceeded),
					static_cast<unsigned long long>(ariseStats.workerNrFallback),
					static_cast<unsigned long long>(ariseStats.workerNrFailed));
				const auto model3Stats = upscaling.dx12SwapChain.GetAriseStatistics();
					if (upscaling.dx12SwapChain.useArise)
						ImGui::TextDisabled(T(TKEY("diag_logical_r"), "Logical R execution: %s"),
							model3Stats.asyncTail ?
								T(TKEY("diag_tail_async"), "ARISE asynchronous Tail") :
								T(TKEY("diag_tail_sync"), "Single-thread Present Tail"));
				if (model3Stats.renderDocCompatibility)
					ImGui::TextDisabled("%s", T(TKEY("diag_renderdoc"),
						"RenderDoc compatibility: DLSS/NR replaced; graph topology preserved"));
				ImGui::TextDisabled(T(TKEY("diag_model3_extents"),
					"Model 3 extents: logical R %ux%u | physical D %ux%u"),
					upscaling.dx12SwapChain.LogicalWidth(), upscaling.dx12SwapChain.LogicalHeight(),
					upscaling.dx12SwapChain.PhysicalWidth(), upscaling.dx12SwapChain.PhysicalHeight());
				ImGui::TextDisabled(T(TKEY("diag_dx12_dlss"), "DX12 DLSS provider: %s"),
					model3Stats.dlssAvailable ?
						T(TKEY("diag_available"), "available") :
						T(TKEY("diag_unavailable"), "unavailable"));
				ImGui::TextDisabled(T(TKEY("diag_model3_path"), "ARISE Model 3: %s%s"), model3Stats.graphPath.c_str(),
					model3Stats.tailNrAvailable ? "" : T(TKEY("diag_passthrough"), " (runtime unavailable; passthrough)"));
				ImGui::TextDisabled(T(TKEY("diag_graph_passes"),
					"Graph passes: ACT1 bypass %llu | Lanczos %llu | NR %llu | Guidance %llu | RCAS %llu"),
					static_cast<unsigned long long>(model3Stats.graph.act1Bypass),
					static_cast<unsigned long long>(model3Stats.graph.lanczos),
					static_cast<unsigned long long>(model3Stats.graph.neuralRendering),
					static_cast<unsigned long long>(model3Stats.graph.fusion),
					static_cast<unsigned long long>(model3Stats.graph.rcas));
				ImGui::TextDisabled(T(TKEY("diag_dlss_provider"),
					"DLSS provider: ACT1 ok %llu / failed %llu | ACT2 ok %llu / failed %llu"),
					static_cast<unsigned long long>(model3Stats.graph.dlss.act1Succeeded),
					static_cast<unsigned long long>(model3Stats.graph.dlss.act1Failed),
					static_cast<unsigned long long>(model3Stats.graph.dlss.act2Succeeded),
					static_cast<unsigned long long>(model3Stats.graph.dlss.act2Failed));
				ImGui::TextDisabled(T(TKEY("diag_graph_recovery"),
					"Graph recovery: unsupported %llu | fallback %llu | failed %llu"),
					static_cast<unsigned long long>(model3Stats.graph.unsupported),
					static_cast<unsigned long long>(model3Stats.graph.fallback),
					static_cast<unsigned long long>(model3Stats.graph.failed));
				ImGui::TextDisabled(T(TKEY("diag_model3_frames"),
					"Model 3 frames: captured %llu | reconstructed %llu | fallback %llu | UI %llu"),
					static_cast<unsigned long long>(model3Stats.captured),
					static_cast<unsigned long long>(model3Stats.reconstructed),
					static_cast<unsigned long long>(model3Stats.fallback),
					static_cast<unsigned long long>(model3Stats.uiComposites));
				ImGui::TextDisabled(T(TKEY("diag_model3_present"),
					"Model 3 Present: logical %llu | physical %llu | backpressure %llu | failures %llu | last 0x%08X"),
					static_cast<unsigned long long>(model3Stats.logicalPresents),
					static_cast<unsigned long long>(model3Stats.physicalPresents),
					static_cast<unsigned long long>(model3Stats.backpressure),
					static_cast<unsigned long long>(model3Stats.failures),
					static_cast<std::uint32_t>(model3Stats.lastPresentResult));
				const auto stats = NeuralRendering::GetAct2BackendStats();
				ImGui::TextDisabled(T(TKEY("diag_actual_jitter"), "Actual jitter: %.4f, %.4f"), upscaling.jitter.x, upscaling.jitter.y);
				ImGui::TextDisabled(T(TKEY("diag_act_submits"), "ACT1 submits: %s | ACT2 submits: %s"),
					(globals::game::isVR ? upscaling.settings.flatAct1JitterMode : settings.flatPipelineAct1Jitter) == 0 ?
						T(TKEY("jitter_actual"), "Actual Camera Jitter") : T(TKEY("jitter_zero"), "Zero"),
					(globals::game::isVR ? upscaling.settings.flatAct2JitterMode : settings.flatPipelineAct2Jitter) == 0 ?
						T(TKEY("jitter_actual"), "Actual Camera Jitter") : T(TKEY("jitter_zero"), "Zero"));
				ImGui::TextDisabled(T(TKEY("diag_act1_ok"), "ACT1 DLSS successful frames: %llu"),
					static_cast<unsigned long long>(Upscaling::streamline.flatAct1SuccessfulFrames));
				ImGui::TextDisabled(T(TKEY("diag_act2_ok"), "ACT2 successful frames: NIS %llu | DLSS %llu | FSR3 %llu"),
					static_cast<unsigned long long>(stats.nisSucceeded),
					static_cast<unsigned long long>(stats.dlssSucceeded),
					static_cast<unsigned long long>(stats.fsrSucceeded));
				ImGui::Checkbox(T(TKEY("diag_bypass_nr"), "Bypass NR Output (SR Comparison)"), &settings.neuralRenderingBypassOutput);
				if (!globals::game::isVR) {
					ImGui::Checkbox(T(TKEY("diag_input_isolation"), "Flat DLSS Input Isolation"), &upscaling.settings.flatDlssInputIsolation);
					Util::UI::RestartGatedAnnotate(upscaling.bootSnapshot, upscaling.settings,
						&Upscaling::Settings::flatDlssInputIsolation,
						T(TKEY("diag_input_isolation_help"),
							"Copies the active viewport into exact RenderRes inputs. Restart required."));
					ImGui::Checkbox(T(TKEY("diag_reset_nr"), "Reset NR History Every Frame"), &settings.neuralRenderingResetEveryFrame);
					if (globals::state && globals::state->IsDeveloperMode()) {
						const char* inputPrefilters[] = {
							T(TKEY("diag_prefilter_off"), "Off"),
							T(TKEY("diag_prefilter_77"), "77% Roundtrip"),
							T(TKEY("diag_prefilter_66"), "66% Roundtrip"),
							T(TKEY("diag_prefilter_50"), "50% Roundtrip")
						};
						int inputPrefilter = static_cast<int>(settings.neuralRenderingInputPrefilter);
						if (ImGui::Combo(T(TKEY("diag_prefilter"), "Full NR Input Prefilter"), &inputPrefilter, inputPrefilters, IM_ARRAYSIZE(inputPrefilters)))
							settings.neuralRenderingInputPrefilter = static_cast<uint>(inputPrefilter);
						const char* motionVectorModes[] = {
							T(TKEY("diag_mv_dilated"), "Dilated"),
							T(TKEY("diag_mv_raw"), "Raw Engine MV"),
							T(TKEY("diag_mv_zero"), "Zero MV"),
							T(TKEY("diag_mv_inverted"), "Inverted MV"),
							T(TKEY("diag_mv_half"), "0.5x MV"),
							T(TKEY("diag_mv_double"), "2.0x MV")
						};
						int motionVectorMode = static_cast<int>(settings.neuralRenderingMotionVectorMode);
						if (ImGui::Combo(T(TKEY("diag_mv_test"), "NR Motion Vector Test"), &motionVectorMode, motionVectorModes, IM_ARRAYSIZE(motionVectorModes)))
							settings.neuralRenderingMotionVectorMode = static_cast<uint>(motionVectorMode);
					}
				}
				ImGui::TreePop();
			}

			auto& neuralRenderer = NeuralRendering::Renderer::Instance();
			if (neuralRenderer.IsFailureLatched()) {
				Util::Text::Warning(T(TKEY("neural_rendering_failed"),
					"DLSS Neural Rendering failed and is disabled for this session. Check CommunityShaders.log."));
				if (ImGui::Button(T(TKEY("neural_rendering_reset_failure"), "Reset Neural Rendering Failure")))
					neuralRenderer.Reset();
			}
			if (globals::state && globals::state->IsDeveloperMode()) {
				ImGui::TextDisabled(T(TKEY("diag_nr_status"), "Status: %s | NGX: 0x%08X | Evaluations: %llu"),
					neuralRenderer.StatusText(), neuralRenderer.NgxResult(),
					static_cast<unsigned long long>(neuralRenderer.SuccessfulFrames()));
			}
			static double endToEndGpuMs = 0.0;
			static std::uint32_t observedCapturedFrame = 0;
			static std::uint32_t endToEndCapturedFrame = 0;
			bool endToEndAvailable = endToEndCapturedFrame != 0;
			if (globals::profiler) {
				globals::profiler->RequestCapture();
				const std::uint32_t capturedFrame = globals::profiler->GetCapturedFrameCount();
				const std::string_view timerName = globals::game::isVR ?
					"NeuralRendering::EvaluateStereo" : "NeuralRendering::Evaluate";
				if (capturedFrame != observedCapturedFrame) {
					observedCapturedFrame = capturedFrame;
					endToEndCapturedFrame = 0;
					endToEndAvailable = false;
					for (const auto& result : globals::profiler->GetResults()) {
						if (result.name == timerName && result.activeGpu) {
							endToEndGpuMs = result.gpuTimeMs;
							endToEndCapturedFrame = capturedFrame;
							endToEndAvailable = true;
							break;
						}
					}
				}
			}
			const double dx12TotalGpuMs = neuralRenderer.LastD3D12TotalGpuMs();
			const double pureNgxGpuMs = neuralRenderer.LastEvaluationGpuMs();
			const double endToEndSampleGpuMs = endToEndGpuMs;
			const double dx12WrapperGpuMs = std::max(0.0, dx12TotalGpuMs - pureNgxGpuMs);
			const double interopResidualGpuMs = endToEndAvailable ? std::max(0.0, endToEndSampleGpuMs - dx12TotalGpuMs) : 0.0;
			const auto percent = [endToEndSampleGpuMs, endToEndAvailable](double value) {
				return endToEndAvailable && endToEndSampleGpuMs > 0.0 ? value * 100.0 / endToEndSampleGpuMs : 0.0;
			};
			if (endToEndAvailable)
				ImGui::TextDisabled("NR End-to-End GPU: %.3f ms | captured frame: %u", endToEndGpuMs, endToEndCapturedFrame);
			else
				ImGui::TextDisabled("NR End-to-End GPU: waiting for profiler capture");
			if (endToEndAvailable) {
				ImGui::TextDisabled("NR DX12 Total GPU: %.3f ms (%.1f%%) | samples: %llu",
					dx12TotalGpuMs, percent(dx12TotalGpuMs),
					static_cast<unsigned long long>(neuralRenderer.D3D12TotalTimingSamples()));
				ImGui::TextDisabled("NR Pure NGX GPU: %.3f ms (%.1f%%) | samples: %llu",
					pureNgxGpuMs, percent(pureNgxGpuMs),
					static_cast<unsigned long long>(neuralRenderer.EvaluationTimingSamples()));
				ImGui::TextDisabled("DX12 wrapper: %.3f ms (%.1f%%)", dx12WrapperGpuMs, percent(dx12WrapperGpuMs));
				ImGui::TextDisabled("Interop/copy/fence residual: %.3f ms (%.1f%%)",
					interopResidualGpuMs, percent(interopResidualGpuMs));
			} else {
				ImGui::TextDisabled("NR DX12 Total GPU: %.3f ms | share: N/A | samples: %llu",
					dx12TotalGpuMs, static_cast<unsigned long long>(neuralRenderer.D3D12TotalTimingSamples()));
				ImGui::TextDisabled("NR Pure NGX GPU: %.3f ms | share: N/A | samples: %llu",
					pureNgxGpuMs, static_cast<unsigned long long>(neuralRenderer.EvaluationTimingSamples()));
				ImGui::TextDisabled("DX12 wrapper: %.3f ms | share: N/A", dx12WrapperGpuMs);
				ImGui::TextDisabled("Interop/copy/fence residual: N/A");
			}
			ImGui::TextDisabled("Percentages compare latest asynchronous samples.");
			ImGui::TextDisabled("Interop CPU backpressure: %llu | last wait: %.1f ms",
				static_cast<unsigned long long>(neuralRenderer.CpuBackpressureCount()),
				neuralRenderer.LastCpuBackpressureMs());
		}
		if (!supportedRoute)
			ImGui::EndDisabled();
	}
}

void FoveatedRender::DrawSettings()
{
	ClampSettings();

	Util::Text::WrappedInfo("%s", T(TKEY("foveated_shared_panel_note"), "Quality and Sharpness are on the main Upscaling panel — changes there apply to foveated rendering too. DLSS Preset also applies there when DLSS is the selected upscaler."));

	// ── VR-only knobs ──
	if (globals::game::isVR) {
		ImGui::Separator();
		ImGui::Text("%s", T(TKEY("foveated_dlss_mode_header"), "VR DLSS Mode"));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("foveated_dlss_mode_tooltip"),
								  "Default — highest quality. Each eye gets its own isolated copy of color/depth/motion\n"
								  "vectors so DLSS can't sample across the stereo midline. 5 copies per eye per frame.\n"
								  "All DLSS presets supported. Best for screenshots or when Faster shows edge artifacts.\n"
								  "\n"
								  "Faster — lower overhead. DLSS reads directly from the frame buffer using a viewport\n"
								  "offset instead of isolating each eye. 1 snapshot + 2 mask clears per frame.\n"
								  "DLSS may sample 1-2 pixels from the neighboring eye near the stereo center — usually\n"
								  "invisible in motion. Presets J and K are incompatible and auto-clamp to L."));
		}

		const bool isFSR = globals::features::upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kFSR;
		if (isFSR)
			ImGui::BeginDisabled();
		uint prevMode = settings.dlssMode;
		ImGui::SliderInt(T(TKEY("foveated_dlss_mode_label"), "DLSS Mode"), reinterpret_cast<int*>(&settings.dlssMode), 0, 1, DlssModeName((DlssMode)std::min(settings.dlssMode, 1u)));
		if (settings.dlssMode != prevMode) {
			const uint prevPreset = globals::features::upscaling.settings.presetDLSS;
			ClampPresetToMode();
			if (globals::features::upscaling.settings.presetDLSS != prevPreset) {
				logger::info("[FOVEATED] DLSS preset clamped from {} to {} after mode switch (J/K incompatible with Faster)",
					prevPreset, globals::features::upscaling.settings.presetDLSS);
			}
		}
		if (isFSR) {
			ImGui::EndDisabled();
			ImGui::TextWrapped("%s", T(TKEY("foveated_dlss_mode_fsr_desc"), "Not used by FSR -- applies only when DLSS is the selected upscaler."));
		} else {
			switch (GetDlssMode()) {
			case DlssMode::kDefault:
				ImGui::TextWrapped("%s", T(TKEY("foveated_dlss_mode_default_desc"), "Per-eye isolation: 5 copies per frame, 2 DLSS evaluates. All presets."));
				break;
			case DlssMode::kFaster:
				ImGui::TextWrapped("%s", T(TKEY("foveated_dlss_mode_faster_desc"), "Viewport offset: 1 snapshot, 2 mask clears, 2 DLSS evaluates. Presets J/K unavailable."));
				break;
			default:
				break;
			}
		}

		ImGui::Separator();
		ImGui::Text("%s", T(TKEY("foveated_periphery_header"), "Periphery Rendering"));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("foveated_periphery_tooltip"),
								  "The area outside your selected subrect is filled cheaply rather than running\n"
								  "the selected upscaler. These settings control how that cheap fill looks and\n"
								  "whether it flickers.\n"
								  "\n"
								  "Stretch method: how pixels outside the subrect are reconstructed from the lower-res\n"
								  "render buffer. Does not affect the upscaled subrect region at all.\n"
								  "\n"
								  "Periphery AA: reduces temporal flicker in the stretched area using motion-compensated\n"
								  "history blending. Independent of the upscaled subrect.\n"
								  "\n"
								  "Edge Blend: controls how the upscaled subrect edge meets the stretched periphery.\n"
								  "Hard Copy leaves a sharp seam; Feather/Dither soften it. Edge Shape selects a\n"
								  "rectangle or oval composite."));
		}

		ImGui::SliderInt(T(TKEY("foveated_stretch_label"), "Stretch"), reinterpret_cast<int*>(&settings.stretchMode), 0, 2, StretchModeName((StretchMode)settings.stretchMode));
		switch (GetStretchMode()) {
		case StretchMode::kBilinear:
			ImGui::TextWrapped("%s", T(TKEY("foveated_stretch_bilinear_desc"), "Bilinear: smooth upscale of the render buffer. Looks soft but clean."));
			break;
		case StretchMode::kPoint:
			ImGui::TextWrapped("%s", T(TKEY("foveated_stretch_point_desc"), "Point: cheapest, visibly pixelated. Good for benchmarking foveated savings."));
			break;
		case StretchMode::kGaussianBlur:
			ImGui::TextWrapped("%s", T(TKEY("foveated_stretch_gaussian_desc"), "Gaussian: blurs the periphery further into soft focus. Good default for foveated use."));
			ImGui::SliderFloat(T(TKEY("foveated_blur_radius"), "Blur Radius"), &settings.peripheryBlurRadius, 0.5f, 4.0f, "%.1f px");
			break;
		}

		ImGui::SliderInt(T(TKEY("foveated_periphery_aa_label"), "Periphery AA"), reinterpret_cast<int*>(&settings.peripheryAAMode), 0, 1, PeripheryAAModeName((PeripheryAAMode)settings.peripheryAAMode));
		if (GetPeripheryAAMode() == PeripheryAAMode::kTemporalSmooth) {
			ImGui::TextWrapped("%s", T(TKEY("foveated_periphery_aa_temporal_desc"), "Blends the stretched periphery with motion-reprojected history to reduce flicker."));
			ImGui::SliderFloat(T(TKEY("foveated_smoothing"), "Smoothing"), &settings.peripheryTemporalAlpha, 0.05f, 0.5f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("foveated_smoothing_tooltip"), "Lower = more temporal history (smoother but may ghost). Higher = more responsive."));
			}
		}

		ImGui::SliderInt(T(TKEY("foveated_edge_blend_label"), "Edge Blend"), reinterpret_cast<int*>(&settings.subrectBlendMode), 0, 2, SubrectBlendModeName((SubrectBlendMode)std::min(settings.subrectBlendMode, 2u)));
		switch (GetSubrectBlendMode()) {
		case SubrectBlendMode::kHardCopy:
			ImGui::TextWrapped("%s", T(TKEY("foveated_blend_hard_copy_desc"), "Sharp seam at the subrect boundary. Lowest cost."));
			break;
		case SubrectBlendMode::kFeather:
			ImGui::TextWrapped("%s", T(TKEY("foveated_blend_feather_desc"), "Smoothstep fade over N pixels at the boundary. Hides the seam. Falloff Curve reshapes the ramp."));
			ImGui::SliderFloat(T(TKEY("foveated_feather_width"), "Feather Width"), &settings.subrectFeatherWidth, 2.0f, 128.0f, "%.0f px");
			break;
		case SubrectBlendMode::kDither:
			ImGui::TextWrapped("%s", T(TKEY("foveated_blend_dither_desc"), "Noise-dithered fade — more natural-looking than feather at large subrects."));
			ImGui::SliderFloat(T(TKEY("foveated_band_width"), "Band Width"), &settings.subrectFeatherWidth, 2.0f, 128.0f, "%.0f px");
			ImGui::SliderFloat(T(TKEY("foveated_noise_amount"), "Noise Amount"), &settings.subrectDitherStrength, 0.0f, 2.0f, "%.2f");
			break;
		}

		if (GetSubrectBlendMode() != SubrectBlendMode::kHardCopy) {
			ImGui::SliderFloat(T(TKEY("foveated_falloff_curve"), "Falloff Curve"), &settings.subrectFalloffCurve, 0.5f, 2.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("foveated_falloff_curve_tooltip"), "Controls how the transition distributes the fade. 1.00 is balanced; lower values carry the upscaled result farther into the band, higher values hold the periphery longer."));
			}

			ImGui::SliderInt(T(TKEY("foveated_mask_shape_label"), "Edge Shape"), reinterpret_cast<int*>(&settings.subrectMaskMode), 0, 1,
				SubrectMaskModeName(GetSubrectMaskMode()));
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("foveated_mask_shape_tooltip"), "Oval rounds the blended edge, so the subrect corners keep the stretched periphery instead of the upscaled image. The upscaler still processes the full rectangle either way."));
			}
		}

		ImGui::Separator();
		ImGui::Text("%s", T(TKEY("foveated_subrect_region_header"), "Subrect Region"));
		ImGui::TextWrapped("%s", T(TKEY("foveated_subrect_region_desc"),
									 "Drag in the preview below to select the region that gets full upscaling. "
									 "The rest is cheaply stretched — saves significant upscaling cost."));
		Util::Text::WrappedInfo("%s", T(TKEY("foveated_screenshot_subrect_note"), "Screenshot has its own subrect; align them only if you want pixel-matched captures."));

		bool debugBool = settings.debugVisualize != 0;
		if (ImGui::Checkbox(T(TKEY("foveated_visualize_regions"), "Visualize regions"), &debugBool))
			settings.debugVisualize = debugBool ? 1u : 0u;
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("foveated_visualize_regions_tooltip"),
								  "Diagnostic: tint the cheap-stretched periphery red so the upscaled\n"
								  "subrect (un-tinted) pops visually in-game. Lets you confirm at a glance where\n"
								  "the selected upscaler is actually running vs where the cheap stretch is filling.\n"
								  "No perf impact; runtime toggle, no restart needed. Also shows briefly whenever\n"
								  "you drag-resize the region below, even with this off."));
		}

		auto renderer = globals::game::renderer;
		ID3D11ShaderResourceView* previewSrv = nullptr;
		ID3D11Texture2D* previewTex = nullptr;
		if (auto* owned = NeuralRendering::GetVrOwnedComposite(&previewSrv)) {
			previewTex = owned;
		} else if (renderer) {
			// Real object is VR-sized (kVRTOTAL) here; the multi-runtime build's
			// compile-time array type stays kTOTAL-sized.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warray-bounds"
			auto& fb = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kVR_FRAMEBUFFER];
#pragma clang diagnostic pop
			previewSrv = Util::AsReal(fb.SRV);
			previewTex = Util::AsReal(fb.texture);
		}
		if (previewSrv && previewTex)
			subrectController.DrawEditor(previewSrv, previewTex, 0.5f, 0.0f, Util::Subrect::OpaquePreviewBlendCallback);
		else
			subrectController.DrawEditor(nullptr, nullptr, 0.5f);

		if (subrectController.IsDragging())
			lastDragTime = std::chrono::steady_clock::now();
	}
}

#undef I18N_KEY_PREFIX
