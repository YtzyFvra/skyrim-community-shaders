#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace NeuralRendering::ARISE
{
	struct Statistics
	{
		std::uint64_t observedSubmits = 0;
		std::uint64_t observedFlatPresents = 0;
		std::uint64_t observedVrSubmits = 0;
		std::uint64_t consumedSubmits = 0;
		std::uint64_t saturatedSubmits = 0;
		std::uint64_t colorCopies = 0;
		std::uint64_t gpuSubmits = 0;
		std::uint64_t gpuCompletes = 0;
		std::uint64_t visualPresents = 0;
		std::uint64_t interopFailures = 0;
		std::uint64_t workerNrQueued = 0;
		std::uint64_t workerNrSucceeded = 0;
		std::uint64_t workerNrFallback = 0;
		std::uint64_t workerNrFailed = 0;
		std::uint32_t queuedSlots = 0;
		std::uint32_t slotCount = 2;
		std::uint32_t queueHighWater = 0;
		std::uint32_t currentSaturationStreak = 0;
		std::uint32_t maxSaturationStreak = 0;
		double saturationRate = 0.0;
	};

	class Pipeline
	{
	public:
		static Pipeline& Instance();

		void Install();
		void SetEnabled(bool enabled);
		bool SetSlotCount(std::uint32_t slotCount);
		void SetVisualValidation(bool enabled);
		void SetWorkerNrEnabled(bool enabled);
		void SetModel3Enabled(bool enabled);
		bool SubmitWorkerTask(std::function<void()> task);
		bool SubmitTailTask(std::function<void()> task);
		void WaitForWorkerTasks();
		bool WaitForWorkerTasksFor(std::chrono::milliseconds timeout, const std::function<bool()>& cancelled);
		void NotifyWorkerWaiters();
		void RecordWorkerNrSucceeded();
		void RecordWorkerNrFallback();
		void RecordWorkerNrFailed();
		void ObserveFlatPresent(std::uintptr_t swapChain, std::uint32_t syncInterval, std::uint32_t flags);
		std::uint32_t PrepareFlatVisualValidation(std::uintptr_t swapChain, std::uint32_t flags);
		void CompleteFlatVisualValidation(std::uint32_t token, long presentResult);
		[[nodiscard]] bool IsEnabled() const;
		[[nodiscard]] bool IsWorkerNrEnabled() const;
		[[nodiscard]] std::uint32_t SlotCount() const;
		[[nodiscard]] Statistics GetStatistics() const;

	private:
		enum class SlotState : std::uint8_t
		{
			kWritable,
			kSealed,
			kConsuming,
			kSubmitted,
			kReadyToDisplay,
			kFailed,
		};

		struct SubmitTicket
		{
			enum class Source : std::uint8_t
			{
				kFlatPresent,
				kVrSubmit,
			};

			Source source = Source::kFlatPresent;
			std::uint64_t frameId = 0;
			std::uintptr_t textureHandle = 0;
			std::uint32_t textureType = 0;
			std::uint32_t colorSpace = 0;
			std::uint32_t submitFlags = 0;
			std::uint32_t syncInterval = 0;
			bool hasBounds = false;
		};

		struct Slot
		{
			SlotState state = SlotState::kWritable;
			SubmitTicket ticket;
			Microsoft::WRL::ComPtr<ID3D11Texture2D> color11;
			Microsoft::WRL::ComPtr<ID3D12Resource> color12;
			Microsoft::WRL::ComPtr<ID3D11Texture2D> validation11;
			Microsoft::WRL::ComPtr<ID3D12Resource> validation12;
			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
			std::uint64_t engineReady = 0;
			std::uint64_t tailDone = 0;
		};

		Pipeline() = default;
		Pipeline(const Pipeline&) = delete;
		Pipeline& operator=(const Pipeline&) = delete;

		bool StartWorker();
		void StopWorker();
		void Enqueue(const SubmitTicket& ticket);
		bool EnqueueFlatColor(const SubmitTicket& ticket, IDXGISwapChain* swapChain);
		bool EnsureFlatInteropLocked(IDXGISwapChain* swapChain);
		bool CreateFlatSlotResourcesLocked(const D3D11_TEXTURE2D_DESC& sourceDesc);
		void LatchInteropFault(const char* message);
		void ReapCompletedSlotsLocked();
		bool SubmitFlatColorCopy(std::size_t slotIndex, std::uint64_t& tailDone);
		void WaitForTailIdle();
		void WorkerMain(std::stop_token stopToken);

		static constexpr std::size_t kMaxSlotCount = 3;
		std::array<Slot, kMaxSlotCount> slots_;
		std::size_t activeSlotCount_ = 2;
		std::mutex lifecycleMutex_;
		mutable std::mutex mutex_;
		std::condition_variable_any condition_;
		std::jthread worker_;
		std::deque<std::function<void()>> workerTasks_;
		bool workerTaskActive_ = false;
		std::size_t producerCursor_ = 0;
		std::size_t consumerCursor_ = 0;
		std::atomic_bool enabled_ = false;
		std::atomic_bool installed_ = false;
		std::atomic_uint64_t observedSubmits_ = 0;
		std::atomic_uint64_t observedFlatPresents_ = 0;
		std::atomic_uint64_t observedVrSubmits_ = 0;
		std::atomic_uint64_t consumedSubmits_ = 0;
		std::atomic_uint64_t saturatedSubmits_ = 0;
		std::atomic_uint64_t colorCopies_ = 0;
		std::atomic_uint64_t gpuSubmits_ = 0;
		std::atomic_uint64_t gpuCompletes_ = 0;
		std::atomic_uint64_t visualPresents_ = 0;
		std::atomic_uint64_t interopFailures_ = 0;
		std::atomic_uint64_t workerNrQueued_ = 0;
		std::atomic_uint64_t workerNrSucceeded_ = 0;
		std::atomic_uint64_t workerNrFallback_ = 0;
		std::atomic_uint64_t workerNrFailed_ = 0;
		std::atomic_uint32_t queueHighWater_ = 0;
		std::atomic_uint32_t currentSaturationStreak_ = 0;
		std::atomic_uint32_t maxSaturationStreak_ = 0;
		std::atomic_bool firstConsumptionLogged_ = false;
		std::atomic_bool visualValidationEnabled_ = false;
		std::atomic_bool workerNrEnabled_ = false;
		std::atomic_bool model3Enabled_ = false;
		Microsoft::WRL::ComPtr<ID3D11Device5> device11_;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context11_;
		Microsoft::WRL::ComPtr<ID3D12Device> device12_;
		Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue12_;
		Microsoft::WRL::ComPtr<ID3D11Fence> engineFence11_;
		Microsoft::WRL::ComPtr<ID3D12Fence> engineFence12_;
		Microsoft::WRL::ComPtr<ID3D12Fence> tailFence12_;
		Microsoft::WRL::ComPtr<ID3D11Fence> tailFence11_;
		D3D11_TEXTURE2D_DESC flatColorDesc_{};
		std::uint64_t engineFenceValue_ = 0;
		std::uint64_t tailFenceValue_ = 0;
		bool flatInteropReady_ = false;
		std::atomic_bool flatInteropFaulted_ = false;

#ifdef ENABLE_SKYRIM_VR
		friend struct SubmitHook;
#endif
	};
}
