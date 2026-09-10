#include "Renderer.h"

#include "ArisePipeline.h"
#include "D3D12Interop.h"
#include "GpuPass.h"
#include "Utils/LazyShader.h"

#include <array>
#include <atomic>
#include <mutex>
#include <utility>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace NeuralRendering
{
	namespace
	{
		bool GetTextureDesc(ID3D11Resource* resource, D3D11_TEXTURE2D_DESC& desc)
		{
			Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
			if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))))
				return false;
			texture->GetDesc(&desc);
			return true;
		}

		bool Matches(const SharedTexture& texture, const D3D11_TEXTURE2D_DESC& desc)
		{
			return texture.resource11 && texture.desc.Width == desc.Width && texture.desc.Height == desc.Height &&
			       texture.desc.Format == desc.Format && texture.desc.ArraySize == desc.ArraySize &&
			       texture.desc.MipLevels == desc.MipLevels && texture.desc.SampleDesc.Count == desc.SampleDesc.Count;
		}

		D3D11_TEXTURE2D_DESC MakeSharedDesc(const D3D11_TEXTURE2D_DESC& source, std::uint32_t width,
			std::uint32_t height, UINT bindFlags)
		{
			auto desc = source;
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = bindFlags;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = 0;
			return desc;
		}
	}

	class Renderer::State
	{
	public:
		~State()
		{
			auto& pipeline = ARISE::Pipeline::Instance();
			pipeline.SetWorkerNrEnabled(false);
			pipeline.WaitForWorkerTasks();
			if (model3Device && model3DrainOwner && !model3DrainOwner())
				logger::critical("[DLSSNR] Model 3 owner drain failed during renderer shutdown");
			std::scoped_lock lock(submissionMutex);
			interop.WaitForIdle();
		}

		struct EyeResources
		{
			SharedTexture color;
			SharedTexture depth;
			SharedTexture motionVectors;
			SharedTexture output;
		};

		struct PreparedArguments
		{
			std::uint32_t guideWidth = 0;
			std::uint32_t guideHeight = 0;
			std::uint32_t colorWidth = 0;
			std::uint32_t colorHeight = 0;
			std::uint32_t outputWidth = 0;
			std::uint32_t outputHeight = 0;
			float motionVectorScaleX = 1.0f;
			float motionVectorScaleY = 1.0f;
			Tuning tuning{};
			bool reset = false;
		};

		enum class AsyncSlotState : std::uint8_t
		{
			kWritable,
			kQueued,
			kSubmitted,
			kFailed,
		};

		struct AsyncSlot
		{
			EyeResources resources;
			D3D12Interop::PreparedSubmission submission;
			PreparedArguments arguments;
			std::atomic<AsyncSlotState> state = AsyncSlotState::kWritable;
		};

		bool Apply(ID3D11Device* device, ID3D11DeviceContext* context, std::uint32_t eyeIndex,
			ID3D11Resource* color, ID3D11Resource* output,
			ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
			ID3D11Resource* motionVectors,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight,
			std::uint32_t outputWidth, std::uint32_t outputHeight,
			float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning, bool forceReset)
		{
			if (failureLatched || model3Device || !device || !context || eyeIndex >= eyes.size() || !color || !output || !depth || !depthSRV || !motionVectors)
				return false;
			CS_GPU_PASS("NeuralRendering::Evaluate");

			if (!interop.IsInitialized() && !InitializeInterop(device, context))
				return false;
			if (runtime.Status() != RuntimeStatus::Initialized && !InitializeRuntime())
				return false;
			if (eyeIndex == 0 && ARISE::Pipeline::Instance().IsWorkerNrEnabled()) {
				context->CopyResource(output, color);
				if (asyncFallbackPending.exchange(false, std::memory_order_acq_rel)) {
					if (!PrepareSynchronousFallback()) {
						asyncFallbackPending.store(true, std::memory_order_release);
						return true;
					}
				} else if (ApplyAsync(context, color, output, depthSRV, motionVectors,
						guideWidth, guideHeight, colorWidth, colorHeight, outputWidth, outputHeight,
						motionVectorScaleX, motionVectorScaleY, tuning, forceReset)) {
					return true;
				} else {
					ARISE::Pipeline::Instance().RecordWorkerNrFallback();
					asyncFallbackPending.store(true, std::memory_order_release);
					return true;
				}
			}
			if (!EnsureResources(eyeIndex, color, depth, motionVectors, guideWidth, guideHeight,
					colorWidth, colorHeight, outputWidth, outputHeight))
				return LatchFailure("shared resource creation", interop.LastError());

			auto& eye = eyes[eyeIndex];
			context->CopyResource(eye.color.resource11.Get(), color);
			if (!CopyDepthGuide(context, depthSRV, eye.depth.uav11.Get(), guideWidth, guideHeight))
				return LatchFailure("depth guide conversion", E_FAIL);
			context->CopyResource(eye.motionVectors.resource11.Get(), motionVectors);

			const PreparedArguments arguments{
				guideWidth, guideHeight, colorWidth, colorHeight, outputWidth, outputHeight,
				motionVectorScaleX, motionVectorScaleY, tuning, resetPending[eyeIndex] || forceReset
			};
			D3D12Interop::PreparedSubmission submission;
			if (!PrepareInput(submission))
				return LatchFailure("PrepareInput", interop.LastError());
			if (!ExecutePrepared(eye, eyeIndex, submission, arguments))
				return LatchFailure("Feature 18", static_cast<HRESULT>(runtime.NgxResult()));
			if (!interop.QueueWaitOutput(submission))
				return LatchFailure("QueueWaitOutput", interop.LastError());

			context->CopyResource(output, eye.output.resource11.Get());
			resetPending[eyeIndex] = false;
			return true;
		}

		bool ApplyStereo(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Resource* color,
			const std::array<StereoEyeInput, 2>& inputs,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight, const Tuning& tuning)
		{
			if (failureLatched || model3Device || !device || !context || !color)
				return false;
			CS_GPU_PASS("NeuralRendering::EvaluateStereo");

			if (!interop.IsInitialized() && !InitializeInterop(device, context))
				return false;
			if (runtime.Status() != RuntimeStatus::Initialized && !InitializeRuntime())
				return false;

			D3D11_TEXTURE2D_DESC colorDesc{};
			if (!GetTextureDesc(color, colorDesc))
				return false;
			for (std::uint32_t eyeIndex = 0; eyeIndex < inputs.size(); ++eyeIndex) {
				const auto& input = inputs[eyeIndex];
				if (!input.depth || !input.depthSRV || !input.motionVectors)
					return false;
				if (!EnsureResources(eyeIndex, color, input.depth, input.motionVectors,
						guideWidth, guideHeight, colorWidth, colorHeight, colorWidth, colorHeight))
					return LatchFailure("shared resource creation", interop.LastError());

				D3D11_BOX sourceBox{
					input.sourceX, input.sourceY, 0,
					input.sourceX + colorWidth, input.sourceY + colorHeight, 1
				};
				auto& eye = eyes[eyeIndex];
				context->CopySubresourceRegion(eye.color.resource11.Get(), 0, 0, 0, 0, color, 0, &sourceBox);
				if (!CopyDepthGuide(context, input.depthSRV, eye.depth.uav11.Get(), guideWidth, guideHeight))
					return LatchFailure("depth guide conversion", E_FAIL);
				context->CopyResource(eye.motionVectors.resource11.Get(), input.motionVectors);
			}

			ID3D12GraphicsCommandList* commandList = nullptr;
			if (!interop.BeginD3D12(&commandList))
				return LatchFailure("BeginD3D12 stereo", interop.LastError());

			bool succeeded = true;
			for (std::uint32_t eyeIndex = 0; eyeIndex < inputs.size(); ++eyeIndex) {
				auto& eye = eyes[eyeIndex];
				D3D12_RESOURCE_BARRIER barriers[4]{};
				ID3D12Resource* resources[4]{
					eye.color.resource12.Get(), eye.depth.resource12.Get(),
					eye.motionVectors.resource12.Get(), eye.output.resource12.Get()
				};
				interop.BeginTotalTiming(commandList);
				for (std::size_t index = 0; index < std::size(barriers); ++index) {
					barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					barriers[index].Transition.pResource = resources[index];
					barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
					barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
					barriers[index].Transition.StateAfter = index == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS :
					                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
				}
				commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
				interop.BeginEvaluationTiming(commandList);
				const auto& input = inputs[eyeIndex];
				const bool eyeSucceeded = runtime.Execute(commandList, eyeIndex,
					eye.color.resource12.Get(), eye.depth.resource12.Get(), eye.motionVectors.resource12.Get(),
					eye.output.resource12.Get(), guideWidth, guideHeight,
					colorWidth, colorHeight, colorWidth, colorHeight,
					input.motionVectorScaleX, input.motionVectorScaleY, tuning, resetPending[eyeIndex]);
				interop.EndEvaluationTiming(commandList);
				for (auto& barrier : barriers)
					std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
				commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
				interop.EndTotalTiming(commandList);
				if (!eyeSucceeded) {
					succeeded = false;
					break;
				}
			}

			if (!interop.EndD3D12())
				return LatchFailure("EndD3D12 stereo", interop.LastError());
			if (!succeeded)
				return LatchFailure("Feature 18 stereo", static_cast<HRESULT>(runtime.NgxResult()));

			D3D11_BOX outputBox{ 0, 0, 0, colorWidth, colorHeight, 1 };
			for (std::uint32_t eyeIndex = 0; eyeIndex < inputs.size(); ++eyeIndex) {
				const auto& input = inputs[eyeIndex];
				context->CopySubresourceRegion(color, 0, input.sourceX, input.sourceY, 0,
					eyes[eyeIndex].output.resource11.Get(), 0, &outputBox);
				resetPending[eyeIndex] = false;
			}
			return true;
		}

		bool InitializeModel3(ID3D12Device* externalDevice, std::function<bool()> drainOwner)
		{
			if (!externalDevice || !drainOwner)
				return false;
			ARISE::Pipeline::Instance().WaitForWorkerTasks();
			std::scoped_lock lock(submissionMutex);
			if (model3Device == externalDevice && runtime.Status() == RuntimeStatus::Initialized) {
				model3DrainOwner = std::move(drainOwner);
				return true;
			}
			if (!interop.WaitForIdle())
				return false;
			runtime.Shutdown();
			interop.Shutdown();
			eyes = {};
			ResetAsyncSlots();
			resetPending = { true, true };
			failureLatched = false;
			model3Device = nullptr;
			if (!runtime.Probe() || !runtime.Initialize(externalDevice)) {
				logger::error("[DLSSNR] Model 3 runtime initialization failed ngx=0x{:08X} status={} detail={}",
					runtime.NgxResult(), ToString(runtime.Status()), runtime.Detail());
				return false;
			}
			model3Device = externalDevice;
			model3DrainOwner = std::move(drainOwner);
			logger::info("[DLSSNR] Model 3 runtime bound to physical D3D12 device version={} appId=0x{:08X} api=0x{:X}",
				runtime.Version(), runtime.ApplicationId(), runtime.ApiVersion());
			return true;
		}

		bool ExecuteModel3(ID3D12GraphicsCommandList* commandList, ID3D12Resource* color,
			ID3D12Resource* depth, ID3D12Resource* motionVectors, ID3D12Resource* output,
			std::uint32_t width, std::uint32_t height,
			float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning, bool reset,
			std::uint32_t slot)
		{
			std::scoped_lock lock(submissionMutex);
			if (!model3Device || runtime.Status() != RuntimeStatus::Initialized || slot >= 2)
				return false;
			return runtime.Execute(commandList, slot, color, depth, motionVectors, output,
				width, height, width, height, width, height,
				motionVectorScaleX, motionVectorScaleY, tuning, reset);
		}

		void ResetModel3()
		{
			std::scoped_lock lock(submissionMutex);
			if (!model3Device)
				return;
			runtime.ResetFeature(0);
			runtime.ResetFeature(1);
			runtime.Shutdown();
			model3Device = nullptr;
			model3DrainOwner = {};
		}

		void Reset()
		{
			auto& pipeline = ARISE::Pipeline::Instance();
			const bool restoreWorker = pipeline.IsWorkerNrEnabled();
			pipeline.SetWorkerNrEnabled(false);
			pipeline.WaitForWorkerTasks();
			if (model3Device) {
				const bool drained = model3DrainOwner && model3DrainOwner();
				if (drained) {
					std::scoped_lock lock(submissionMutex);
					runtime.ResetFeatures();
					resetPending = { true, true };
					failureLatched = false;
					copyDepthGuideCS.Reset();
				}
				if (restoreWorker)
					pipeline.SetWorkerNrEnabled(true);
				return;
			}
			{
				std::scoped_lock lock(submissionMutex);
				interop.WaitForIdle();
				runtime.Shutdown();
				interop.Shutdown();
				eyes = {};
				ResetAsyncSlots();
				resetPending = { true, true };
				failureLatched = false;
				model3Device = nullptr;
				copyDepthGuideCS.Reset();
			}
			if (restoreWorker)
				pipeline.SetWorkerNrEnabled(true);
		}

		void ResetHistory()
		{
			ARISE::Pipeline::Instance().WaitForWorkerTasks();
			if (model3Device && (!model3DrainOwner || !model3DrainOwner()))
				return;
			std::scoped_lock lock(submissionMutex);
			if (!model3Device)
				interop.WaitForIdle();
			runtime.ResetFeatures();
			resetPending = { true, true };
		}

		[[nodiscard]] bool IsFailureLatched() const { return failureLatched; }
		[[nodiscard]] double LastEvaluationGpuMs() const { std::scoped_lock lock(submissionMutex); return interop.LastEvaluationGpuMs(); }
		[[nodiscard]] double LastD3D12TotalGpuMs() const { std::scoped_lock lock(submissionMutex); return interop.LastTotalGpuMs(); }
		[[nodiscard]] std::uint64_t D3D12TotalTimingSamples() const { std::scoped_lock lock(submissionMutex); return interop.TotalTimingSamples(); }
		[[nodiscard]] std::uint64_t EvaluationTimingSamples() const { std::scoped_lock lock(submissionMutex); return interop.EvaluationTimingSamples(); }
		[[nodiscard]] std::uint64_t CpuBackpressureCount() const { std::scoped_lock lock(submissionMutex); return interop.CpuBackpressureCount(); }
		[[nodiscard]] double LastCpuBackpressureMs() const { std::scoped_lock lock(submissionMutex); return interop.LastCpuBackpressureMs(); }
		[[nodiscard]] std::uint32_t NgxResult() const { std::scoped_lock lock(submissionMutex); return runtime.NgxResult(); }
		[[nodiscard]] std::uint64_t SuccessfulFrames() const { std::scoped_lock lock(submissionMutex); return runtime.SuccessfulFrames(); }
		[[nodiscard]] const char* StatusText() const { std::scoped_lock lock(submissionMutex); return ToString(runtime.Status()); }

	private:
		bool ExecutePrepared(EyeResources& eye, std::uint32_t eyeIndex,
			const D3D12Interop::PreparedSubmission& submission, const PreparedArguments& arguments)
		{
			std::scoped_lock lock(submissionMutex);
			ID3D12GraphicsCommandList* commandList = nullptr;
			if (!interop.BeginPrepared(submission, &commandList)) {
				interop.SignalPreparedFailure(submission);
				return false;
			}
			D3D12_RESOURCE_BARRIER barriers[4]{};
			ID3D12Resource* resources[4]{
				eye.color.resource12.Get(), eye.depth.resource12.Get(),
				eye.motionVectors.resource12.Get(), eye.output.resource12.Get()
			};
			interop.BeginTotalTiming(commandList);
			for (std::size_t index = 0; index < std::size(barriers); ++index) {
				barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[index].Transition.pResource = resources[index];
				barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
				barriers[index].Transition.StateAfter = index == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS :
				                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			}
			commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
			interop.BeginEvaluationTiming(commandList);
			const bool succeeded = runtime.Execute(commandList, eyeIndex,
				eye.color.resource12.Get(), eye.depth.resource12.Get(), eye.motionVectors.resource12.Get(),
				eye.output.resource12.Get(), arguments.guideWidth, arguments.guideHeight,
				arguments.colorWidth, arguments.colorHeight, arguments.outputWidth, arguments.outputHeight,
				arguments.motionVectorScaleX, arguments.motionVectorScaleY, arguments.tuning, arguments.reset);
			interop.EndEvaluationTiming(commandList);
			for (auto& barrier : barriers)
				std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
			commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
			interop.EndTotalTiming(commandList);
			if (!interop.EndPrepared(submission)) {
				return false;
			}
			return succeeded;
		}

		bool PrepareInput(D3D12Interop::PreparedSubmission& submission)
		{
			std::scoped_lock lock(submissionMutex);
			return interop.PrepareInput(submission);
		}

		bool QueueWaitOutput(const D3D12Interop::PreparedSubmission& submission)
		{
			std::scoped_lock lock(submissionMutex);
			return interop.QueueWaitOutput(submission);
		}

		void CompletePreparedFailure(const D3D12Interop::PreparedSubmission& submission)
		{
			std::scoped_lock lock(submissionMutex);
			interop.SignalPreparedFailure(submission);
		}

		bool ApplyAsync(ID3D11DeviceContext* context, ID3D11Resource* color, ID3D11Resource* output,
			ID3D11ShaderResourceView* depthSRV, ID3D11Resource* motionVectors,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight,
			std::uint32_t outputWidth, std::uint32_t outputHeight,
			float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning, bool forceReset)
		{
			if (colorWidth != outputWidth || colorHeight != outputHeight)
				return false;
			const auto slotCount = ARISE::Pipeline::Instance().SlotCount();
			if (!EnsureAsyncResources(color, motionVectors, guideWidth, guideHeight,
					colorWidth, colorHeight, outputWidth, outputHeight, slotCount))
				return false;
			for (std::uint32_t offset = 0; offset < slotCount; ++offset) {
				const auto slotIndex = (asyncProducerCursor + offset) % slotCount;
				auto& slot = asyncSlots[slotIndex];
				auto state = slot.state.load(std::memory_order_acquire);
				if ((state == AsyncSlotState::kSubmitted || state == AsyncSlotState::kFailed) &&
					interop.IsComplete(slot.submission)) {
					if (state == AsyncSlotState::kFailed)
						asyncFallbackPending.store(true, std::memory_order_release);
					slot.state.store(AsyncSlotState::kWritable, std::memory_order_release);
					state = AsyncSlotState::kWritable;
				}
				if (state != AsyncSlotState::kWritable)
					continue;

				context->CopyResource(slot.resources.color.resource11.Get(), color);
				context->CopyResource(slot.resources.output.resource11.Get(), color);
				if (!CopyDepthGuide(context, depthSRV, slot.resources.depth.uav11.Get(), guideWidth, guideHeight))
					return false;
				context->CopyResource(slot.resources.motionVectors.resource11.Get(), motionVectors);
				slot.arguments = {
					guideWidth, guideHeight, colorWidth, colorHeight, outputWidth, outputHeight,
					motionVectorScaleX, motionVectorScaleY, tuning, resetPending[0] || forceReset
				};
				if (!PrepareInput(slot.submission))
					return false;
				slot.state.store(AsyncSlotState::kQueued, std::memory_order_release);
				if (!ARISE::Pipeline::Instance().SubmitWorkerTask([this, slotIndex] {
						auto& workerSlot = asyncSlots[slotIndex];
						const bool workerHealthy = !asyncFallbackPending.load(std::memory_order_acquire);
						const bool succeeded = workerHealthy ? ExecutePrepared(workerSlot.resources, 0,
							workerSlot.submission, workerSlot.arguments) :
							(CompletePreparedFailure(workerSlot.submission), false);
						workerSlot.state.store(succeeded ? AsyncSlotState::kSubmitted : AsyncSlotState::kFailed,
							std::memory_order_release);
						if (succeeded)
							ARISE::Pipeline::Instance().RecordWorkerNrSucceeded();
						else {
							asyncFallbackPending.store(true, std::memory_order_release);
							ARISE::Pipeline::Instance().RecordWorkerNrFailed();
						}
					})) {
					ARISE::Pipeline::Instance().WaitForWorkerTasks();
					CompletePreparedFailure(slot.submission);
					slot.state.store(AsyncSlotState::kWritable, std::memory_order_release);
					return false;
				}
				if (!QueueWaitOutput(slot.submission))
					return false;
				context->CopyResource(output, slot.resources.output.resource11.Get());
				resetPending[0] = false;
				asyncProducerCursor = (slotIndex + 1) % slotCount;
				return true;
			}
			return false;
		}

		bool CopyDepthGuide(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source,
			ID3D11UnorderedAccessView* destination, std::uint32_t width, std::uint32_t height)
		{
			auto* shader = copyDepthGuideCS.Get(
				L"Data\\Shaders\\Upscaling\\NeuralRendering\\CopyDepthGuideCS.hlsl", {}, "cs_5_0",
				"main", "NeuralRendering::CopyDepthGuideCS");
			if (!shader || !destination)
				return false;
			context->CSSetShader(shader, nullptr, 0);
			context->CSSetShaderResources(0, 1, &source);
			context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
			context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
			ID3D11ShaderResourceView* nullSRV = nullptr;
			ID3D11UnorderedAccessView* nullUAV = nullptr;
			context->CSSetShaderResources(0, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
			return true;
		}

		bool InitializeInterop(ID3D11Device* device, ID3D11DeviceContext* context)
		{
			Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
			Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
			HRESULT result = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
			if (SUCCEEDED(result)) result = dxgiDevice->GetAdapter(&adapter);
			if (FAILED(result) || !interop.Initialize(adapter.Get(), device, context))
				return LatchFailure("D3D12 interop initialization", FAILED(result) ? result : interop.LastError());
			return true;
		}

		bool InitializeRuntime()
		{
			if (!runtime.Probe() || !runtime.Initialize(interop.Device()))
				return LatchFailure("runtime initialization", static_cast<HRESULT>(runtime.NgxResult()));
			logger::info("[DLSSNR] initialized version={} appId=0x{:08X} api=0x{:X}",
				runtime.Version(), runtime.ApplicationId(), runtime.ApiVersion());
			return true;
		}

		bool EnsureResources(std::uint32_t eyeIndex, ID3D11Resource* color, ID3D11Resource* depth,
			ID3D11Resource* motionVectors, std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight,
			std::uint32_t outputWidth, std::uint32_t outputHeight)
		{
			D3D11_TEXTURE2D_DESC colorSource{}, depthSource{}, motionSource{};
			if (!GetTextureDesc(color, colorSource) || !GetTextureDesc(depth, depthSource) ||
				!GetTextureDesc(motionVectors, motionSource))
				return false;
			const UINT sharedFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			auto colorDesc = MakeSharedDesc(colorSource, colorWidth, colorHeight, sharedFlags);
			auto outputDesc = MakeSharedDesc(colorSource, outputWidth, outputHeight, D3D11_BIND_UNORDERED_ACCESS);
			auto depthDesc = MakeSharedDesc(depthSource, guideWidth, guideHeight, sharedFlags);
			depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
			auto motionDesc = MakeSharedDesc(motionSource, guideWidth, guideHeight, sharedFlags);
			auto& eye = eyes[eyeIndex];
			if (Matches(eye.color, colorDesc) && Matches(eye.output, outputDesc) &&
				Matches(eye.depth, depthDesc) && Matches(eye.motionVectors, motionDesc))
				return true;

			ARISE::Pipeline::Instance().WaitForWorkerTasks();
			std::scoped_lock lock(submissionMutex);
			if (!interop.WaitForIdle())
				return false;
			runtime.ResetFeature(eyeIndex);
			eye = {};
			if (eyeIndex == 0)
				ResetAsyncSlots();
			const std::string suffix = eyeIndex == 0 ? "Left" : "Right";
			if (!interop.CreateSharedTexture(colorDesc, eye.color, ("NeuralRendering::Color" + suffix).c_str()) ||
				!interop.CreateSharedTexture(outputDesc, eye.output, ("NeuralRendering::Output" + suffix).c_str()) ||
				!interop.CreateSharedTexture(depthDesc, eye.depth, ("NeuralRendering::Depth" + suffix).c_str()) ||
				!interop.CreateSharedTexture(motionDesc, eye.motionVectors, ("NeuralRendering::Motion" + suffix).c_str()))
				return false;
			resetPending = { true, true };
			logger::info("[DLSSNR] resources eye={} guides={}x{} color={}x{}", eyeIndex, guideWidth, guideHeight, colorWidth, colorHeight);
			return true;
		}

		bool EnsureAsyncResources(ID3D11Resource* color, ID3D11Resource* motionVectors,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight,
			std::uint32_t outputWidth, std::uint32_t outputHeight, std::uint32_t slotCount)
		{
			D3D11_TEXTURE2D_DESC colorSource{}, motionSource{};
			if (!GetTextureDesc(color, colorSource) || !GetTextureDesc(motionVectors, motionSource))
				return false;
			const UINT sharedFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			const auto colorDesc = MakeSharedDesc(colorSource, colorWidth, colorHeight, sharedFlags);
			const auto outputDesc = MakeSharedDesc(colorSource, outputWidth, outputHeight, D3D11_BIND_UNORDERED_ACCESS);
			auto depthDesc = MakeSharedDesc(motionSource, guideWidth, guideHeight, sharedFlags);
			depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
			const auto motionDesc = MakeSharedDesc(motionSource, guideWidth, guideHeight, sharedFlags);
			bool matches = true;
			for (std::uint32_t index = 0; index < slotCount; ++index) {
				const auto& resources = asyncSlots[index].resources;
				matches &= Matches(resources.color, colorDesc) && Matches(resources.output, outputDesc) &&
					Matches(resources.depth, depthDesc) && Matches(resources.motionVectors, motionDesc);
			}
			if (matches)
				return true;
			ARISE::Pipeline::Instance().WaitForWorkerTasks();
			std::scoped_lock lock(submissionMutex);
			if (!interop.WaitForIdle())
				return false;
			runtime.ResetFeature(0);
			for (std::uint32_t index = 0; index < slotCount; ++index) {
				auto& slot = asyncSlots[index];
				slot.resources = {};
				const auto suffix = "Async" + std::to_string(index);
				if (!interop.CreateSharedTexture(colorDesc, slot.resources.color,
						("NeuralRendering::Color" + suffix).c_str()) ||
					!interop.CreateSharedTexture(outputDesc, slot.resources.output,
						("NeuralRendering::Output" + suffix).c_str()) ||
					!interop.CreateSharedTexture(depthDesc, slot.resources.depth,
						("NeuralRendering::Depth" + suffix).c_str()) ||
					!interop.CreateSharedTexture(motionDesc, slot.resources.motionVectors,
						("NeuralRendering::Motion" + suffix).c_str()))
					return false;
				slot.state.store(AsyncSlotState::kWritable, std::memory_order_relaxed);
			}
			resetPending[0] = true;
			asyncProducerCursor = 0;
			logger::info("[DLSSNR] ARISE Feature 18 slots ready count={} guides={}x{} color={}x{}",
				slotCount, guideWidth, guideHeight, colorWidth, colorHeight);
			return true;
		}

		bool PrepareSynchronousFallback()
		{
			ARISE::Pipeline::Instance().WaitForWorkerTasks();
			std::scoped_lock lock(submissionMutex);
			if (!interop.WaitForIdle())
				return false;
			runtime.ResetFeature(0);
			ResetAsyncSlots();
			resetPending[0] = true;
			return true;
		}

		void ResetAsyncSlots()
		{
			for (auto& slot : asyncSlots) {
				slot.resources = {};
				slot.submission = {};
				slot.state.store(AsyncSlotState::kWritable, std::memory_order_relaxed);
			}
			asyncProducerCursor = 0;
			asyncFallbackPending.store(false, std::memory_order_relaxed);
		}

		bool LatchFailure(const char* operation, HRESULT error)
		{
			failureLatched = true;
			logger::error("[DLSSNR] {} failed hr/ngx=0x{:08X} status={} detail={}",
				operation, static_cast<std::uint32_t>(error), ToString(runtime.Status()), runtime.Detail());
			return false;
		}

		D3D12Interop interop;
		Runtime runtime;
		mutable std::mutex submissionMutex;
		Util::LazyShader<ID3D11ComputeShader> copyDepthGuideCS;
		std::array<EyeResources, 2> eyes;
		std::array<AsyncSlot, 3> asyncSlots;
		std::array<bool, 2> resetPending{ true, true };
		std::uint32_t asyncProducerCursor = 0;
		std::atomic_bool asyncFallbackPending = false;
		bool failureLatched = false;
		ID3D12Device* model3Device = nullptr;
		std::function<bool()> model3DrainOwner;
	};

	Renderer::Renderer() : state_(new State()) {}
	Renderer::~Renderer() { delete state_; }
	Renderer& Renderer::Instance() { static Renderer instance; return instance; }

	bool Renderer::Apply(ID3D11Device* device, ID3D11DeviceContext* context, std::uint32_t eyeIndex,
		ID3D11Resource* color, ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
		ID3D11Resource* motionVectors,
		std::uint32_t guideWidth, std::uint32_t guideHeight, std::uint32_t colorWidth, std::uint32_t colorHeight,
		float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning, bool forceReset)
	{
		return state_->Apply(device, context, eyeIndex, color, color, depth, depthSRV, motionVectors,
			guideWidth, guideHeight, colorWidth, colorHeight, colorWidth, colorHeight,
			motionVectorScaleX, motionVectorScaleY, tuning, forceReset);
	}

	bool Renderer::ApplyToOutput(ID3D11Device* device, ID3D11DeviceContext* context, std::uint32_t eyeIndex,
		ID3D11Resource* color, ID3D11Resource* output,
		ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV, ID3D11Resource* motionVectors,
		std::uint32_t guideWidth, std::uint32_t guideHeight,
		std::uint32_t colorWidth, std::uint32_t colorHeight,
		std::uint32_t outputWidth, std::uint32_t outputHeight,
		float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning, bool forceReset)
	{
		return state_->Apply(device, context, eyeIndex, color, output, depth, depthSRV, motionVectors,
			guideWidth, guideHeight, colorWidth, colorHeight, outputWidth, outputHeight,
			motionVectorScaleX, motionVectorScaleY, tuning, forceReset);
	}

	bool Renderer::ApplyStereo(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Resource* color,
		const std::array<StereoEyeInput, 2>& eyes,
		std::uint32_t guideWidth, std::uint32_t guideHeight,
		std::uint32_t colorWidth, std::uint32_t colorHeight, const Tuning& tuning)
	{
		return state_->ApplyStereo(device, context, color, eyes,
			guideWidth, guideHeight, colorWidth, colorHeight, tuning);
	}

	bool Renderer::InitializeModel3(ID3D12Device* externalDevice, std::function<bool()> drainOwner)
	{
		return state_->InitializeModel3(externalDevice, std::move(drainOwner));
	}
	bool Renderer::ExecuteModel3(ID3D12GraphicsCommandList* commandList, ID3D12Resource* color,
		ID3D12Resource* depth, ID3D12Resource* motionVectors, ID3D12Resource* output,
		std::uint32_t width, std::uint32_t height,
		float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning, bool reset,
		std::uint32_t slot)
	{
		return state_->ExecuteModel3(commandList, color, depth, motionVectors, output, width, height,
			motionVectorScaleX, motionVectorScaleY, tuning, reset, slot);
	}
	void Renderer::ResetModel3() { state_->ResetModel3(); }

	void Renderer::Reset() { state_->Reset(); }
	void Renderer::ResetHistory() { state_->ResetHistory(); }
	bool Renderer::IsFailureLatched() const { return state_->IsFailureLatched(); }
	double Renderer::LastEvaluationGpuMs() const { return state_->LastEvaluationGpuMs(); }
	double Renderer::LastD3D12TotalGpuMs() const { return state_->LastD3D12TotalGpuMs(); }
	std::uint64_t Renderer::D3D12TotalTimingSamples() const { return state_->D3D12TotalTimingSamples(); }
	std::uint64_t Renderer::EvaluationTimingSamples() const { return state_->EvaluationTimingSamples(); }
	std::uint64_t Renderer::CpuBackpressureCount() const { return state_->CpuBackpressureCount(); }
	double Renderer::LastCpuBackpressureMs() const { return state_->LastCpuBackpressureMs(); }
	std::uint32_t Renderer::NgxResult() const { return state_->NgxResult(); }
	std::uint64_t Renderer::SuccessfulFrames() const { return state_->SuccessfulFrames(); }
	const char* Renderer::StatusText() const { return state_->StatusText(); }
}