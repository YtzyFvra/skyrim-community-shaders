#include "VrSubmitScheduler.h"

#include <PCH.h>

#ifdef ENABLE_SKYRIM_VR
#	include "../VrSubmitDlssg.h"
#endif

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <d3d11.h>

namespace
{
	std::mutex mutex;
	std::condition_variable cv;
	std::thread worker;
	bool running = false;
	bool hasPending = false;
	VrFrameGen::ScheduledSubmit pending{};
	std::atomic<std::uint64_t> submitted{ 0 };
	std::atomic<std::uint64_t> dropped{ 0 };
	bool loggedSubmit = false;

	void WaitUntilQpc(std::int64_t targetQpc)
	{
		LARGE_INTEGER freq{};
		QueryPerformanceFrequency(&freq);
		for (;;) {
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			if (now.QuadPart >= targetQpc)
				return;
			const std::int64_t remain = targetQpc - now.QuadPart;
			if (remain * 1000 > freq.QuadPart)
				Sleep(1);
			else
				SwitchToThread();
		}
	}

	void WorkerLoop()
	{
		for (;;) {
			VrFrameGen::ScheduledSubmit job{};
			{
				std::unique_lock lock(mutex);
				cv.wait(lock, [] { return !running || hasPending; });
				if (!running && !hasPending)
					return;
				if (!hasPending)
					continue;
				job = pending;
				hasPending = false;
			}

			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			if (job.targetQpc != 0 && now.QuadPart > job.targetQpc) {
				dropped.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
			if (job.targetQpc != 0)
				WaitUntilQpc(job.targetQpc);

#ifdef ENABLE_SKYRIM_VR
			if (job.sbs) {
				D3D11_TEXTURE2D_DESC desc{};
				job.sbs->GetDesc(&desc);
				if (VrSubmitDlssg::SubmitOwnedSbs(job.sbs, desc.Width, desc.Height)) {
					submitted.fetch_add(1, std::memory_order_relaxed);
					if (!loggedSubmit) {
						logger::info("[VrSubmitScheduler] owned Submit frame={} {}x{}",
							job.frameId, desc.Width, desc.Height);
						loggedSubmit = true;
					}
				}
			}
#endif
		}
	}
}

void VrFrameGen::SubmitScheduler::Start()
{
	std::lock_guard lock(mutex);
	if (running)
		return;
	running = true;
	worker = std::thread(WorkerLoop);
}

void VrFrameGen::SubmitScheduler::Stop()
{
	{
		std::lock_guard lock(mutex);
		if (!running)
			return;
		running = false;
		hasPending = false;
	}
	cv.notify_all();
	if (worker.joinable())
		worker.join();
}

void VrFrameGen::SubmitScheduler::Enqueue(const ScheduledSubmit& submit)
{
	Start();
	{
		std::lock_guard lock(mutex);
		pending = submit;
		hasPending = true;
	}
	cv.notify_one();
}

std::uint64_t VrFrameGen::SubmitScheduler::Submitted()
{
	return submitted.load(std::memory_order_relaxed);
}

std::uint64_t VrFrameGen::SubmitScheduler::Dropped()
{
	return dropped.load(std::memory_order_relaxed);
}
