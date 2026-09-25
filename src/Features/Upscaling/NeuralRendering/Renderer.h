#pragma once

#include "Runtime.h"

#include <array>
#include <cstdint>
#include <functional>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11ShaderResourceView;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace NeuralRendering
{
	class Renderer
	{
	public:
		struct StereoEyeInput
		{
			ID3D11Resource* depth = nullptr;
			ID3D11ShaderResourceView* depthSRV = nullptr;
			ID3D11Resource* motionVectors = nullptr;
			std::uint32_t sourceX = 0;
			std::uint32_t sourceY = 0;
			float motionVectorScaleX = 1.0f;
			float motionVectorScaleY = 1.0f;
		};

		static Renderer& Instance();
		~Renderer();

		Renderer(const Renderer&) = delete;
		Renderer& operator=(const Renderer&) = delete;

		bool Apply(ID3D11Device* device, ID3D11DeviceContext* context, std::uint32_t eyeIndex,
			ID3D11Resource* color, ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
			ID3D11Resource* motionVectors,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight,
			float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning,
			bool forceReset = false, bool deferWait = false);
		/** @brief Waits for an Apply submitted with deferWait and copies that eye's output. */
		bool FinishApply(ID3D11DeviceContext* context, std::uint32_t eyeIndex, ID3D11Resource* output);
		bool ApplyToOutput(ID3D11Device* device, ID3D11DeviceContext* context, std::uint32_t eyeIndex,
			ID3D11Resource* color, ID3D11Resource* output,
			ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV, ID3D11Resource* motionVectors,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight,
			std::uint32_t outputWidth, std::uint32_t outputHeight,
			float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning,
			bool forceReset = false);
		bool ApplyStereo(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Resource* color,
			const std::array<StereoEyeInput, 2>& eyes,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight, const Tuning& tuning);
		/** @brief Binds the process-wide Feature 18 runtime to the Model 3 physical D3D12 device. */
		bool InitializeModel3(ID3D12Device* externalDevice, std::function<bool()> drainOwner);
		/** @brief Records a native-resolution Feature 18 evaluation on the caller's command list. */
		bool ExecuteModel3(ID3D12GraphicsCommandList* commandList, ID3D12Resource* color,
			ID3D12Resource* depth, ID3D12Resource* motionVectors, ID3D12Resource* output,
			std::uint32_t width, std::uint32_t height,
			float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning, bool reset,
			std::uint32_t slot = 0);
		/** @brief Shuts down Model 3 after its owner has drained all submitted GPU work. */
		void ResetModel3();
		void Reset();
		void ResetHistory();

		[[nodiscard]] bool IsFailureLatched() const;
		[[nodiscard]] double LastEvaluationGpuMs() const;
		[[nodiscard]] double LastD3D12TotalGpuMs() const;
		[[nodiscard]] std::uint64_t D3D12TotalTimingSamples() const;
		[[nodiscard]] std::uint64_t EvaluationTimingSamples() const;
		[[nodiscard]] std::uint64_t CpuBackpressureCount() const;
		[[nodiscard]] double LastCpuBackpressureMs() const;
		[[nodiscard]] std::uint32_t NgxResult() const;
		[[nodiscard]] std::uint64_t SuccessfulFrames() const;
		[[nodiscard]] const char* StatusText() const;

	private:
		Renderer();
		class State;
		State* state_ = nullptr;
	};
}