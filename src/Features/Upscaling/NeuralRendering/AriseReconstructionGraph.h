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
		};

		struct FusionParameters
		{
			float luma = 0.85f;
			float chroma = 0.65f;
			float edgeProtection = 0.25f;
			float residualLimit = 0.16f;
			std::uint32_t direction = 0;
			std::uint32_t denoise = 1;
			float denoiseStrength = 0.45f;
			float softness = 0.35f;
			float frequencyGuidance = 0.65f;
			float depthGuidance = 0.75f;
			std::uint32_t enhancedGuidance = 0;
			float colorGuidance = 0.70f;
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
			std::uint32_t act1Quality = 1;
			std::uint32_t act1Preset = 0;
			std::uint32_t act2Quality = 1;
			std::uint32_t act2Preset = 0;
			AriseDlssProvider::CameraConstants act1Camera{};
			AriseDlssProvider::CameraConstants act2Camera{};
			FusionParameters fusion{};
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