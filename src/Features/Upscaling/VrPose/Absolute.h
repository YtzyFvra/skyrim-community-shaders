#pragma once

#ifdef ENABLE_SKYRIM_VR

#	include "Body.h"

/**
 * Absolute pose controller. Works before Copy on either path
 * (sim-compositor gate or bridge). Path does not change it.
 * One destination set has exactly one pose: the pose used to draw it.
 */
namespace VrPoseAbs
{
	/** @brief Copy the OpenVR pose the engine's own Wait/GetLastPoses just received. */
	void OnEngineDrawStart();

	/**
	 * @brief Freeze the pose that belongs to this specific DB publish.
	 * @param pose The pose captured at the same instant as the color content
	 * (from `SharedPublish::pose`), not whatever the engine's most recent
	 * `UpdatePoses` happens to be by the time Kick gets around to this frame.
	 * Kick is async and can lag several engine frames behind PublishKTemp;
	 * re-reading "current" pose here would pair the wrong frame's pose with
	 * this frame's pixels and judder on head rotation only.
	 */
	void LatchDraw(const VrPose::Body& pose);

	/** @brief Mark the set that the next Copy will overwrite. Call before Copy. */
	void StampReady();

	/** @brief Pose bound to the current destination set. */
	VrPose::Body Ready();
}

#endif
