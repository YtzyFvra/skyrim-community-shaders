#pragma once

#include "AriseDlssProvider.h"
#include "Runtime.h"

#include <d3d12.h>
#include <dxgiformat.h>
#include <winrt/base.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace NeuralRendering
{
	class AriseReconstructionGraph
	{
	public:
		enum class Act1Backend : std::uint32_t
		{
			kBypass,
			kDLSS,
			kFSR3,
		};

		enum class Act2SrBackend : std::uint32_t
		{
			kDLSS,
			kFSR3,
			kNIS,
			kLanczos,
			kMitchell,
			kEasu,
			kTent,
		};

		struct FusionParameters
		{
			float detailLuma = 1.0f;
			float detailChroma = 0.7f;
			float rdEdgeFloor = 0.3f;
			float regularization = 0.14f;
			std::uint32_t compareMode = 0;
			std::uint32_t debugView = 0;
			float gainPrior = 1.0f;
			float fitWindow = 0.2f;
			float maxGain = 0.7f;
			float depthGuidance = 0.65f;
			float depthDilate = 1.0f;
			float detailGain = 1.0f;
			float toneGainLuma = 1.0f;
			float toneGainChroma = 1.0f;
			float ditherStrength = 1.0f;
			std::uint32_t edgeBlockMode = 0;  ///< Unused; kept for CB layout.
			float edgeBlockGThresh = 0.15f;
			float edgeBlockSkip = 0.20f;
			float edgeBlockDilateD = 0.0f;
			float edgeBlockMix = 0.0f;
		};

		struct Act3Parameters
		{
			bool enabled = false;
			float scale = 0.50f;
			std::uint32_t cropX = 0;
			std::uint32_t cropY = 0;
			std::uint32_t cropWidth = 0;
			std::uint32_t cropHeight = 0;
			Tuning tuning{};
			bool sharpen = false;
			float sharpness = 1.0f;
			bool resetEveryFrame = false;
			std::uint32_t blendMode = 1;
			float featherWidth = 64.0f;
			float ditherStrength = 1.0f;
			bool guidanceEnabled = false;
			float guidanceLuma = 1.0f;
			float guidanceChroma = 0.70f;
			float guidancePrior = 1.0f;
			float guidanceMaxGain = 0.70f;
			float guidanceToneLuma = 1.0f;
			float guidanceToneChroma = 1.0f;
			float guidanceDetailGain = 1.0f;
			float guidanceDither = 1.0f;
			float guidanceDirSharpen = 0.0f;
		};

		struct Plan
		{
			std::uint64_t frameId = 0;
			std::uint32_t sceneInputWidth = 0;
			std::uint32_t sceneInputHeight = 0;
			std::uint32_t engineWidth = 0;
			std::uint32_t engineHeight = 0;
			std::uint32_t displayWidth = 0;
			std::uint32_t displayHeight = 0;
			std::uint32_t act1OutputWidth = 0;
			std::uint32_t act1OutputHeight = 0;
			std::uint32_t nrWidth = 0;
			std::uint32_t nrHeight = 0;
			Act1Backend act1 = Act1Backend::kBypass;
			Act2SrBackend act2Sr = Act2SrBackend::kLanczos;
			bool fusionEnabled = false;
			std::uint32_t act1Downsample = 0;
			std::uint32_t act1DepthDownsample = 0;
			std::uint32_t act1Quality = 1;
			std::uint32_t act1Preset = 0;
			std::uint32_t act2Quality = 1;
			std::uint32_t act2Preset = 0;
			std::uint32_t act1Viewport = AriseDlssProvider::kAct1Viewport;
			std::uint32_t act2Viewport = AriseDlssProvider::kAct2Viewport;
			std::uint32_t nrSlot = 0;
			AriseDlssProvider::CameraConstants act1Camera{};
			AriseDlssProvider::CameraConstants act2Camera{};
			FusionParameters fusion{};
			Act3Parameters act3{};
			Tuning tuning{};
			float motionVectorScaleX = 1.0f;
			float motionVectorScaleY = 1.0f;
			float sharpness = 0.2f;
			bool sharpen = false;
			bool requested = false;
			bool eligible = false;
			bool renderDocReplacement = false;
			bool reset = false;
		};

		struct Inputs
		{
			ID3D12Resource* scene = nullptr;
			ID3D12Resource* depth = nullptr;
			ID3D12Resource* motionVectors = nullptr;
		};

		struct Result
		{
			ID3D12Resource* finalResource = nullptr;
			D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
			bool reconstructed = false;
		};

		struct Statistics
		{
			std::uint64_t act1Bypass = 0;
			AriseDlssProvider::Statistics dlss{};
			std::uint64_t lanczos = 0;
			std::uint64_t neuralRendering = 0;
			std::uint64_t fusion = 0;
			std::uint64_t rcas = 0;
			std::uint64_t unsupported = 0;
			std::uint64_t fallback = 0;
			std::uint64_t failed = 0;
		};

		AriseReconstructionGraph();
		~AriseReconstructionGraph();
		AriseReconstructionGraph(const AriseReconstructionGraph&) = delete;
		AriseReconstructionGraph& operator=(const AriseReconstructionGraph&) = delete;

		/** @brief Creates immutable pipelines and per-slot descriptor ownership. */
		bool Initialize(ID3D12Device* device, DXGI_FORMAT displayFormat, std::size_t slotCount,
			Streamline* streamline);
		/** @brief Releases all graph-owned resources after the owner drains GPU work. */
		void Shutdown();
		/** @brief Invalidates temporal history and lazily rebuilt slot resources. */
		void Reset();
		/** @brief Ensures one slot has resources matching the immutable plan and inputs. */
		bool EnsureSlotResources(std::size_t slotIndex, const Inputs& inputs, const Plan& plan);
		/** @brief Records the complete reconstruction tail and returns a COMMON-state color. */
		Result Record(ID3D12GraphicsCommandList* commandList, std::size_t slotIndex,
			const Inputs& inputs, const Plan& plan);
		[[nodiscard]] Statistics GetStatistics() const;
		[[nodiscard]] bool IsDlssAvailable() const;
		[[nodiscard]] const char* ActivePath() const;

	private:
		class State;
		std::unique_ptr<State> state_;
	};
}