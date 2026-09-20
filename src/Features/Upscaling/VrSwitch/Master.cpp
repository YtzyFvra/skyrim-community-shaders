#include "PCH.h"
#include "Master.h"

#ifdef ENABLE_SKYRIM_VR

#	include "../../../Globals.h"
#	include "../../Upscaling.h"

#	include <atomic>
#	include <condition_variable>
#	include <mutex>
#	include <thread>

namespace
{
	std::atomic<bool> applied{ false };
	std::atomic<bool> haveApplied{ false };

	// Streamline::ShutdownDLSSG() calls slFreeResources, which blocks until the
	// GPU has finished with DLSS-G's resources before freeing them. Tick() is
	// called every iteration of Doorbell::WorkerLoopImpl's loop (the same
	// thread that drains Snapshot()s into Kick()) -- calling it there stalled
	// that thread for the free's duration while the game thread kept
	// publishing new snapshots at full engine rate, dropping ~19 generations
	// per toggle (measured 09-19 23:14, confirmed unrelated to render
	// resolution -- it landed on the exact millisecond of the off transition
	// twice in a row, and a preceding 28s bridge-only stretch at the same
	// resolution dropped nothing). Route the actual SL call to a dedicated
	// thread so the falling edge only ever costs an atomic + notify on the
	// Doorbell thread.
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
			globals::features::upscaling.streamlineDX12.ShutdownDLSSG();
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
