#include "ArisePipeline.h"

#include "../../../Globals.h"
#include "../../../State.h"
#include "../FoveatedRender.h"
#include "../VrSubmitDlssg.h"

#include <dxgi.h>

#include <Windows.h>

#ifdef ENABLE_SKYRIM_VR
#	include "RE/B/BSOpenVR.h"

namespace NeuralRendering::ARISE
{
	struct SubmitHook
	{
		static vr::EVRCompositorError thunk(RE::BSOpenVR* openVR, const vr::Texture_t* texture,
			const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags)
		{
			vr::EVRCompositorError ownedError = vr::VRCompositorError_None;
			if (VrSubmitDlssg::TrySubmit(openVR, texture, bounds, flags, func, ownedError))
				return ownedError;
			auto& pipeline = Pipeline::Instance();
			if (pipeline.IsEnabled() && texture) {
				pipeline.Enqueue({
					.source = Pipeline::SubmitTicket::Source::kVrSubmit,
					.frameId = globals::state ? globals::state->frameCount : 0,
					.textureHandle = reinterpret_cast<std::uintptr_t>(texture->handle),
					.textureType = static_cast<std::uint32_t>(texture->eType),
					.colorSpace = static_cast<std::uint32_t>(texture->eColorSpace),
					.submitFlags = static_cast<std::uint32_t>(flags),
					.hasBounds = bounds != nullptr,
				});
			}
			return func(openVR, texture, bounds, flags);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};
}
#endif

namespace NeuralRendering::ARISE
{
	Pipeline& Pipeline::Instance()
	{
		static auto* instance = new Pipeline;
		return *instance;
	}

	void Pipeline::Install()
	{
		if (installed_.exchange(true))
			return;
		logger::info("[ARISE] Shadow scheduler installed for {} observation",
			globals::game::isVR ? "BSOpenVR::Submit" : "main DXGI Present");
#ifdef ENABLE_SKYRIM_VR
		if (globals::game::isVR) {
			stl::write_vfunc<0x03, SubmitHook>(RE::VTABLE_BSOpenVR[0]);
		}
#endif
	}

	void Pipeline::SetEnabled(bool enabled)
	{
		std::scoped_lock lifecycleLock(lifecycleMutex_);
		if (enabled && !installed_.load(std::memory_order_acquire))
			return;
		if (enabled_.load(std::memory_order_acquire) == enabled)
			return;
		if (enabled) {
			if (!StartWorker()) {
				logger::error("[ARISE] Shadow scheduler cannot start after an interop fault");
				return;
			}
			enabled_.store(true, std::memory_order_release);
			logger::info("[ARISE] Shadow scheduler enabled");
		} else {
			enabled_.store(false, std::memory_order_release);
			if (!workerNrEnabled_.load(std::memory_order_acquire) &&
				!model3Enabled_.load(std::memory_order_acquire))
				StopWorker();
			logger::info("[ARISE] Shadow scheduler disabled observed={} consumed={} saturated={}",
				observedSubmits_.load(std::memory_order_relaxed),
				consumedSubmits_.load(std::memory_order_relaxed),
				saturatedSubmits_.load(std::memory_order_relaxed));
		}
	}

	void Pipeline::SetWorkerNrEnabled(bool enabled)
	{
		std::scoped_lock lifecycleLock(lifecycleMutex_);
		if (enabled && globals::game::isVR)
			enabled = false;
		if (workerNrEnabled_.load(std::memory_order_acquire) == enabled)
			return;
		if (enabled) {
			if (!StartWorker()) {
				logger::error("[ARISE] Feature 18 worker cannot start");
				return;
			}
			workerNrEnabled_.store(true, std::memory_order_release);
			logger::info("[ARISE] Feature 18 worker enabled");
		} else {
			workerNrEnabled_.store(false, std::memory_order_release);
			WaitForWorkerTasks();
			if (!enabled_.load(std::memory_order_acquire) &&
				!model3Enabled_.load(std::memory_order_acquire))
				StopWorker();
			logger::info("[ARISE] Feature 18 worker disabled");
		}
	}

	void Pipeline::SetModel3Enabled(bool enabled)
	{
		std::scoped_lock lifecycleLock(lifecycleMutex_);
		if (enabled && globals::game::isVR)
			enabled = false;
		if (model3Enabled_.load(std::memory_order_acquire) == enabled)
			return;
		if (enabled) {
			if (!StartWorker()) {
				logger::error("[ARISE] Model 3 tail worker cannot start");
				return;
			}
			model3Enabled_.store(true, std::memory_order_release);
			logger::info("[ARISE] Model 3 tail worker enabled");
		} else {
			model3Enabled_.store(false, std::memory_order_release);
			WaitForWorkerTasks();
			if (!enabled_.load(std::memory_order_acquire) &&
				!workerNrEnabled_.load(std::memory_order_acquire))
				StopWorker();
			logger::info("[ARISE] Model 3 tail worker disabled");
		}
	}

	bool Pipeline::SubmitWorkerTask(std::function<void()> task)
	{
		if (!task || !workerNrEnabled_.load(std::memory_order_acquire))
			return false;
		std::scoped_lock lock(mutex_);
		if (!worker_.joinable() || !workerNrEnabled_.load(std::memory_order_acquire))
			return false;
		workerTasks_.push_back(std::move(task));
		++workerNrQueued_;
		condition_.notify_one();
		return true;
	}

	bool Pipeline::SubmitTailTask(std::function<void()> task)
	{
		if (!task || !model3Enabled_.load(std::memory_order_acquire))
			return false;
		std::scoped_lock lock(mutex_);
		if (!worker_.joinable() || !model3Enabled_.load(std::memory_order_acquire))
			return false;
		workerTasks_.push_back(std::move(task));
		condition_.notify_one();
		return true;
	}

	void Pipeline::WaitForWorkerTasks()
	{
		std::unique_lock lock(mutex_);
		condition_.wait(lock, [this] { return workerTasks_.empty() && !workerTaskActive_; });
	}

	bool Pipeline::WaitForWorkerTasksFor(std::chrono::milliseconds timeout, const std::function<bool()>& cancelled)
	{
		std::unique_lock lock(mutex_);
		condition_.wait_for(lock, timeout, [this, &cancelled] {
			return (workerTasks_.empty() && !workerTaskActive_) || cancelled();
		});
		return workerTasks_.empty() && !workerTaskActive_;
	}

	void Pipeline::NotifyWorkerWaiters() { condition_.notify_all(); }

	void Pipeline::RecordWorkerNrSucceeded() { ++workerNrSucceeded_; }
	void Pipeline::RecordWorkerNrFallback() { ++workerNrFallback_; }
	void Pipeline::RecordWorkerNrFailed() { ++workerNrFailed_; }

	bool Pipeline::SetSlotCount(std::uint32_t slotCount)
	{
		std::scoped_lock lifecycleLock(lifecycleMutex_);
		if (enabled_.load(std::memory_order_acquire) || workerNrEnabled_.load(std::memory_order_acquire) ||
			model3Enabled_.load(std::memory_order_acquire))
			return false;
		std::scoped_lock lock(mutex_);
		const auto newSlotCount = std::clamp<std::size_t>(slotCount, 2, kMaxSlotCount);
		if (activeSlotCount_ == newSlotCount)
			return true;
		activeSlotCount_ = newSlotCount;
		flatInteropReady_ = false;
		flatColorDesc_ = {};
		producerCursor_ = 0;
		consumerCursor_ = 0;
		for (auto& slot : slots_) {
			slot.state = SlotState::kWritable;
			slot.color11.Reset();
			slot.color12.Reset();
			slot.validation11.Reset();
			slot.validation12.Reset();
			slot.allocator.Reset();
			slot.commandList.Reset();
			slot.engineReady = 0;
			slot.tailDone = 0;
		}
		queueHighWater_.store(0, std::memory_order_relaxed);
		currentSaturationStreak_.store(0, std::memory_order_relaxed);
		maxSaturationStreak_.store(0, std::memory_order_relaxed);
		return true;
	}

	void Pipeline::SetVisualValidation(bool enabled)
	{
		visualValidationEnabled_.store(enabled, std::memory_order_release);
		if (!enabled) {
			std::scoped_lock lock(mutex_);
			for (auto& slot : slots_)
				if (slot.state == SlotState::kReadyToDisplay)
					slot.state = SlotState::kWritable;
		}
	}

	void Pipeline::ObserveFlatPresent(std::uintptr_t swapChain, std::uint32_t syncInterval, std::uint32_t flags)
	{
		if (!IsEnabled() || globals::game::isVR ||
			(flags & DXGI_PRESENT_TEST) != 0 ||
			swapChain != reinterpret_cast<std::uintptr_t>(globals::d3d::swapChain))
			return;
		EnqueueFlatColor({
			.source = SubmitTicket::Source::kFlatPresent,
			.frameId = globals::state ? globals::state->frameCount : 0,
			.textureHandle = swapChain,
			.submitFlags = flags,
			.syncInterval = syncInterval,
		}, reinterpret_cast<IDXGISwapChain*>(swapChain));
	}

	std::uint32_t Pipeline::PrepareFlatVisualValidation(std::uintptr_t swapChain, std::uint32_t flags)
	{
		if (!IsEnabled() || !visualValidationEnabled_.load(std::memory_order_acquire) || globals::game::isVR ||
			(flags & DXGI_PRESENT_TEST) != 0 ||
			swapChain != reinterpret_cast<std::uintptr_t>(globals::d3d::swapChain))
			return 0;
		std::scoped_lock lock(mutex_);
		ReapCompletedSlotsLocked();
		Slot* selected = nullptr;
		for (std::size_t index = 0; index < activeSlotCount_; ++index) {
			auto& slot = slots_[index];
			if (slot.state == SlotState::kReadyToDisplay &&
				(!selected || slot.ticket.frameId < selected->ticket.frameId))
				selected = &slot;
		}
		if (!selected || !context11_ || !tailFence11_ || !selected->validation11)
			return 0;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
		if (FAILED(reinterpret_cast<IDXGISwapChain*>(swapChain)->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
			FAILED(context11_->Wait(tailFence11_.Get(), selected->tailDone))) {
			LatchInteropFault("Visual validation backbuffer/fence handoff failed");
			selected->state = SlotState::kFailed;
			return 0;
		}
		context11_->CopyResource(backBuffer.Get(), selected->validation11.Get());
		return static_cast<std::uint32_t>(selected - slots_.data()) + 1;
	}

	void Pipeline::CompleteFlatVisualValidation(std::uint32_t token, long presentResult)
	{
		if (token == 0 || token > activeSlotCount_)
			return;
		std::scoped_lock lock(mutex_);
		auto& slot = slots_[token - 1];
		if (slot.state != SlotState::kReadyToDisplay)
			return;
		if (SUCCEEDED(presentResult)) {
			slot.state = SlotState::kWritable;
			++visualPresents_;
		}
	}

	bool Pipeline::IsEnabled() const
	{
		return enabled_.load(std::memory_order_acquire);
	}

	bool Pipeline::IsWorkerNrEnabled() const
	{
		return workerNrEnabled_.load(std::memory_order_acquire);
	}

	std::uint32_t Pipeline::SlotCount() const
	{
		std::scoped_lock lock(mutex_);
		return static_cast<std::uint32_t>(activeSlotCount_);
	}

	Statistics Pipeline::GetStatistics() const
	{
		Statistics result{
			observedSubmits_.load(std::memory_order_relaxed),
			observedFlatPresents_.load(std::memory_order_relaxed),
			observedVrSubmits_.load(std::memory_order_relaxed),
			consumedSubmits_.load(std::memory_order_relaxed),
			saturatedSubmits_.load(std::memory_order_relaxed),
			colorCopies_.load(std::memory_order_relaxed),
			gpuSubmits_.load(std::memory_order_relaxed),
			gpuCompletes_.load(std::memory_order_relaxed),
			visualPresents_.load(std::memory_order_relaxed),
			interopFailures_.load(std::memory_order_relaxed),
			workerNrQueued_.load(std::memory_order_relaxed),
			workerNrSucceeded_.load(std::memory_order_relaxed),
			workerNrFallback_.load(std::memory_order_relaxed),
			workerNrFailed_.load(std::memory_order_relaxed),
			0,
			static_cast<std::uint32_t>(activeSlotCount_),
			queueHighWater_.load(std::memory_order_relaxed),
			currentSaturationStreak_.load(std::memory_order_relaxed),
			maxSaturationStreak_.load(std::memory_order_relaxed),
			0.0,
		};
		std::scoped_lock lock(mutex_);
		for (std::size_t index = 0; index < activeSlotCount_; ++index)
			result.queuedSlots += slots_[index].state == SlotState::kWritable ? 0u : 1u;
		result.saturationRate = result.observedSubmits > 0 ?
			static_cast<double>(result.saturatedSubmits) * 100.0 / static_cast<double>(result.observedSubmits) : 0.0;
		return result;
	}

	bool Pipeline::StartWorker()
	{
		std::scoped_lock lock(mutex_);
		if (worker_.joinable())
			return true;
		if (flatInteropFaulted_.load(std::memory_order_acquire))
			return false;
		for (std::size_t index = 0; index < activeSlotCount_; ++index)
			slots_[index].state = SlotState::kWritable;
		producerCursor_ = 0;
		consumerCursor_ = 0;
		worker_ = std::jthread([this](std::stop_token stopToken) { WorkerMain(stopToken); });
		return true;
	}

	void Pipeline::StopWorker()
	{
		std::jthread worker;
		{
			std::scoped_lock lock(mutex_);
			if (!worker_.joinable())
				return;
			worker_.request_stop();
			condition_.notify_all();
			worker = std::move(worker_);
		}
		worker.join();
		WaitForTailIdle();
		std::scoped_lock lock(mutex_);
		ReapCompletedSlotsLocked();
		if (!flatInteropFaulted_.load(std::memory_order_acquire))
			for (std::size_t index = 0; index < activeSlotCount_; ++index)
				slots_[index].state = SlotState::kWritable;
	}

	bool Pipeline::EnsureFlatInteropLocked(IDXGISwapChain* swapChain)
	{
		if (flatInteropFaulted_.load(std::memory_order_acquire) || !swapChain ||
			!globals::d3d::device || !globals::d3d::context)
			return false;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
		if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
			LatchInteropFault("Failed to inspect the Flat backbuffer");
			return false;
		}
		D3D11_TEXTURE2D_DESC sourceDesc{};
		backBuffer->GetDesc(&sourceDesc);
		if (flatInteropReady_ && device11_.Get() == globals::d3d::device &&
			context11_.Get() == globals::d3d::context && sourceDesc.Width == flatColorDesc_.Width &&
			sourceDesc.Height == flatColorDesc_.Height && sourceDesc.Format == flatColorDesc_.Format)
			return true;
		if (flatInteropReady_ && (device11_.Get() != globals::d3d::device || context11_.Get() != globals::d3d::context)) {
			LatchInteropFault("D3D11 device/context changed; shared Color validation disabled for this session");
			return false;
		}

		ReapCompletedSlotsLocked();
		for (std::size_t index = 0; index < activeSlotCount_; ++index)
			if (slots_[index].state != SlotState::kWritable)
				return false;

		if (!device12_) {
			Microsoft::WRL::ComPtr<ID3D11Device5> device11;
			Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context11;
			Microsoft::WRL::ComPtr<ID3D12Device> device12;
			Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue12;
			Microsoft::WRL::ComPtr<ID3D12Fence> engineFence12;
			Microsoft::WRL::ComPtr<ID3D12Fence> tailFence12;
			Microsoft::WRL::ComPtr<ID3D11Fence> engineFence11;
			Microsoft::WRL::ComPtr<ID3D11Fence> tailFence11;
			if (FAILED(globals::d3d::device->QueryInterface(IID_PPV_ARGS(&device11))) ||
				FAILED(globals::d3d::context->QueryInterface(IID_PPV_ARGS(&context11)))) {
				LatchInteropFault("D3D11 fence interfaces unavailable");
				return false;
			}
			Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
			Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
			if (FAILED(device11.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
				FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device12)))) {
				LatchInteropFault("D3D12 device creation failed");
				return false;
			}
			D3D12_COMMAND_QUEUE_DESC queueDesc{};
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			if (FAILED(device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue12))) ||
				FAILED(device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&engineFence12))) ||
				FAILED(device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&tailFence12)))) {
				LatchInteropFault("D3D12 queue/fence creation failed");
				return false;
			}
			HANDLE sharedFence = nullptr;
			if (FAILED(device12->CreateSharedHandle(engineFence12.Get(), nullptr, GENERIC_ALL, nullptr, &sharedFence))) {
				LatchInteropFault("D3D12 shared fence handle creation failed");
				return false;
			}
			const HRESULT openResult = device11->OpenSharedFence(sharedFence, IID_PPV_ARGS(&engineFence11));
			CloseHandle(sharedFence);
			if (FAILED(openResult)) {
				LatchInteropFault("D3D11 shared fence open failed");
				return false;
			}
			sharedFence = nullptr;
			if (FAILED(device12->CreateSharedHandle(tailFence12.Get(), nullptr, GENERIC_ALL, nullptr, &sharedFence))) {
				LatchInteropFault("D3D12 tail fence handle creation failed");
				return false;
			}
			const HRESULT openTailResult = device11->OpenSharedFence(sharedFence, IID_PPV_ARGS(&tailFence11));
			CloseHandle(sharedFence);
			if (FAILED(openTailResult)) {
				LatchInteropFault("D3D11 tail fence open failed");
				return false;
			}
			device11_ = std::move(device11);
			context11_ = std::move(context11);
			device12_ = std::move(device12);
			queue12_ = std::move(queue12);
			engineFence12_ = std::move(engineFence12);
			tailFence12_ = std::move(tailFence12);
			engineFence11_ = std::move(engineFence11);
			tailFence11_ = std::move(tailFence11);
		}

		for (auto& slot : slots_) {
			slot.color11.Reset();
			slot.color12.Reset();
			slot.validation11.Reset();
			slot.validation12.Reset();
			slot.allocator.Reset();
			slot.commandList.Reset();
		}
		flatInteropReady_ = CreateFlatSlotResourcesLocked(sourceDesc);
		if (flatInteropReady_) {
			flatColorDesc_ = sourceDesc;
			logger::info("[ARISE] Flat shared Color slots ready {}x{} format={}",
				sourceDesc.Width, sourceDesc.Height, static_cast<std::uint32_t>(sourceDesc.Format));
		} else {
			LatchInteropFault("Failed to create shared Color slots; validation disabled for this session");
		}
		return flatInteropReady_;
	}

	void Pipeline::LatchInteropFault(const char* message)
	{
		if (!flatInteropFaulted_.exchange(true, std::memory_order_acq_rel)) {
			++interopFailures_;
			logger::error("[ARISE] {}", message);
		}
	}

	bool Pipeline::CreateFlatSlotResourcesLocked(const D3D11_TEXTURE2D_DESC& sourceDesc)
	{
		D3D11_TEXTURE2D_DESC desc = sourceDesc;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.CPUAccessFlags = 0;
		desc.BindFlags = 0;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		for (std::size_t index = 0; index < activeSlotCount_; ++index) {
			auto& slot = slots_[index];
			if (FAILED(device11_->CreateTexture2D(&desc, nullptr, &slot.color11)))
				return false;
			Microsoft::WRL::ComPtr<IDXGIResource1> dxgiResource;
			if (FAILED(slot.color11.As(&dxgiResource)))
				return false;
			HANDLE sharedTexture = nullptr;
			if (FAILED(dxgiResource->CreateSharedHandle(nullptr,
					DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedTexture)))
				return false;
			const HRESULT openResult = device12_->OpenSharedHandle(sharedTexture, IID_PPV_ARGS(&slot.color12));
			CloseHandle(sharedTexture);
			if (FAILED(openResult))
				return false;

			if (FAILED(device11_->CreateTexture2D(&desc, nullptr, &slot.validation11)))
				return false;
			Microsoft::WRL::ComPtr<IDXGIResource1> validationDxgiResource;
			if (FAILED(slot.validation11.As(&validationDxgiResource)))
				return false;
			HANDLE validationHandle = nullptr;
			if (FAILED(validationDxgiResource->CreateSharedHandle(nullptr,
					DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &validationHandle)))
				return false;
			const HRESULT validationOpenResult = device12_->OpenSharedHandle(
				validationHandle, IID_PPV_ARGS(&slot.validation12));
			CloseHandle(validationHandle);
			if (FAILED(validationOpenResult) ||
				FAILED(device12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
				FAILED(device12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
					IID_PPV_ARGS(&slot.commandList))) || FAILED(slot.commandList->Close()))
				return false;
			slot.state = SlotState::kWritable;
			slot.engineReady = 0;
			slot.tailDone = 0;
		}
		return true;
	}

	void Pipeline::ReapCompletedSlotsLocked()
	{
		if (!tailFence12_)
			return;
		const auto completed = tailFence12_->GetCompletedValue();
		if (completed == UINT64_MAX) {
			LatchInteropFault("Tail fence reported device removal");
			return;
		}
		for (std::size_t index = 0; index < activeSlotCount_; ++index) {
			auto& slot = slots_[index];
			if (slot.state == SlotState::kSubmitted && completed >= slot.tailDone) {
				slot.state = visualValidationEnabled_.load(std::memory_order_acquire) ?
					SlotState::kReadyToDisplay : SlotState::kWritable;
				++gpuCompletes_;
			}
		}
	}

	bool Pipeline::EnqueueFlatColor(const SubmitTicket& ticket, IDXGISwapChain* swapChain)
	{
		std::scoped_lock lock(mutex_);
		if (!enabled_.load(std::memory_order_acquire) || !worker_.joinable())
			return false;
		++observedSubmits_;
		++observedFlatPresents_;
		ReapCompletedSlotsLocked();
		if (!EnsureFlatInteropLocked(swapChain)) {
			return false;
		}
		auto& slot = slots_[producerCursor_];
		if (slot.state != SlotState::kWritable) {
			++saturatedSubmits_;
			const auto streak = currentSaturationStreak_.fetch_add(1, std::memory_order_relaxed) + 1;
			auto maximum = maxSaturationStreak_.load(std::memory_order_relaxed);
			while (maximum < streak && !maxSaturationStreak_.compare_exchange_weak(maximum, streak, std::memory_order_relaxed)) {}
			return false;
		}
		currentSaturationStreak_.store(0, std::memory_order_relaxed);
		Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
		if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
			LatchInteropFault("Failed to acquire the Flat backbuffer");
			return false;
		}
		context11_->CopyResource(slot.color11.Get(), backBuffer.Get());
		slot.engineReady = ++engineFenceValue_;
		if (FAILED(context11_->Signal(engineFence11_.Get(), slot.engineReady))) {
			LatchInteropFault("D3D11 engine-ready signal failed");
			return false;
		}
		++colorCopies_;
		slot.ticket = ticket;
		slot.state = SlotState::kSealed;
		producerCursor_ = (producerCursor_ + 1) % activeSlotCount_;
		std::uint32_t queued = 0;
		for (std::size_t index = 0; index < activeSlotCount_; ++index)
			queued += slots_[index].state == SlotState::kWritable ? 0u : 1u;
		auto highWater = queueHighWater_.load(std::memory_order_relaxed);
		while (highWater < queued && !queueHighWater_.compare_exchange_weak(highWater, queued, std::memory_order_relaxed)) {}
		condition_.notify_one();
		return true;
	}

	bool Pipeline::SubmitFlatColorCopy(std::size_t slotIndex, std::uint64_t& tailDoneOut)
	{
		auto& slot = slots_[slotIndex];
		if (!queue12_ || !engineFence12_ || !tailFence12_ || !slot.color12 || !slot.validation12 ||
			!slot.allocator || !slot.commandList || FAILED(slot.allocator->Reset()) ||
			FAILED(slot.commandList->Reset(slot.allocator.Get(), nullptr))) {
			LatchInteropFault("Tail command context reset failed");
			return false;
		}
		if (FAILED(queue12_->Wait(engineFence12_.Get(), slot.engineReady))) {
			LatchInteropFault("Tail queue engine-ready wait failed");
			return false;
		}
		D3D12_RESOURCE_BARRIER barriers[2]{};
		barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[0].Transition.pResource = slot.color12.Get();
		barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[1].Transition.pResource = slot.validation12.Get();
		barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		slot.commandList->ResourceBarrier(2, barriers);
		slot.commandList->CopyResource(slot.validation12.Get(), slot.color12.Get());
		for (auto& barrier : barriers)
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		slot.commandList->ResourceBarrier(2, barriers);
		if (FAILED(slot.commandList->Close())) {
			LatchInteropFault("Tail command list close failed");
			return false;
		}
		ID3D12CommandList* commandLists[]{ slot.commandList.Get() };
		queue12_->ExecuteCommandLists(1, commandLists);
		const std::uint64_t tailDone = tailFenceValue_ + 1;
		if (FAILED(queue12_->Signal(tailFence12_.Get(), tailDone))) {
			LatchInteropFault("Tail completion signal failed after command execution");
			return false;
		}
		tailFenceValue_ = tailDone;
		tailDoneOut = tailDone;
		++gpuSubmits_;
		return true;
	}

	void Pipeline::WaitForTailIdle()
	{
		if (!tailFence12_ || !tailFenceValue_)
			return;
		const auto completed = tailFence12_->GetCompletedValue();
		if (completed == UINT64_MAX) {
			LatchInteropFault("Tail fence reported device removal during shutdown");
			return;
		}
		if (completed >= tailFenceValue_)
			return;
		HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!eventHandle) {
			LatchInteropFault("Tail idle event creation failed");
			return;
		}
		if (SUCCEEDED(tailFence12_->SetEventOnCompletion(tailFenceValue_, eventHandle))) {
			const DWORD waitResult = WaitForSingleObject(eventHandle, 2000);
			if (waitResult != WAIT_OBJECT_0) {
				LatchInteropFault("Timed out waiting for Tail queue; resources retained for process lifetime");
			}
		} else
			LatchInteropFault("Tail idle fence event registration failed");
		CloseHandle(eventHandle);
	}

	void Pipeline::Enqueue(const SubmitTicket& ticket)
	{
		std::scoped_lock lock(mutex_);
		if (!enabled_.load(std::memory_order_acquire) || !worker_.joinable())
			return;
		++observedSubmits_;
		if (ticket.source == SubmitTicket::Source::kFlatPresent)
			++observedFlatPresents_;
		else
			++observedVrSubmits_;
		auto& slot = slots_[producerCursor_];
		if (slot.state != SlotState::kWritable) {
			++saturatedSubmits_;
			const auto streak = currentSaturationStreak_.fetch_add(1, std::memory_order_relaxed) + 1;
			auto maximum = maxSaturationStreak_.load(std::memory_order_relaxed);
			while (maximum < streak && !maxSaturationStreak_.compare_exchange_weak(maximum, streak, std::memory_order_relaxed)) {}
			return;
		}
		currentSaturationStreak_.store(0, std::memory_order_relaxed);
		slot.ticket = ticket;
		slot.state = SlotState::kSealed;
		producerCursor_ = (producerCursor_ + 1) % activeSlotCount_;
		std::uint32_t queued = 0;
		for (std::size_t index = 0; index < activeSlotCount_; ++index)
			queued += slots_[index].state == SlotState::kWritable ? 0u : 1u;
		auto highWater = queueHighWater_.load(std::memory_order_relaxed);
		while (highWater < queued && !queueHighWater_.compare_exchange_weak(highWater, queued, std::memory_order_relaxed)) {}
		condition_.notify_one();
	}

	void Pipeline::WorkerMain(std::stop_token stopToken)
	{
		while (!stopToken.stop_requested()) {
			std::unique_lock lock(mutex_);
			condition_.wait(lock, stopToken, [this] {
				return !workerTasks_.empty() || slots_[consumerCursor_].state == SlotState::kSealed;
			});
			if (stopToken.stop_requested())
				break;
			if (!workerTasks_.empty()) {
				auto task = std::move(workerTasks_.front());
				workerTasks_.pop_front();
				workerTaskActive_ = true;
				lock.unlock();
				task();
				lock.lock();
				workerTaskActive_ = false;
				condition_.notify_all();
				continue;
			}
			auto& slot = slots_[consumerCursor_];
			slot.state = SlotState::kConsuming;
			const auto ticket = slot.ticket;
			const auto slotIndex = consumerCursor_;
			lock.unlock();

			std::uint64_t tailDone = 0;
			const bool gpuSubmitted = ticket.source != SubmitTicket::Source::kFlatPresent ||
				SubmitFlatColorCopy(slotIndex, tailDone);

			lock.lock();
			if (gpuSubmitted && ticket.source == SubmitTicket::Source::kFlatPresent)
				slot.tailDone = tailDone;
			if (!gpuSubmitted && ticket.source == SubmitTicket::Source::kFlatPresent &&
				flatInteropFaulted_.load(std::memory_order_acquire))
				slot.state = SlotState::kFailed;
			else
				slot.state = gpuSubmitted && ticket.source == SubmitTicket::Source::kFlatPresent ?
					SlotState::kSubmitted : SlotState::kWritable;
			consumerCursor_ = (consumerCursor_ + 1) % activeSlotCount_;
			++consumedSubmits_;
			if (!firstConsumptionLogged_.exchange(true))
				logger::info("[ARISE] Shadow worker consumed first {} ticket at frame {}",
					ticket.source == SubmitTicket::Source::kFlatPresent ? "Flat Present" : "VR Submit",
					ticket.frameId);
		}
	}
}
