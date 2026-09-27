#pragma once

#ifdef ENABLE_SKYRIM_VR

#	include <d3d12.h>

/**
 * FSR Frame Generation interpolator for the VR doorbell.
 * Records on Queue12 and offers frames; does not own Wait or Submit.
 */
namespace VrFfxOp
{
	/**
	 * @brief Create or resize the FG context on the Tail D3D12 device.
	 * Recreates the context when the color size changes. The device must not be SL-upgraded.
	 */
	bool Ensure(ID3D12Device* device, ID3D12CommandQueue* queue,
		UINT colorW, UINT colorH, UINT guideW, UINT guideH,
		DXGI_FORMAT colorFmt, DXGI_FORMAT depthFmt, DXGI_FORMAT mvecFmt);

	/**
	 * @brief Dispatch one FG pair: interpolated offer plus real CopySbs.
	 * On dispatch timeout the interpolated offer is skipped. On CopySbs
	 * fail a prior offer is cancelled so Beat does not pair a stale interp.
	 */
	bool OnDbFinalColor(ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* mvec,
		UINT colorW, UINT colorH, UINT guideW, UINT guideH,
		DXGI_FORMAT colorFmt, DXGI_FORMAT depthFmt, DXGI_FORMAT mvecFmt);

	/**
	 * @brief Publish serial skipped, or the plan changed.
	 * Next FG dispatch resets. prevKick is cleared so the next pair is not a midpoint from the gap.
	 */
	void NoteDiscontinuity();

	/** @brief HUD already copied off the color slot this Kick is using. Null if that publish had none. */
	void NotePairedHud(ID3D12Resource* hud);

	/** @brief Camera and frame length captured with this publish, not read later. */
	struct LatchedView
	{
		float right[3]{};
		float up[3]{};
		float fwd[3]{};
		float pos[3]{};
		float jitterX = 0.0f;
		float jitterY = 0.0f;
		float nearZ = 0.15f;
		float farZ = 2000.0f;
		float fov = 1.0f;
		float frameMs = 11.11f;
	};
	void NoteLatchedView(const LatchedView& view);

	/** @brief Tear down context and GPU objects. Idempotent; may run off the doorbell thread. */
	void Shutdown();
}

#endif
