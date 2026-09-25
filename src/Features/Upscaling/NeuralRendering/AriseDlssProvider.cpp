#include "AriseDlssProvider.h"

#include "../Streamline.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <optional>

namespace NeuralRendering
{
	namespace
	{
		sl::DLSSPreset Preset(std::uint32_t value)
		{
			switch (value) {
			case 1: return sl::DLSSPreset::ePresetJ;
			case 2: return sl::DLSSPreset::ePresetK;
			case 3: return sl::DLSSPreset::ePresetL;
			case 4: return sl::DLSSPreset::ePresetM;
			default: return sl::DLSSPreset::eDefault;
			}
		}
	}

	bool AriseDlssProvider::Initialize(Streamline* streamline, ID3D12Device* device)
	{
		Shutdown();
		streamline_ = streamline;
		device_ = device;
		Reset();
		return IsAvailable();
	}

	void AriseDlssProvider::Shutdown()
	{
		Reset();
		streamline_ = nullptr;
		device_ = nullptr;
	}

	void AriseDlssProvider::Reset()
	{
		if (streamline_ && streamline_->featureDLSS && streamline_->slFreeResources) {
			const std::uint32_t viewports[]{ kAct1Viewport, kAct2Viewport, kAct1ViewportRight, kAct2ViewportRight };
			for (const std::uint32_t viewport : viewports)
				streamline_->slFreeResources(sl::kFeatureDLSS, sl::ViewportHandle(viewport));
		}
		for (std::size_t index = 0; index < 4; ++index) {
			forceReset_[index] = true;
			lastContract_[index] = 0;
			lastOutcome_[index] = 0;
		}
	}

	bool AriseDlssProvider::IsAvailable() const
	{
		return device_ && streamline_ && streamline_->initialized && streamline_->featureDLSS &&
			streamline_->slGetNewFrameToken && streamline_->slSetConstants && streamline_->slDLSSSetOptions &&
			streamline_->slSetTagForFrame && streamline_->slEvaluateFeature;
	}

	bool AriseDlssProvider::Record(ID3D12GraphicsCommandList* commandList, const Dispatch& dispatch)
	{
		const int slot = dispatch.viewport == kAct1Viewport ? 0 :
			dispatch.viewport == kAct2Viewport ? 1 :
			dispatch.viewport == kAct1ViewportRight ? 2 :
			dispatch.viewport == kAct2ViewportRight ? 3 : -1;
		if (slot < 0) {
			++act1Failed_;
			return false;
		}
		const bool act2 = slot == 1 || slot == 3;
		const std::size_t index = static_cast<std::size_t>(slot);
		auto& forceReset = forceReset_[index];
		auto& succeeded = act2 ? act2Succeeded_ : act1Succeeded_;
		auto& failed = act2 ? act2Failed_ : act1Failed_;
		std::uint64_t contract = dispatch.viewport;
		auto hashCombine = [&](std::uint64_t value) {
			contract ^= value + 0x9E3779B97F4A7C15ull + (contract << 6) + (contract >> 2);
		};
		for (const std::uint64_t value : std::array<std::uint64_t, 8>{ dispatch.inputWidth, dispatch.inputHeight,
			dispatch.outputWidth, dispatch.outputHeight, dispatch.quality, dispatch.preset,
			static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(dispatch.camera.value.mvecScale.x)),
			static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(dispatch.camera.value.mvecScale.y)) })
			hashCombine(value);
		const bool contractChanged = lastContract_[index] != contract;
		auto logContract = [&](bool ok, const char* stage, sl::Result result) {
			const int outcome = ok ? 1 : -1;
			if (!contractChanged && lastOutcome_[index] == outcome)
				return;
			if (ok)
				logger::info("[ARISE DLSS] success viewport={} input={}x{} output={}x{} mode={} jitter=({:.4f},{:.4f}) "
					"mvecScale=({:.4f},{:.4f}) reset={} stage={} result={}",
					dispatch.viewport, dispatch.inputWidth, dispatch.inputHeight,
					dispatch.outputWidth, dispatch.outputHeight,
					magic_enum::enum_name(Streamline::DLSSModeForQualityMode(dispatch.quality)),
					dispatch.camera.value.jitterOffset.x, dispatch.camera.value.jitterOffset.y,
					dispatch.camera.value.mvecScale.x, dispatch.camera.value.mvecScale.y,
					dispatch.reset || forceReset, stage, static_cast<int>(result));
			else
				logger::error("[ARISE DLSS] failure viewport={} input={}x{} output={}x{} mode={} jitter=({:.4f},{:.4f}) "
					"mvecScale=({:.4f},{:.4f}) reset={} stage={} result={}",
					dispatch.viewport, dispatch.inputWidth, dispatch.inputHeight,
					dispatch.outputWidth, dispatch.outputHeight,
					magic_enum::enum_name(Streamline::DLSSModeForQualityMode(dispatch.quality)),
					dispatch.camera.value.jitterOffset.x, dispatch.camera.value.jitterOffset.y,
					dispatch.camera.value.mvecScale.x, dispatch.camera.value.mvecScale.y,
					dispatch.reset || forceReset, stage, static_cast<int>(result));
			lastContract_[index] = contract;
			lastOutcome_[index] = outcome;
		};
		auto fail = [&](const char* stage, sl::Result result) {
			forceReset = true;
			++failed;
			logContract(false, stage, result);
			return false;
		};
		if (!IsAvailable() || !commandList || !dispatch.color || !dispatch.output || !dispatch.depth ||
			!dispatch.motionVectors || !dispatch.inputWidth || !dispatch.inputHeight ||
			!dispatch.outputWidth || !dispatch.outputHeight)
			return fail("validation", sl::Result::eErrorInvalidParameter);
		if (!std::isfinite(dispatch.camera.value.jitterOffset.x) ||
			!std::isfinite(dispatch.camera.value.jitterOffset.y) ||
			!std::isfinite(dispatch.camera.value.cameraPinholeOffset.x) ||
			!std::isfinite(dispatch.camera.value.cameraPinholeOffset.y)) {
			logger::error("[ARISE DLSS] non-finite camera constants viewport={} jitter=({}, {}) pinhole=({}, {})",
				dispatch.viewport, dispatch.camera.value.jitterOffset.x, dispatch.camera.value.jitterOffset.y,
				dispatch.camera.value.cameraPinholeOffset.x, dispatch.camera.value.cameraPinholeOffset.y);
			return fail("camera-constants", sl::Result::eErrorInvalidParameter);
		}
		const auto colorDesc = dispatch.color->GetDesc();
		const auto outputDesc = dispatch.output->GetDesc();
		const auto depthDesc = dispatch.depth->GetDesc();
		const auto motionDesc = dispatch.motionVectors->GetDesc();
		if (colorDesc.Width != dispatch.inputWidth || colorDesc.Height != dispatch.inputHeight ||
			outputDesc.Width != dispatch.outputWidth || outputDesc.Height != dispatch.outputHeight ||
			depthDesc.Width != dispatch.inputWidth || depthDesc.Height != dispatch.inputHeight ||
			motionDesc.Width != dispatch.inputWidth || motionDesc.Height != dispatch.inputHeight) {
			logger::error("[ARISE DLSS] resource contract mismatch viewport={} input={}x{} color={}x{} depth={}x{} motion={}x{} output={}x{} resource={}x{}",
				dispatch.viewport, dispatch.inputWidth, dispatch.inputHeight,
				colorDesc.Width, colorDesc.Height, depthDesc.Width, depthDesc.Height,
				motionDesc.Width, motionDesc.Height, dispatch.outputWidth, dispatch.outputHeight,
				outputDesc.Width, outputDesc.Height);
			return fail("resource-contract", sl::Result::eErrorInvalidParameter);
		}

		sl::FrameToken* frameToken = nullptr;
		const std::uint32_t frameId = static_cast<std::uint32_t>(dispatch.frameId);
		const sl::Result tokenResult = streamline_->slGetNewFrameToken(frameToken, &frameId);
		if (tokenResult != sl::Result::eOk || !frameToken)
			return fail("token", tokenResult);
		const sl::ViewportHandle viewport(dispatch.viewport);
		sl::Constants constants = dispatch.camera.value;
		constants.reset = (dispatch.reset || forceReset) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		const sl::Result constantsResult = streamline_->slSetConstants(constants, *frameToken, viewport);
		if (constantsResult != sl::Result::eOk)
			return fail("constants", constantsResult);

		sl::DLSSOptions options{};
		options.mode = Streamline::DLSSModeForQualityMode(dispatch.quality);
		options.outputWidth = dispatch.outputWidth;
		options.outputHeight = dispatch.outputHeight;
		options.colorBuffersHDR = sl::Boolean::eTrue;
		options.useAutoExposure = sl::Boolean::eTrue;
		options.preExposure = 1.0f;
		const auto preset = Preset(dispatch.preset);
		options.dlaaPreset = preset;
		options.ultraQualityPreset = preset;
		options.qualityPreset = preset;
		options.balancedPreset = preset;
		options.performancePreset = preset;
		options.ultraPerformancePreset = preset;
		const sl::Result optionsResult = streamline_->slDLSSSetOptions(viewport, options);
		if (optionsResult != sl::Result::eOk)
			return fail("options", optionsResult);

		sl::Extent inputExtent{ 0, 0, dispatch.inputWidth, dispatch.inputHeight };
		sl::Extent outputExtent{ 0, 0, dispatch.outputWidth, dispatch.outputHeight };
		sl::Resource color{ sl::ResourceType::eTex2d, dispatch.color, static_cast<std::uint32_t>(dispatch.colorState) };
		sl::Resource output{ sl::ResourceType::eTex2d, dispatch.output, static_cast<std::uint32_t>(dispatch.outputState) };
		sl::Resource depth{ sl::ResourceType::eTex2d, dispatch.depth, static_cast<std::uint32_t>(dispatch.depthState) };
		sl::Resource motion{ sl::ResourceType::eTex2d, dispatch.motionVectors, static_cast<std::uint32_t>(dispatch.motionVectorState) };
		sl::ResourceTag tags[]{
			{ &color, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &inputExtent },
			{ &output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &outputExtent },
			{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &inputExtent },
			{ &motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &inputExtent },
		};
		const sl::Result tagResult = streamline_->slSetTagForFrame(*frameToken, viewport, tags, _countof(tags), commandList);
		if (tagResult != sl::Result::eOk)
			return fail("tag", tagResult);
		const sl::BaseStructure* inputs[]{ &viewport };
		const sl::Result evaluateResult = streamline_->slEvaluateFeature(
			sl::kFeatureDLSS, *frameToken, inputs, _countof(inputs), commandList);
		const bool ok = evaluateResult == sl::Result::eOk;
		forceReset = !ok;
		if (ok) {
			++succeeded;
			logContract(true, "evaluate", evaluateResult);
		} else {
			++failed;
			logContract(false, "evaluate", evaluateResult);
		}
		return ok;
	}

	AriseDlssProvider::Statistics AriseDlssProvider::GetStatistics() const
	{
		return { act1Succeeded_.load(), act1Failed_.load(), act2Succeeded_.load(), act2Failed_.load() };
	}
}