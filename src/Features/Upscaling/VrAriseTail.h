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

	struct Status
	{
		bool active = false;
		const char* path = "off";
		double lastWaitMs = 0.0;
		std::uint64_t submits = 0;
		int lastWaitErr = 0;
		int lastSubmitL = 0;
		int lastSubmitR = 0;
	};

	/** @brief X1 counters for the menu / devbench. */
	Status GetStatus();
}
