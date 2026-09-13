#pragma once

// Stage 2 of vr-frame-generation.md: a worker that submits the interpolated SBS at the midpoint of
// the engine frame interval, with the interpolated pose. It never records GPU work; the game thread
// finishes the texture (Flush + fence) before Enqueue.

#include <openvr.h>

#include <cstdint>

struct ID3D11Texture2D;

namespace VrFrameGen
{
	struct ScheduledSubmit
	{
		ID3D11Texture2D* sbs = nullptr;          ///< packed 2E×E interpolated frame, ring slot owned by caller
		vr::HmdMatrix34_t pose{};                ///< pose(N-½)
		std::int64_t targetQpc = 0;              ///< when to submit: qpc(N) + interval/2
		std::uint64_t frameId = 0;
	};

	class SubmitScheduler
	{
	public:
		/** Starts the worker once; idempotent. */
		static void Start();
		static void Stop();
		/**
		 * Replaces any pending submit. Called on the game thread right after the real frame's Submit
		 * has been issued for frame N; the worker wakes at targetQpc and calls the owned Submit path
		 * with Submit_TextureWithPose. A submit whose target has already passed is dropped and counted.
		 */
		static void Enqueue(const ScheduledSubmit& submit);
		static std::uint64_t Submitted();
		static std::uint64_t Dropped();
	};
}
