#pragma once

#include <cstdint>

#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <FidelityFX/host/ffx_fsr3.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Query;
struct ID3D11Resource;

namespace NeuralRendering
{
	class Act2FsrScaler
	{
	public:
		~Act2FsrScaler();
		bool Apply(ID3D11Device* device, ID3D11DeviceContext* context,
			ID3D11Resource* color, ID3D11Resource* depth, ID3D11Resource* motionVectors,
			ID3D11Resource* reactiveMask, ID3D11Resource* transparencyMask, ID3D11Resource* output,
			std::uint32_t inputWidth, std::uint32_t inputHeight,
			std::uint32_t outputWidth, std::uint32_t outputHeight,
			float motionVectorScaleX, float motionVectorScaleY,
			float jitterX, float jitterY, float sharpness, bool reset);
		void Reset();

	private:
		bool EnsureContext(ID3D11Device* device, ID3D11Resource* output,
			std::uint32_t inputWidth, std::uint32_t inputHeight,
			std::uint32_t outputWidth, std::uint32_t outputHeight);
		void RecordIdleFence(ID3D11Device* device, ID3D11DeviceContext* context);
		void WaitForIdle();

		FfxFsr3Context context_{};
		void* scratch_ = nullptr;
		std::uint32_t inputWidth_ = 0;
		std::uint32_t inputHeight_ = 0;
		std::uint32_t outputWidth_ = 0;
		std::uint32_t outputHeight_ = 0;
		FfxSurfaceFormat outputFormat_ = FFX_SURFACE_FORMAT_UNKNOWN;
		bool valid_ = false;
		bool failureLatched_ = false;
		ID3D11DeviceContext* context11_ = nullptr;
		ID3D11Query* idleQuery_ = nullptr;
	};
}