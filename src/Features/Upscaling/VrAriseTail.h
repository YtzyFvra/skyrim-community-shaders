#pragma once

#include <cstdint>

struct ID3D11Texture2D;

namespace VrAriseTail
{
	/** @brief Install the UpdatePoses hook. Tail starts on the first engine Submit. */
	void Install();

	/** @brief True when `vrArise` is boot-latched (engine Submit is swallowed). */
	bool IsActive();

	/** @brief First engine Submit after VR is up: latch vrReady and start Tail. */
	void NoteEngineSubmit();

	/**
	 * @brief Swallow engine Submit (0x03 / 0x04). The first call blocks until Tail's
	 * first owned Submit returns. Does not pass the engine texture through after Tail
	 * has taken WaitGetPoses — OC session bind on that Submit can take seconds.
	 */
	bool ShouldSwallowEngineSubmit();

	/**
	 * @brief Game thread: copy kTEMP plus kMAIN depth / motion vectors into NT-shared
	 * slots and Signal one fence. Tail opens the handles on its D3D12 device.
	 */
	void PublishKTemp(ID3D11Texture2D* kTemp);
	/** @brief Game thread: copy kMENUBG into its own shared texture. Not the publish fence. */
	void NoteHud(ID3D11Texture2D* hud);
	void* HudSharedHandle();
	void* HudFenceHandle();
	std::uint64_t HudFenceValue();
	/** @brief A frame published nothing since the last success. DLSS/NR must reset. */
	bool HistoryResetPending();
	/** @brief Six-pass consumed the pending history reset. */
	void ClearHistoryReset();

	/** @brief Last doorbell kick-fence wait, in milliseconds. */
	double LastKickFenceMs();
	/** @brief Last shared-fence value the game thread signaled. */
	std::uint64_t LastPublishFence();
	/** @brief Last shared-fence value PrepareShared consumed. */
	std::uint64_t LastConsumedFence();
	/** @brief Start the tail if VR is up. Safe to call from the tonemap hook. */
	void EnsureRunning();

	struct Status
	{
		bool active = false;
		const char* path = "off";
		double lastWaitMs = 0.0;
		std::uint64_t submits = 0;
		int lastWaitErr = 0;
		int lastSubmitL = 0;
		int lastSubmitR = 0;

		// Doorbell double-buffer diagnostics (bridge and G paths both feed
		// this). Correlate a lastSnapshotWaitMs/lastKickWaitMs spike or a
		// droppedGens increase with "when did it start juddering" in the log.
		std::uint32_t lastKeepSlot = 0;
		double lastSnapshotWaitMs = 0.0;
		double lastKickWaitMs = 0.0;
		std::uint64_t droppedGens = 0;

		// PrepareShared (X2, D3D11->D3D12 cross-API handoff) failure
		// tracking. sharedPrepareFails climbing every poll (not just once)
		// means it's stuck failing every cycle, not a self-corrected hitch.
		std::uint64_t sharedPrepareFails = 0;
		const char* lastSharedFailReason = "";

		// Same idea for Snapshot (keep-buffer double-copy before Kick).
		std::uint64_t snapshotFails = 0;
		const char* lastSnapshotFailReason = "";
	};

	/** @brief X1 counters for the menu / devbench. */
	Status GetStatus();
}
