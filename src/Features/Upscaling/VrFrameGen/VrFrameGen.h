#pragma once

// Operator frame generation for the VR owned-Submit path. See docs/development/vr-frame-generation.md.
// The game thread feeds per-eye E×E colour plus guides and receives an interpolated eye pair; the
// caller packs and submits. Nothing here touches a swapchain.

#include <d3d11.h>

#include <cstdint>

namespace VrFrameGen
{
	struct EyeInputs
	{
		ID3D11Texture2D* color = nullptr;        ///< E×E eye colour after six-pass (RGBA8)
		ID3D11ShaderResourceView* depth = nullptr;   ///< engine depth SRV, eye half (CopyDepthSubrect target or kMAIN view)
		ID3D11Texture2D* motionVectors = nullptr;    ///< engine motion vectors, eye half
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		float cameraNear = 0.0f;
		float cameraFar = 0.0f;
		float cameraFovAngleVertical = 0.0f;     ///< radians, per eye
		float motionVectorScaleX = 1.0f;         ///< same convention Act2FsrScaler feeds FSR3 upscaler
		float motionVectorScaleY = 1.0f;
	};

	struct FrameInputs
	{
		EyeInputs eye[2];
		float frameTimeDeltaMs = 16.0f;
		std::uint64_t frameId = 0;
		bool reset = false;                      ///< first frame after planChanged / history reset
	};

	struct FrameOutputs
	{
		ID3D11Texture2D* interpolatedEye[2]{};   ///< E×E, owned by the generator, valid until next Generate
		bool valid = false;
	};

	/** One interpolated frame per real frame, produced synchronously on the game thread. */
	class IVrFrameGenerator
	{
	public:
		virtual ~IVrFrameGenerator() = default;
		/** (Re)creates per-eye contexts for this eye size; safe to call every frame. */
		virtual bool Ensure(std::uint32_t eyeWidth, std::uint32_t eyeHeight, DXGI_FORMAT colorFormat) = 0;
		/** Records optical flow + prepare + interpolation for both eyes into the immediate context. */
		virtual bool Generate(const FrameInputs& in, FrameOutputs& out) = 0;
		virtual void Reset() = 0;
		virtual const char* Name() const = 0;
	};

	/** FidelityFX optical flow + frame interpolation on the D3D11 backend (default). */
	IVrFrameGenerator& FfxGenerator();
}
