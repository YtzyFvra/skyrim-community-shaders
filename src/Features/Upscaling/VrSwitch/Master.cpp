#include "PCH.h"
#include "Master.h"

#ifdef ENABLE_SKYRIM_VR

#	include "../../../Globals.h"
#	include "../../Upscaling.h"
#	include "../VrFfxOp/Operator.h"

#	include <atomic>
#	include <condition_variable>
#	include <mutex>
#	include <thread>

namespace
{
	std::atomic<bool> applied{ false };
	std::atomic<bool> haveApplied{ false };

	// DestroyContext waits GPU-idle. Tick() runs on the doorbell worker that
	// drains Snapshot into Kick, so teardown goes to this thread and the
	// falling edge only costs an atomic + notify there.
	std::thread shutdownThread;
	std::mutex shutdownMutex;
	std::condition_variable shutdownCv;
	bool shutdownPending = false;
	bool stopShutdownThread = false;

	void ShutdownThreadLoop()
	{
		while (true) {
			std::unique_lock lock(shutdownMutex);
			shutdownCv.wait(lock, [] { return shutdownPending || stopShutdownThread; });
			if (stopShutdownThread)
				return;
			shutdownPending = false;
			lock.unlock();
			VrFfxOp::Shutdown();
		}
	}

	void ShutdownG()
	{
		if (!shutdownThread.joinable())
			shutdownThread = std::thread(ShutdownThreadLoop);
		{
			std::lock_guard lock(shutdownMutex);
			shutdownPending = true;
		}
		shutdownCv.notify_one();
	}
}

bool VrSwitch::On()
{
	return globals::features::upscaling.settings.vrG;
}

void VrSwitch::Tick()
{
	const bool want = On();
	if (!haveApplied.exchange(true, std::memory_order_acq_rel)) {
		applied.store(want, std::memory_order_release);
		if (!want)
			ShutdownG();
		logger::info("[VrSwitch] {} ({})", want ? "on" : "off", want ? "Eon G chain" : "Eoff bridge");
		return;
	}
	const bool was = applied.exchange(want, std::memory_order_acq_rel);
	if (was == want)
		return;
	if (want)
		logger::info("[VrSwitch] on; Eon; G chain");
	else {
		ShutdownG();
		logger::info("[VrSwitch] off; Eoff; bridge");
	}
}

#endif
