#pragma once

#ifdef ENABLE_SKYRIM_VR

/**
 * Hot master switch. Sits in front of G.
 * On = eOn and the G chain. Off = eOff and the bridge.
 */
namespace VrSwitch
{
	/** @brief Current hot state. */
	bool On();

	/**
	 * @brief Apply a settings edge.
	 * Falling edge shuts G down per SL/NGX (eOff + free).
	 */
	void Tick();
}

#endif
