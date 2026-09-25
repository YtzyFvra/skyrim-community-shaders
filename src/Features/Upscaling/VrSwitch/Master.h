#pragma once

#ifdef ENABLE_SKYRIM_VR

/**
 * Hot master switch. Sits in front of the interpolator.
 * On = FSR-FG operator. Off = bridge.
 */
namespace VrSwitch
{
	/** @brief Current hot state. */
	bool On();

	/**
	 * @brief Apply a settings edge.
	 * Falling edge tears the interpolator down off the doorbell thread.
	 */
	void Tick();
}

#endif
