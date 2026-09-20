#pragma once

#ifdef ENABLE_SKYRIM_VR

#	include "../VrPose/Body.h"

#	include <cstdint>
#	include <d3d12.h>
#	include <dxgi1_4.h>

/**
 * G wrapper: fake Present only. Each native Present under this fake one is
 * copied to the metronome, in firing order, with a pose.
 */
namespace VrGWrap
{
	/** @brief Create the fake Present swapchain and hook. Idempotent. */
	bool Ensure(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGIAdapter* adapter,
		UINT width, UINT height, DXGI_FORMAT format);

	IDXGISwapChain3* SwapChain();
	ID3D12Resource* BackBuffer(UINT index);
	bool Ready();
	void Pump();
	void Shutdown();

	/**
	 * @brief Called from PresentGate right before sc->Present() for this Kick.
	 * Rolls the remembered real pose (this Kick's becomes "previous" on the
	 * next call) and resets the per-Kick native-fire counter, so OnPresent
	 * can tell "am I the 1st or 2nd native fire under this Kick's Present"
	 * without needing to classify real-vs-interpolated up front. SL's own
	 * "generated then real" ordering (Programming Guide) does the rest: a
	 * 1st-of-2 fire uses the midpoint of the previous and this pose (close
	 * enough during warm-up/skipped-insertion too — those are off by at
	 * most one Kick's worth of head motion, not the several-seconds-stale
	 * bug the old Pre/After index formula had); the 2nd (or the only) fire
	 * always uses this Kick's own real pose.
	 */
	void BeginKickPresent(const VrPose::Body& pose);
}

#endif
