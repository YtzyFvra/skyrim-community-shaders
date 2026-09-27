#include "D3D12Interop.h"
#include "Features/Upscaling/VrSharedDevice.h"

#include "Utils/D3D.h"

#include <utility>

#include <Windows.h>

namespace NeuralRendering
{
	D3D12Interop::~D3D12Interop() { Shutdown(); }

	bool D3D12Interop::RecordFailure(HRESULT result)
	{
		lastError_ = result;
		return false;
	}

	bool D3D12Interop::Initialize(IDXGIAdapter* adapter, ID3D11Device* device, ID3D11DeviceContext* context)
	{
		Shutdown();
		if (!adapter || !device || !context)
			return RecordFailure(E_INVALIDARG);

		HRESULT result = device->QueryInterface(IID_PPV_ARGS(&device11_));
		if (FAILED(result)) return RecordFailure(result);
		result = context->QueryInterface(IID_PPV_ARGS(&context11_));
		if (FAILED(result)) return RecordFailure(result);
		ID3D12Device* shared = VrSharedDevice::Get(adapter);
		ID3D12CommandQueue* sharedQueue = VrSharedDevice::Queue(adapter);
		if (!shared || !sharedQueue)
			return RecordFailure(E_FAIL);
		device12_ = shared;
		queue12_ = sharedQueue;
		queue12Eye_ = sharedQueue;

		result = queue12_->GetTimestampFrequency(&timestampFrequency_);
		if (FAILED(result) || !timestampFrequency_) return RecordFailure(FAILED(result) ? result : E_FAIL);
		D3D12_QUERY_HEAP_DESC queryDesc{};
		queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		queryDesc.Count = static_cast<UINT>(kCommandContextCount * kTimingPairsPerContext * 2);
		result = device12_->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&timestampHeap_));
		if (FAILED(result)) return RecordFailure(result);
		result = device12_->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&totalTimestampHeap_));
		if (FAILED(result)) return RecordFailure(result);
		D3D12_HEAP_PROPERTIES readbackHeap{};
		readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC readbackDesc{};
		readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		readbackDesc.Width = kCommandContextCount * kTimingPairsPerContext * 2 * sizeof(std::uint64_t);
		readbackDesc.Height = 1;
		readbackDesc.DepthOrArraySize = 1;
		readbackDesc.MipLevels = 1;
		readbackDesc.SampleDesc.Count = 1;
		readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		result = device12_->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&timestampReadback_));
		if (FAILED(result)) return RecordFailure(result);
		result = device12_->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&totalTimestampReadback_));
		if (FAILED(result)) return RecordFailure(result);
		for (auto& commandContext : commandContexts_) {
			result = device12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
				IID_PPV_ARGS(&commandContext.allocator));
			if (FAILED(result)) return RecordFailure(result);
			result = device12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				commandContext.allocator.Get(), nullptr, IID_PPV_ARGS(&commandContext.commandList));
			if (FAILED(result)) return RecordFailure(result);
			result = commandContext.commandList->Close();
			if (FAILED(result)) return RecordFailure(result);
		}

		result = device12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&engineReadyFence12_));
		if (FAILED(result)) return RecordFailure(result);
		HANDLE sharedFence = nullptr;
		result = device12_->CreateSharedHandle(engineReadyFence12_.Get(), nullptr, GENERIC_ALL, nullptr, &sharedFence);
		if (FAILED(result)) return RecordFailure(result);
		result = device11_->OpenSharedFence(sharedFence, IID_PPV_ARGS(&engineReadyFence11_));
		CloseHandle(sharedFence);
		if (FAILED(result)) return RecordFailure(result);
		result = device12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&nrDoneFence12_));
		if (FAILED(result)) return RecordFailure(result);
		sharedFence = nullptr;
		result = device12_->CreateSharedHandle(nrDoneFence12_.Get(), nullptr, GENERIC_ALL, nullptr, &sharedFence);
		if (FAILED(result)) return RecordFailure(result);
		result = device11_->OpenSharedFence(sharedFence, IID_PPV_ARGS(&nrDoneFence11_));
		CloseHandle(sharedFence);
		if (FAILED(result)) return RecordFailure(result);
		result = device12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&nrDoneFence12Eye_));
		if (FAILED(result)) return RecordFailure(result);
		sharedFence = nullptr;
		result = device12_->CreateSharedHandle(nrDoneFence12Eye_.Get(), nullptr, GENERIC_ALL, nullptr, &sharedFence);
		if (FAILED(result)) return RecordFailure(result);
		result = device11_->OpenSharedFence(sharedFence, IID_PPV_ARGS(&nrDoneFence11Eye_));
		CloseHandle(sharedFence);
		if (FAILED(result)) return RecordFailure(result);

		fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!fenceEvent_) return RecordFailure(HRESULT_FROM_WIN32(GetLastError()));
		initialized_ = true;
		lastError_ = S_OK;
		logger::info("[DLSSNR] D3D12 interop initialized commandContexts={} queues=2 cpuFenceWait=backpressure-only",
			kCommandContextCount);
		return true;
	}

	void D3D12Interop::Shutdown()
	{
		if (fenceEvent_) CloseHandle(fenceEvent_);
		fenceEvent_ = nullptr;
		recording_ = false;
		recordingContext_ = kCommandContextCount;
		commandContextCursor_ = 0;
		backpressureLogged_ = false;
		timestampFrequency_ = 0;
		lastEvaluationGpuMs_ = 0.0;
		lastTotalGpuMs_ = 0.0;
		evaluationTimingSamples_ = 0;
		totalTimingSamples_ = 0;
		cpuBackpressureCount_ = 0;
		lastCpuBackpressureMs_ = 0.0;
		recordingTimingPairCount_ = 0;
		recordingTotalTimingPairCount_ = 0;
		recordingSubmission_ = {};
		evaluationTimingActive_ = false;
		totalTimingActive_ = false;
		initialized_ = false;
		engineReadyFenceValue_ = 0;
		nrDoneFenceValue_[0] = nrDoneFenceValue_[1] = 0;
		finalizedNrDoneFenceValue_[0] = finalizedNrDoneFenceValue_[1] = 0;
		engineReadyFence11_.Reset();
		nrDoneFence11_.Reset();
		nrDoneFence11Eye_.Reset();
		engineReadyFence12_.Reset();
		nrDoneFence12_.Reset();
		nrDoneFence12Eye_.Reset();
		commandContexts_ = {};
		timestampReadback_.Reset();
		timestampHeap_.Reset();
		totalTimestampReadback_.Reset();
		totalTimestampHeap_.Reset();
		queue12Eye_.Reset();
		queue12_.Reset();
		device12_.Reset();
		context11_.Reset();
		device11_.Reset();
	}

	bool D3D12Interop::CreateSharedTexture(const D3D11_TEXTURE2D_DESC& sourceDesc, SharedTexture& texture, const char* name)
	{
		if (!initialized_ || sourceDesc.Width == 0 || sourceDesc.Height == 0 ||
			sourceDesc.ArraySize == 0 || sourceDesc.ArraySize > UINT16_MAX || sourceDesc.MipLevels > UINT16_MAX)
			return RecordFailure(E_INVALIDARG);

		D3D11_TEXTURE2D_DESC desc = sourceDesc;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		if ((desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == 0) {
			lastOperation_ = "CreateSharedTextureRequiresUAV";
			return RecordFailure(E_INVALIDARG);
		}

		SharedTexture replacement;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		HRESULT result = device11_->CreateTexture2D(&desc, nullptr, &replacement.resource11);
		if (FAILED(result)) {
			lastOperation_ = "D3D11CreateTexture2D";
			return RecordFailure(result);
		}
		Util::SetResourceName(replacement.resource11.Get(), name);
		result = device11_->CreateUnorderedAccessView(replacement.resource11.Get(), nullptr, &replacement.uav11);
		if (FAILED(result)) {
			lastOperation_ = "D3D11CreateUnorderedAccessView";
			return RecordFailure(result);
		}
		Util::SetResourceName(replacement.uav11.Get(), "%s UAV", name);

		Microsoft::WRL::ComPtr<IDXGIResource1> dxgiResource;
		result = replacement.resource11.As(&dxgiResource);
		if (FAILED(result)) return RecordFailure(result);
		HANDLE sharedTexture = nullptr;
		result = dxgiResource->CreateSharedHandle(nullptr,
			DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedTexture);
		if (FAILED(result)) return RecordFailure(result);
		result = device12_->OpenSharedHandle(sharedTexture, IID_PPV_ARGS(&replacement.resource12));
		CloseHandle(sharedTexture);
		if (FAILED(result)) return RecordFailure(result);

		lastResourceFlags_ = static_cast<std::uint32_t>(replacement.resource12->GetDesc().Flags);
		replacement.desc = desc;
		texture = std::move(replacement);
		lastOperation_ = "CreateSharedTexture(D3D11First)";
		lastError_ = S_OK;
		return true;
	}

	bool D3D12Interop::PrepareInput(PreparedSubmission& submission)
	{
		if (!initialized_)
			return RecordFailure(E_UNEXPECTED);
		const std::uint32_t queueIndex = submission.queueIndex == 0 ? 0u : 1u;
		submission.queueIndex = queueIndex;
		submission.engineReady = engineReadyFenceValue_ + 1;
		submission.nrDone = nrDoneFenceValue_[queueIndex] + 1;
		lastOperation_ = "D3D11SignalEngineReady";
		const HRESULT result = context11_->Signal(engineReadyFence11_.Get(), submission.engineReady);
		if (FAILED(result)) {
			submission = {};
			return RecordFailure(result);
		}
		engineReadyFenceValue_ = submission.engineReady;
		nrDoneFenceValue_[queueIndex] = submission.nrDone;
		lastError_ = S_OK;
		return true;
	}

	bool D3D12Interop::BeginPrepared(const PreparedSubmission& submission, ID3D12GraphicsCommandList** commandList)
	{
		if (!initialized_ || recording_ || !commandList || !submission.engineReady || !submission.nrDone)
			return RecordFailure(E_UNEXPECTED);

		const std::uint32_t queueIndex = submission.queueIndex == 0 ? 0u : 1u;
		auto* const queue = queueIndex == 0 ? queue12_.Get() : queue12Eye_.Get();
		std::size_t contextIndex = kCommandContextCount;
		for (std::size_t offset = 0; offset < kCommandContextCount; ++offset) {
			const std::size_t candidate = (commandContextCursor_ + offset) % kCommandContextCount;
			const auto& candidateContext = commandContexts_[candidate];
			const auto pendingValue = candidateContext.fenceValue;
			auto* const candidateFence = candidateContext.fenceQueue == 0 ? nrDoneFence12_.Get() : nrDoneFence12Eye_.Get();
			if (pendingValue == 0 || (candidateFence && candidateFence->GetCompletedValue() >= pendingValue)) {
				contextIndex = candidate;
				break;
			}
		}
		if (contextIndex == kCommandContextCount) {
			contextIndex = commandContextCursor_;
			if (!backpressureLogged_) {
				logger::warn("[DLSSNR] D3D12 command contexts saturated; applying CPU backpressure");
				backpressureLogged_ = true;
			}
			const ULONGLONG waitStart = GetTickCount64();
			if (!WaitForFence(commandContexts_[contextIndex].fenceValue, commandContexts_[contextIndex].fenceQueue))
				return false;
			lastCpuBackpressureMs_ = static_cast<double>(GetTickCount64() - waitStart);
			++cpuBackpressureCount_;
		}

		auto& commandContext = commandContexts_[contextIndex];
		ResolveCompletedTiming(contextIndex);
		commandContext.fenceValue = 0;
		lastOperation_ = "D3D12WaitEngineReady";
		HRESULT result = queue->Wait(engineReadyFence12_.Get(), submission.engineReady);
		if (FAILED(result)) return RecordFailure(result);
		result = commandContext.allocator->Reset();
		if (FAILED(result)) return RecordFailure(result);
		result = commandContext.commandList->Reset(commandContext.allocator.Get(), nullptr);
		if (FAILED(result)) return RecordFailure(result);
		recording_ = true;
		recordingTimingPairCount_ = 0;
		recordingTotalTimingPairCount_ = 0;
		evaluationTimingActive_ = false;
		totalTimingActive_ = false;
		recordingContext_ = contextIndex;
		recordingSubmission_ = submission;
		commandContextCursor_ = (contextIndex + 1) % kCommandContextCount;
		*commandList = commandContext.commandList.Get();
		return true;
	}

	bool D3D12Interop::BeginD3D12(ID3D12GraphicsCommandList** commandList)
	{
		PreparedSubmission submission;
		if (!PrepareInput(submission))
			return false;
		if (BeginPrepared(submission, commandList))
			return true;
		const HRESULT beginError = lastError_;
		SignalPreparedFailure(submission);
		return RecordFailure(beginError);
	}

	bool D3D12Interop::BeginTotalTiming(ID3D12GraphicsCommandList* commandList)
	{
		// Queue 1 runs beside queue 0. One timestamp heap cannot be executed by both.
		if (recordingSubmission_.queueIndex != 0)
			return false;
		if (!recording_ || recordingContext_ >= kCommandContextCount || !commandList || !totalTimestampHeap_ ||
			totalTimingActive_ || recordingTotalTimingPairCount_ >= kTimingPairsPerContext)
			return false;
		const UINT queryIndex = static_cast<UINT>((recordingContext_ * kTimingPairsPerContext + recordingTotalTimingPairCount_) * 2);
		commandList->EndQuery(totalTimestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);
		totalTimingActive_ = true;
		return true;
	}

	bool D3D12Interop::EndTotalTiming(ID3D12GraphicsCommandList* commandList)
	{
		if (!recording_ || recordingContext_ >= kCommandContextCount || !commandList || !totalTimestampHeap_ ||
			!totalTimestampReadback_ || !totalTimingActive_)
			return false;
		const UINT firstQuery = static_cast<UINT>((recordingContext_ * kTimingPairsPerContext + recordingTotalTimingPairCount_) * 2);
		commandList->EndQuery(totalTimestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery + 1);
		commandList->ResolveQueryData(totalTimestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery, 2,
			totalTimestampReadback_.Get(), firstQuery * sizeof(std::uint64_t));
		++recordingTotalTimingPairCount_;
		totalTimingActive_ = false;
		return true;
	}

	bool D3D12Interop::BeginEvaluationTiming(ID3D12GraphicsCommandList* commandList)
	{
		if (recordingSubmission_.queueIndex != 0)
			return false;
		if (!recording_ || recordingContext_ >= kCommandContextCount || !commandList || !timestampHeap_ ||
			evaluationTimingActive_ || recordingTimingPairCount_ >= kTimingPairsPerContext)
			return false;
		const UINT queryIndex = static_cast<UINT>((recordingContext_ * kTimingPairsPerContext + recordingTimingPairCount_) * 2);
		commandList->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
			queryIndex);
		evaluationTimingActive_ = true;
		return true;
	}

	bool D3D12Interop::EndEvaluationTiming(ID3D12GraphicsCommandList* commandList)
	{
		if (!recording_ || recordingContext_ >= kCommandContextCount || !commandList || !timestampHeap_ ||
			!timestampReadback_ || !evaluationTimingActive_)
			return false;
		const UINT firstQuery = static_cast<UINT>((recordingContext_ * kTimingPairsPerContext + recordingTimingPairCount_) * 2);
		commandList->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery + 1);
		commandList->ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery, 2,
			timestampReadback_.Get(), firstQuery * sizeof(std::uint64_t));
		++recordingTimingPairCount_;
		evaluationTimingActive_ = false;
		return true;
	}

	bool D3D12Interop::EndPrepared(const PreparedSubmission& submission)
	{
		const std::uint32_t queueIndex = submission.queueIndex == 0 ? 0u : 1u;
		auto* const queue = queueIndex == 0 ? queue12_.Get() : queue12Eye_.Get();
		auto* const fence = queueIndex == 0 ? nrDoneFence12_.Get() : nrDoneFence12Eye_.Get();
		if (!initialized_ || !recording_ || recordingContext_ >= kCommandContextCount ||
			submission.nrDone != recordingSubmission_.nrDone ||
			submission.nrDone != finalizedNrDoneFenceValue_[queueIndex] + 1)
			return RecordFailure(E_UNEXPECTED);
		recording_ = false;
		evaluationTimingActive_ = false;
		totalTimingActive_ = false;
		auto& commandContext = commandContexts_[recordingContext_];
		recordingContext_ = kCommandContextCount;
		recordingSubmission_ = {};
		lastOperation_ = "D3D12ClosePrepared";
		HRESULT result = commandContext.commandList->Close();
		if (FAILED(result)) {
			const HRESULT signalResult = queue->Signal(fence, submission.nrDone);
			if (SUCCEEDED(signalResult))
				finalizedNrDoneFenceValue_[queueIndex] = submission.nrDone;
			return RecordFailure(result);
		}
		ID3D12CommandList* lists[] = { commandContext.commandList.Get() };
		queue->ExecuteCommandLists(1, lists);
		lastOperation_ = "D3D12SignalNrDone";
		result = queue->Signal(fence, submission.nrDone);
		if (FAILED(result)) return RecordFailure(result);
		finalizedNrDoneFenceValue_[queueIndex] = submission.nrDone;
		commandContext.fenceQueue = queueIndex;
		commandContext.fenceValue = submission.nrDone;
		commandContext.timingPairCount = recordingTimingPairCount_;
		commandContext.timingPending = recordingTimingPairCount_ != 0;
		commandContext.totalTimingPairCount = recordingTotalTimingPairCount_;
		commandContext.totalTimingPending = recordingTotalTimingPairCount_ != 0;

		lastError_ = S_OK;
		return true;
	}

	bool D3D12Interop::SignalPreparedFailure(const PreparedSubmission& submission)
	{
		const std::uint32_t queueIndex = submission.queueIndex == 0 ? 0u : 1u;
		auto* const queue = queueIndex == 0 ? queue12_.Get() : queue12Eye_.Get();
		auto* const fence = queueIndex == 0 ? nrDoneFence12_.Get() : nrDoneFence12Eye_.Get();
		if (!initialized_ || !submission.nrDone || submission.nrDone != finalizedNrDoneFenceValue_[queueIndex] + 1)
			return RecordFailure(E_UNEXPECTED);
		if (recording_) {
			recording_ = false;
			recordingContext_ = kCommandContextCount;
			recordingSubmission_ = {};
			evaluationTimingActive_ = false;
			totalTimingActive_ = false;
		}
		lastOperation_ = "D3D12SignalFailedNrDone";
		const HRESULT result = queue->Signal(fence, submission.nrDone);
		if (FAILED(result))
			return RecordFailure(result);
		finalizedNrDoneFenceValue_[queueIndex] = submission.nrDone;
		lastError_ = S_OK;
		return true;
	}

	bool D3D12Interop::QueueWaitOutput(const PreparedSubmission& submission)
	{
		if (!initialized_ || !submission.nrDone)
			return RecordFailure(E_UNEXPECTED);
		lastOperation_ = "D3D11WaitNrDone";
		auto* const fence11 = submission.queueIndex == 0 ? nrDoneFence11_.Get() : nrDoneFence11Eye_.Get();
		const HRESULT result = context11_->Wait(fence11, submission.nrDone);
		return SUCCEEDED(result) ? (lastError_ = S_OK, true) : RecordFailure(result);
	}

	bool D3D12Interop::IsComplete(const PreparedSubmission& submission) const
	{
		auto* const fence12 = submission.queueIndex == 0 ? nrDoneFence12_.Get() : nrDoneFence12Eye_.Get();
		return initialized_ && submission.nrDone && fence12 && fence12->GetCompletedValue() >= submission.nrDone;
	}

	bool D3D12Interop::EndD3D12()
	{
		const auto submission = recordingSubmission_;
		return EndPrepared(submission) && QueueWaitOutput(submission);
	}

	void D3D12Interop::ResolveCompletedTiming(std::size_t contextIndex)
	{
		if (contextIndex >= kCommandContextCount || !timestampFrequency_)
			return;
		auto& commandContext = commandContexts_[contextIndex];
		if (commandContext.timingPending) {
			double gpuMs = 0.0;
			if (ResolveTimingBuffer(timestampReadback_.Get(), contextIndex, commandContext.timingPairCount, gpuMs)) {
				lastEvaluationGpuMs_ = gpuMs;
				++evaluationTimingSamples_;
			}
			commandContext.timingPending = false;
			commandContext.timingPairCount = 0;
		}
		if (commandContext.totalTimingPending) {
			double gpuMs = 0.0;
			if (ResolveTimingBuffer(totalTimestampReadback_.Get(), contextIndex, commandContext.totalTimingPairCount, gpuMs)) {
				lastTotalGpuMs_ = gpuMs;
				++totalTimingSamples_;
			}
			commandContext.totalTimingPending = false;
			commandContext.totalTimingPairCount = 0;
		}
	}

	bool D3D12Interop::ResolveTimingBuffer(ID3D12Resource* readback, std::size_t contextIndex, std::uint32_t pairCount, double& gpuMs)
	{
		if (!readback || !pairCount || !timestampFrequency_)
			return false;
		const std::size_t firstTimestamp = contextIndex * kTimingPairsPerContext * 2;
		const D3D12_RANGE readRange{
			firstTimestamp * sizeof(std::uint64_t),
			(firstTimestamp + pairCount * 2) * sizeof(std::uint64_t)
		};
		void* mapped = nullptr;
		if (FAILED(readback->Map(0, &readRange, &mapped)) || !mapped)
			return false;
		gpuMs = 0.0;
		{
			const auto* timestamps = static_cast<const std::uint64_t*>(mapped);
			for (std::uint32_t pairIndex = 0; pairIndex < pairCount; ++pairIndex) {
				const auto* pair = timestamps + firstTimestamp + pairIndex * 2;
				if (pair[1] >= pair[0])
					gpuMs += static_cast<double>(pair[1] - pair[0]) * 1000.0 /
						static_cast<double>(timestampFrequency_);
			}
			const D3D12_RANGE writtenRange{ 0, 0 };
			readback->Unmap(0, &writtenRange);
		}
		return true;
	}

	bool D3D12Interop::WaitForFence(std::uint64_t value, std::uint32_t queueIndex)
	{
		auto* const fence = queueIndex == 0 ? nrDoneFence12_.Get() : nrDoneFence12Eye_.Get();
		if (!value || !fence || fence->GetCompletedValue() >= value)
			return true;
		const HRESULT result = fence->SetEventOnCompletion(value, fenceEvent_);
		if (FAILED(result)) return RecordFailure(result);
		const DWORD waitResult = WaitForSingleObject(fenceEvent_, 250);
		if (waitResult != WAIT_OBJECT_0)
			return RecordFailure(waitResult == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) :
			                                                        HRESULT_FROM_WIN32(GetLastError()));
		lastError_ = S_OK;
		return true;
	}

	bool D3D12Interop::WaitForIdle()
	{
		if (!initialized_)
			return true;
		for (const auto& commandContext : commandContexts_) {
			if (!WaitForFence(commandContext.fenceValue, commandContext.fenceQueue))
				return false;
		}
		return true;
	}
}