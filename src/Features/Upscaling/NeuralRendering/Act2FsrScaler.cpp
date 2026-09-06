#include "Act2FsrScaler.h"

#include "Globals.h"
#include "GpuPass.h"
#include "Utils/Game.h"

#include <cstdlib>
#include <d3d11.h>

namespace NeuralRendering
{
	namespace
	{
		FfxResource WrapResource(ID3D11Resource* resource, FfxResourceStates state = FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ)
		{
			FfxResource wrapped{};
			wrapped.resource = resource;
			wrapped.state = state;
			wrapped.description = GetFfxResourceDescriptionDX11(resource);
			return wrapped;
		}

		bool DispatchProtected(FfxFsr3Context& context, FfxFsr3DispatchUpscaleDescription& description, bool& faulted)
		{
			faulted = false;
			bool succeeded = false;
			__try {
				succeeded = ffxFsr3ContextDispatchUpscale(&context, &description) == FFX_OK;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				succeeded = false;
				faulted = true;
			}
			return succeeded;
		}
	}

	Act2FsrScaler::~Act2FsrScaler()
	{
		Reset();
	}

	bool Act2FsrScaler::EnsureContext(ID3D11Device* device, ID3D11Resource* output,
		std::uint32_t inputWidth, std::uint32_t inputHeight,
		std::uint32_t outputWidth, std::uint32_t outputHeight)
	{
		winrt::com_ptr<ID3D11Texture2D> outputTexture;
		if (!output || FAILED(output->QueryInterface(IID_PPV_ARGS(outputTexture.put()))))
			return false;
		D3D11_TEXTURE2D_DESC outputDesc{};
		outputTexture->GetDesc(&outputDesc);
		const FfxSurfaceFormat outputFormat = outputDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ?
			FFX_SURFACE_FORMAT_R10G10B10A2_UNORM :
			(outputDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? FFX_SURFACE_FORMAT_R8G8B8A8_UNORM : FFX_SURFACE_FORMAT_UNKNOWN);
		if (outputFormat == FFX_SURFACE_FORMAT_UNKNOWN) {
			logger::error("[ACT2] FSR3 unsupported output DXGI format {}", static_cast<uint32_t>(outputDesc.Format));
			return false;
		}
		if (valid_ && inputWidth_ == inputWidth && inputHeight_ == inputHeight &&
			outputWidth_ == outputWidth && outputHeight_ == outputHeight && outputFormat_ == outputFormat)
			return true;
		Reset();

		const std::size_t scratchSize = ffxGetScratchMemorySizeDX11(1);
		scratch_ = std::calloc(scratchSize, 1);
		if (!scratch_)
			return false;
		FfxInterface backend{};
		if (ffxGetInterfaceDX11(&backend, ffxGetDeviceDX11_Fsr31(device), scratch_, scratchSize, 1) != FFX_OK) {
			Reset();
			return false;
		}

		FfxFsr3ContextDescription description{};
		description.maxRenderSize = { inputWidth, inputHeight };
		description.maxUpscaleSize = { outputWidth, outputHeight };
		description.displaySize = { outputWidth, outputHeight };
		description.flags = FFX_FSR3_ENABLE_UPSCALING_ONLY | FFX_FSR3_ENABLE_AUTO_EXPOSURE;
		description.backBufferFormat = outputFormat;
		description.backendInterfaceUpscaling = backend;
		if (ffxFsr3ContextCreate(&context_, &description) != FFX_OK) {
			Reset();
			return false;
		}

		inputWidth_ = inputWidth;
		inputHeight_ = inputHeight;
		outputWidth_ = outputWidth;
		outputHeight_ = outputHeight;
		outputFormat_ = outputFormat;
		valid_ = true;
		failureLatched_ = false;
		logger::info("[ACT2] Created dedicated FSR3 context input={}x{} output={}x{}", inputWidth, inputHeight, outputWidth, outputHeight);
		return true;
	}

	bool Act2FsrScaler::Apply(ID3D11Device* device, ID3D11DeviceContext* context,
		ID3D11Resource* color, ID3D11Resource* depth, ID3D11Resource* motionVectors,
		ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask, ID3D11Resource* output,
		std::uint32_t inputWidth, std::uint32_t inputHeight,
		std::uint32_t outputWidth, std::uint32_t outputHeight,
		float motionVectorScaleX, float motionVectorScaleY,
		float jitterX, float jitterY, float sharpness, bool reset)
	{
		if (failureLatched_ || !device || !context || !color || !depth || !motionVectors || !reactiveMask ||
			!transparencyMask || !output ||
			!EnsureContext(device, output, inputWidth, inputHeight, outputWidth, outputHeight))
			return false;

		CS_GPU_PASS("NeuralRendering::ACT2FSR3");
		FfxFsr3DispatchUpscaleDescription description{};
		description.commandList = ffxGetCommandListDX11(context);
		description.color = WrapResource(color);
		description.depth = WrapResource(depth);
		description.motionVectors = WrapResource(motionVectors);
		description.exposure = WrapResource(nullptr);
		description.upscaleOutput = WrapResource(output, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
		description.reactive = WrapResource(reactiveMask);
		description.transparencyAndComposition = WrapResource(transparencyMask);
		description.motionVectorScale = { motionVectorScaleX, motionVectorScaleY };
		description.renderSize = { inputWidth, inputHeight };
		description.upscaleSize = { outputWidth, outputHeight };
		description.jitterOffset = { -jitterX, -jitterY };
		description.frameTimeDelta = globals::game::deltaTime ? *globals::game::deltaTime * 1000.0f : 16.67f;
		description.cameraFar = globals::game::cameraFar ? *globals::game::cameraFar : 10000.0f;
		description.cameraNear = globals::game::cameraNear ? *globals::game::cameraNear : 0.1f;
		description.cameraFovAngleVertical = Util::GetVerticalFOVRad();
		description.viewSpaceToMetersFactor = 0.01428222656f;
		description.enableSharpening = sharpness > 0.0f;
		description.sharpness = std::clamp(sharpness, 0.0f, 1.0f);
		description.reset = reset;
		description.preExposure = 1.0f;
		bool faulted = false;
		const bool succeeded = DispatchProtected(context_, description, faulted);
		RecordIdleFence(device, context);
		if (faulted) {
			failureLatched_ = true;
			logger::error("[ACT2] FSR3 dispatch faulted; context quarantined until pipeline reset");
		}
		return succeeded;
	}

	void Act2FsrScaler::RecordIdleFence(ID3D11Device* device, ID3D11DeviceContext* context)
	{
		if (idleQuery_) {
			idleQuery_->Release();
			idleQuery_ = nullptr;
		}
		D3D11_QUERY_DESC description{};
		description.Query = D3D11_QUERY_EVENT;
		if (SUCCEEDED(device->CreateQuery(&description, &idleQuery_)) && idleQuery_) {
			context11_ = context;
			context->End(idleQuery_);
			context->Flush();
		}
	}

	void Act2FsrScaler::WaitForIdle()
	{
		if (!idleQuery_ || !context11_)
			return;
		const ULONGLONG start = GetTickCount64();
		while (context11_->GetData(idleQuery_, nullptr, 0, 0) == S_FALSE && GetTickCount64() - start < 5000)
			Sleep(1);
		idleQuery_->Release();
		idleQuery_ = nullptr;
		context11_ = nullptr;
	}

	void Act2FsrScaler::Reset()
	{
		WaitForIdle();
		if (valid_)
			ffxFsr3ContextDestroy(&context_);
		valid_ = false;
		failureLatched_ = false;
		context_ = {};
		if (scratch_)
			std::free(scratch_);
		scratch_ = nullptr;
		inputWidth_ = inputHeight_ = outputWidth_ = outputHeight_ = 0;
		outputFormat_ = FFX_SURFACE_FORMAT_UNKNOWN;
	}
}