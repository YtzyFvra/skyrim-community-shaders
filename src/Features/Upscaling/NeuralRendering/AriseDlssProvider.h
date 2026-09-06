#pragma once

#include <d3d12.h>

#include <atomic>
#include <cstdint>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>

class Streamline;

namespace NeuralRendering
{
	class AriseDlssProvider
	{
	public:
		struct CameraConstants
		{
			sl::Constants value{};
		};

		struct Dispatch
		{
			std::uint64_t frameId = 0;
			std::uint32_t viewport = 0;
			std::uint32_t inputWidth = 0;
			std::uint32_t inputHeight = 0;
			std::uint32_t outputWidth = 0;
			std::uint32_t outputHeight = 0;
			std::uint32_t quality = 1;
			std::uint32_t preset = 0;
			bool reset = false;
			CameraConstants camera{};
			ID3D12Resource* color = nullptr;
			ID3D12Resource* output = nullptr;
			ID3D12Resource* depth = nullptr;
			ID3D12Resource* motionVectors = nullptr;
			D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_COMMON;
			D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_COMMON;
			D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON;
			D3D12_RESOURCE_STATES motionVectorState = D3D12_RESOURCE_STATE_COMMON;
		};

		struct Statistics
		{
			std::uint64_t act1Succeeded = 0;
			std::uint64_t act1Failed = 0;
			std::uint64_t act2Succeeded = 0;
			std::uint64_t act2Failed = 0;
		};

		static constexpr std::uint32_t kAct1Viewport = 10;
		static constexpr std::uint32_t kAct2Viewport = 11;

		/** @brief Binds the provider to the DX12 Streamline instance and physical device. */
		bool Initialize(Streamline* streamline, ID3D12Device* device);
		/** @brief Frees both viewport allocations after the owning tail has drained. */
		void Shutdown();
		/** @brief Invalidates both temporal histories after the owning tail has drained. */
		void Reset();
		[[nodiscard]] bool IsAvailable() const;
		/** @brief Records one DLSS evaluation from an immutable main-thread snapshot. */
		bool Record(ID3D12GraphicsCommandList* commandList, const Dispatch& dispatch);
		[[nodiscard]] Statistics GetStatistics() const;

	private:
		Streamline* streamline_ = nullptr;
		ID3D12Device* device_ = nullptr;
		bool forceReset_[2]{ true, true };
		std::uint64_t lastContract_[2]{};
		int lastOutcome_[2]{};
		std::atomic_uint64_t act1Succeeded_ = 0;
		std::atomic_uint64_t act1Failed_ = 0;
		std::atomic_uint64_t act2Succeeded_ = 0;
		std::atomic_uint64_t act2Failed_ = 0;
	};
}