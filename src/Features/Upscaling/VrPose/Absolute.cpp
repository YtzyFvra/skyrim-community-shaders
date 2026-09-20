#include "PCH.h"
#include "Absolute.h"

#ifdef ENABLE_SKYRIM_VR

#	include "../VrBeat/Metronome.h"

#	include <mutex>

namespace
{
	std::mutex mutex;
	VrPose::Body pending{};
	VrPose::Body carry{};
	VrPose::Body ready{};
	bool haveCarry = false;
}

void VrPoseAbs::OnEngineDrawStart()
{
	VrPose::Body next{};
	next.valid = VrBeat::CopyEngineDrawPose(next.m);
	std::lock_guard lock(mutex);
	pending = next;
}

void VrPoseAbs::LatchDraw(const VrPose::Body& pose)
{
	std::lock_guard lock(mutex);
	carry = pose;
	haveCarry = true;
}

void VrPoseAbs::StampReady()
{
	std::lock_guard lock(mutex);
	ready = haveCarry ? carry : pending;
	haveCarry = false;
	static std::uint64_t n = 0;
	++n;
	if (n <= 8 || (n % 120) == 0)
		logger::info("[VrPoseAbs] Ready marked valid={}", ready.valid ? 1 : 0);
}

VrPose::Body VrPoseAbs::Ready()
{
	std::lock_guard lock(mutex);
	return ready;
}

#endif
