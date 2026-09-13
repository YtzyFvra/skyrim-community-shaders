#include "DLSSBooster.h"

#include "Globals.h"
#include "Menu.h"
#include "Menu/Fonts.h"
#include "Menu/ThemeManager.h"
#include "Upscaling.h"
#include "Upscaling/NeuralRendering/ArisePipeline.h"
#include "Upscaling/NeuralRendering/Integration.h"
#include "Utils/UI.h"

#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <nlohmann/json.hpp>

#define I18N_KEY_PREFIX "feature.dlss_booster."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	DLSSBooster::Settings,
	overrideEnabled,
	reconstructionPreset,
	reconstruction,
	frameGenerationMode,
	frameGenerationForceEnable,
	frameGenerationAllowInMenus,
	preferFSRFrameGen,
	dlssgFramesToGenerate,
	frameLimitMode,
	reflexLowLatencyMode,
	reflexLowLatencyBoost,
	reflexUseMarkersToOptimize,
	reflexUseFPSLimit,
	reflexFPSLimit);

namespace
{
	void CopyReconstruction(const FoveatedRender::Settings& from, FoveatedRender::Settings& to)
	{
		const uint enabled = to.enabled;
		const uint dlssMode = to.dlssMode;
		const uint stretchMode = to.stretchMode;
		const float peripheryBlurRadius = to.peripheryBlurRadius;
		const uint debugVisualize = to.debugVisualize;
		const uint peripheryAAMode = to.peripheryAAMode;
		const float peripheryTemporalAlpha = to.peripheryTemporalAlpha;
		const uint subrectBlendMode = to.subrectBlendMode;
		const float subrectFeatherWidth = to.subrectFeatherWidth;
		const float subrectDitherStrength = to.subrectDitherStrength;
		to = from;
		to.enabled = enabled;
		to.dlssMode = dlssMode;
		to.stretchMode = stretchMode;
		to.peripheryBlurRadius = peripheryBlurRadius;
		to.debugVisualize = debugVisualize;
		to.peripheryAAMode = peripheryAAMode;
		to.peripheryTemporalAlpha = peripheryTemporalAlpha;
		to.subrectBlendMode = subrectBlendMode;
		to.subrectFeatherWidth = subrectFeatherWidth;
		to.subrectDitherStrength = subrectDitherStrength;
	}

	void DrawReconstructionExtents(Upscaling& upscaling, FoveatedRender& foveated)
	{
		Util::DrawSectionHeader(T(TKEY("pipeline"), "Reconstruction Pipeline"), false, false);
		if (!globals::game::isVR) {
			foveated.DrawLogicalPerfModeControls();
			if (upscaling.dx12SwapChain.useArise || foveated.settings.neuralRenderingEnabled) {
				const auto preview = foveated.BuildFlatPipelinePlan(
					upscaling.dx12SwapChain.useArise ? upscaling.dx12SwapChain.PhysicalWidth() :
						(globals::state ? static_cast<uint32_t>(globals::state->screenSize.x) : 0u),
					upscaling.dx12SwapChain.useArise ? upscaling.dx12SwapChain.PhysicalHeight() :
						(globals::state ? static_cast<uint32_t>(globals::state->screenSize.y) : 0u),
					upscaling.dx12SwapChain.useArise ? upscaling.dx12SwapChain.LogicalWidth() : 0u,
					upscaling.dx12SwapChain.useArise ? upscaling.dx12SwapChain.LogicalHeight() : 0u);
				ImGui::TextDisabled(T(TKEY("extents"),
					"R %ux%u  ->  G %ux%u  ->  N %ux%u  ->  D %ux%u"),
					preview.renderR.width, preview.renderR.height,
					preview.guidanceG.width, preview.guidanceG.height,
					preview.neuralN.width, preview.neuralN.height,
					preview.displayD.width, preview.displayD.height);
			}
			return;
		}

		ImGui::TextDisabled("%s", T(TKEY("pipeline_vr"),
			"VR: per-eye 8-pass on kTEMP, write the box back to engine SBS. No FG. ARISE stays Flat-only."));
		if (!foveated.settings.neuralRenderingEnabled)
			return;
		const auto& previewUV = foveated.subrectController.GetUV();
		uint32_t vrRenderW = 0, vrRenderH = 0, vrDisplayW = 0, vrDisplayH = 0;
		NeuralRendering::GetVrOwnedEyeSizes(vrRenderW, vrRenderH, vrDisplayW, vrDisplayH);
		const uint32_t inW = std::max(1u, static_cast<uint32_t>(vrRenderW * previewUV.w));
		const uint32_t inH = std::max(1u, static_cast<uint32_t>(vrRenderH * previewUV.h));
		const uint32_t outW = std::max(1u, static_cast<uint32_t>(vrDisplayW * previewUV.w));
		const uint32_t outH = std::max(1u, static_cast<uint32_t>(vrDisplayH * previewUV.h));
		const auto preview = foveated.BuildFlatPipelinePlan(outW, outH, inW, inH);
		ImGui::TextDisabled(T(TKEY("extents"),
			"R %ux%u  ->  G %ux%u  ->  N %ux%u  ->  D %ux%u"),
			preview.renderR.width, preview.renderR.height,
			preview.guidanceG.width, preview.guidanceG.height,
			preview.neuralN.width, preview.neuralN.height,
			preview.displayD.width, preview.displayD.height);
	}

	void CopyFrameGenReflex(const Upscaling::Settings& from, DLSSBooster::Settings& to)
	{
		to.frameGenerationMode = from.frameGenerationMode;
		to.frameGenerationForceEnable = from.frameGenerationForceEnable;
		to.frameGenerationAllowInMenus = from.frameGenerationAllowInMenus;
		to.preferFSRFrameGen = from.preferFSRFrameGen;
		to.dlssgFramesToGenerate = from.dlssgFramesToGenerate;
		to.frameLimitMode = from.frameLimitMode;
		to.reflexLowLatencyMode = from.reflexLowLatencyMode;
		to.reflexLowLatencyBoost = from.reflexLowLatencyBoost;
		to.reflexUseMarkersToOptimize = from.reflexUseMarkersToOptimize;
		to.reflexUseFPSLimit = from.reflexUseFPSLimit;
		to.reflexFPSLimit = from.reflexFPSLimit;
	}

	void ApplyFrameGenReflex(const DLSSBooster::Settings& from, Upscaling::Settings& to)
	{
		to.frameGenerationMode = from.frameGenerationMode;
		to.frameGenerationForceEnable = from.frameGenerationForceEnable;
		to.frameGenerationAllowInMenus = from.frameGenerationAllowInMenus;
		to.preferFSRFrameGen = from.preferFSRFrameGen;
		to.dlssgFramesToGenerate = from.dlssgFramesToGenerate;
		to.frameLimitMode = from.frameLimitMode;
		to.reflexLowLatencyMode = from.reflexLowLatencyMode;
		to.reflexLowLatencyBoost = from.reflexLowLatencyBoost;
		to.reflexUseMarkersToOptimize = from.reflexUseMarkersToOptimize;
		to.reflexUseFPSLimit = from.reflexUseFPSLimit;
		to.reflexFPSLimit = from.reflexFPSLimit;
	}

	void ApplyAriseRuntime(const FoveatedRender::Settings& reconstruction)
	{
		auto& pipeline = NeuralRendering::ARISE::Pipeline::Instance();
		pipeline.SetEnabled(false);
		pipeline.SetWorkerNrEnabled(false);
		pipeline.SetSlotCount(reconstruction.ariseBufferCount);
		pipeline.SetVisualValidation(reconstruction.ariseVisualValidation);
		pipeline.SetWorkerNrEnabled(reconstruction.ariseWorkerNr);
		pipeline.SetEnabled(reconstruction.ariseShadowScheduler);
	}

	bool Near(float a, float b)
	{
		return std::fabs(a - b) <= 0.005f;
	}

	void ApplyFusionDetail(FoveatedRender::Settings& s)
	{
		s.neuralRenderingFusionPreset = 4;
		s.flatPipelineFusionDirection = 1;
		s.flatPipelineFusionDenoise = 1;
		s.flatPipelineFusionDenoiseStrength = 0.40f;
		s.flatPipelineFusionSoftness = 0.20f;
		s.flatPipelineFusionEnhancedGuidance = false;
		s.flatPipelineFusionFrequencyGuidance = 0.45f;
		s.flatPipelineFusionDepthGuidance = 0.65f;
		s.flatPipelineFusionColorGuidance = 0.70f;
		s.neuralRenderingFusionLuma = 0.80f;
		s.neuralRenderingFusionChroma = 0.45f;
		s.neuralRenderingFusionEdgeProtection = 0.30f;
		s.neuralRenderingFusionResidualLimit = 0.14f;
	}

	void ApplyConservativePreset(FoveatedRender::Settings& s)
	{
		s = {};
		s.unifiedPipeline.act1Srss = true;
		s.unifiedPipeline.act1SrssQuality = 4;
		s.unifiedPipeline.act1SrssPreset = 4;
		s.unifiedPipeline.guidanceScale = 1.5f;
		s.flatPipelineNrScale = 0.62f;
		s.unifiedPipeline.fusionEnabled = true;
		ApplyFusionDetail(s);
	}

	void Apply5090Preset(FoveatedRender::Settings& s)
	{
		s = {};
		s.unifiedPipeline.act1Srss = true;
		s.unifiedPipeline.act1SrssQuality = 4;
		s.unifiedPipeline.act1SrssPreset = 4;
		s.unifiedPipeline.guidanceScale = 2.0f;
		s.flatPipelineNrScale = 0.95f;
		s.unifiedPipeline.fusionEnabled = true;
		s.unifiedPipeline.fusionReconstruct = static_cast<uint>(FoveatedRender::FusionReconstruct::kMitchell);
	}

	uint ClassifyReconstructionPreset(const FoveatedRender::Settings& s)
	{
		const bool srss = s.unifiedPipeline.act1Srss &&
			s.unifiedPipeline.act1SrssQuality == 4 &&
			s.unifiedPipeline.act1SrssPreset == 4;
		if (srss && !s.flatPipelineNrNative && Near(s.unifiedPipeline.guidanceScale, 1.5f) &&
			Near(s.flatPipelineNrScale, 0.62f) &&
			!s.flatPipelineFusionEnhancedGuidance &&
			s.neuralRenderingFusionPreset == 4)
			return 0;
		if (srss && !s.flatPipelineNrNative && Near(s.unifiedPipeline.guidanceScale, 2.0f) &&
			Near(s.flatPipelineNrScale, 0.95f))
			return 1;
		return 2;
	}
}

void DLSSBooster::ImportFromRuntime()
{
	auto& upscaling = globals::features::upscaling;
	CopyReconstruction(upscaling.foveatedRender.settings, settings.reconstruction);
	CopyFrameGenReflex(upscaling.settings, settings);
}

void DLSSBooster::PullFromRuntime()
{
	ImportFromRuntime();
}

void DLSSBooster::PushToRuntime() const
{
	auto& upscaling = globals::features::upscaling;
	if (!upscaling.loaded)
		return;
	CopyReconstruction(settings.reconstruction, upscaling.foveatedRender.settings);
	ApplyFrameGenReflex(settings, upscaling.settings);
	upscaling.foveatedRender.ClampSettings();
	ApplyAriseRuntime(upscaling.foveatedRender.settings);
}

void DLSSBooster::LoadSettings(json& o_json)
{
	if (o_json.is_structured() && !o_json.empty())
		settings = o_json;
	else
		ImportFromRuntime();
	if (settings.overrideEnabled)
		PushToRuntime();
	settings.reconstructionPreset = ClassifyReconstructionPreset(settings.reconstruction);
	bootSnapshot.LatchIfNeeded(settings);
}

void DLSSBooster::SaveSettings(json& o_json)
{
	if (settings.overrideEnabled)
		PullFromRuntime();
	o_json = settings;
}

void DLSSBooster::RestoreDefaultSettings()
{
	const bool wasOverride = settings.overrideEnabled;
	settings = {};
	ApplyConservativePreset(settings.reconstruction);
	settings.reconstructionPreset = 0;
	if (wasOverride)
		PushToRuntime();
}

void DLSSBooster::Prepass()
{
	if (loaded && settings.overrideEnabled)
		PushToRuntime();
}

void DLSSBooster::DrawIntro()
{
	auto& upscaling = globals::features::upscaling;
	const auto& theme = globals::menu->GetTheme();
	ImVec4 fill = theme.StatusPalette.InfoColor;
	fill.w = 0.14f;

	ImGui::PushStyleColor(ImGuiCol_ChildBg, fill);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(
		ThemeManager::Constants::BUTTON_PADDING, ThemeManager::Constants::BUTTON_SPACING));
	ImGui::BeginChild("##dlssbooster.intro", ImVec2(0.0f, 0.0f),
		ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);

	{
		MenuFonts::FontRoleGuard heading(Menu::FontRole::Heading);
		ImGui::TextColored(Util::Colors::GetInfo(), "%s",
			T(TKEY("badge"), "Designed for NVIDIA RTX"));
	}

	ImGui::TextWrapped("%s", T(TKEY("depends_on_upscaling"),
		"Depends on Upscaling. This tab is a debug host for the reconstruction stack."));

	ImGui::Checkbox(T(TKEY("override_enable"), "Apply this page (override Upscaling)"),
		&settings.overrideEnabled);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T(TKEY("override_enable_tooltip"),
			"Pushes this page every frame and turns off Upscaling's own DLSS Evaluate. Overlapping Upscaling knobs still draw, but they do not stick."));
	Util::UI::RestartGatedAnnotate(bootSnapshot, settings, &Settings::overrideEnabled,
		T(TKEY("override_restart"),
			"May latch a different Present path. Restart to apply."));
	if (!settings.overrideEnabled)
		ImGui::TextDisabled("%s", T(TKEY("override_off_hint"),
			"Override off: preview only."));

	const bool nvidiaReady = upscaling.streamline.featureDLSS || upscaling.dx12SwapChain.useArise;
	if (!nvidiaReady)
		Util::Text::WrappedWarning("%s", T(TKEY("no_rtx"),
			"DLSS is not available on this adapter. Reconstruction options stay inert until an RTX GPU is present."));

	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
}

void DLSSBooster::DrawReconstructionPreset()
{
	settings.reconstructionPreset = ClassifyReconstructionPreset(settings.reconstruction);
	const char* presets[] = {
		T(TKEY("preset_conservative"), "Conservative"),
		T(TKEY("preset_5090"), "RTX 5090"),
		T(TKEY("preset_custom"), "Custom")
	};
	int preset = static_cast<int>(settings.reconstructionPreset);
	if (ImGui::Combo(T(TKEY("preset"), "Preset"), &preset, presets, IM_ARRAYSIZE(presets))) {
		if (preset == 0)
			ApplyConservativePreset(settings.reconstruction);
		else if (preset == 1)
			Apply5090Preset(settings.reconstruction);
		settings.reconstructionPreset = static_cast<uint>(preset);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T(TKEY("preset_tooltip"),
			"Conservative is the default. RTX 5090 is a high-end reference — raise settings toward it."));
}

void DLSSBooster::DrawSettings()
{
	auto& upscaling = globals::features::upscaling;
	auto& foveated = upscaling.foveatedRender;
	if (!upscaling.loaded) {
		Util::Text::WrappedWarning("%s", T(TKEY("needs_upscaling"),
			"Load Upscaling first. This page only hosts settings; Upscaling still owns the runtime."));
		return;
	}

	DrawReconstructionPreset();
	DrawIntro();
	ImGui::Spacing();
	{
		bool menuLog = foveated.menuCopyDiagnostics.load(std::memory_order_relaxed);
		if (ImGui::Checkbox(T(TKEY("diag_menu_copy"), "Menu ISCopy Diagnostics"), &menuLog))
			foveated.menuCopyDiagnostics.store(menuLog, std::memory_order_release);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(T(TKEY("diag_menu_copy_tooltip"),
				"Writes [MENUCOPY] lines to CommunityShaders.log for the main-menu ISCopy path."));
	}
	ImGui::Spacing();

	if (!settings.overrideEnabled)
		ImGui::BeginDisabled();

	FoveatedRender::Settings reconstructionBackup = foveated.settings;
	CopyReconstruction(settings.reconstruction, foveated.settings);

	DrawReconstructionExtents(upscaling, foveated);

	if (ImGui::CollapsingHeader(T(TKEY("six_pass"), "Six-Pass Reconstruction"), ImGuiTreeNodeFlags_DefaultOpen))
		foveated.DrawNeuralRenderingSettings(false);

	Upscaling::Settings frameGenBackup = upscaling.settings;
	ApplyFrameGenReflex(settings, upscaling.settings);
	upscaling.DrawFrameGenerationSettings();
	upscaling.DrawReflexSettings();
	CopyFrameGenReflex(upscaling.settings, settings);
	if (!settings.overrideEnabled) {
		DLSSBooster::Settings restore{};
		CopyFrameGenReflex(frameGenBackup, restore);
		ApplyFrameGenReflex(restore, upscaling.settings);
	}

	CopyReconstruction(foveated.settings, settings.reconstruction);
	settings.reconstructionPreset = ClassifyReconstructionPreset(settings.reconstruction);
	if (settings.overrideEnabled) {
		ApplyAriseRuntime(foveated.settings);
	} else {
		CopyReconstruction(reconstructionBackup, foveated.settings);
	}

	if (!settings.overrideEnabled)
		ImGui::EndDisabled();
}

#undef I18N_KEY_PREFIX
