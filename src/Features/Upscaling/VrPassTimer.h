#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;

namespace VrPassTimer
{
	/** @brief Starts a timestamp range on this command list. `serial` tags the doorbell frame. */
	void Begin(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, std::uint64_t serial);
	/** @brief Closes the previous range and starts one named for the overlay. */
	void Mark(ID3D12GraphicsCommandList* list, const char* name);
	/** @brief Closes the last range and resolves timestamps on this list. */
	void Finish(ID3D12GraphicsCommandList* list);
	/** @brief Drops the open ranges without publishing. The list must not be executed. */
	void Abort();
	/** @brief After the fence for Finish, publishes each range to the overlay. */
	void Report();
	/** @brief GPU-only sum of the last resolved "VrDb::*" ranges, ms. Excludes mailbox/fence
		wait; this is what OC's L1 pace gate should read instead of a wait-inclusive wall clock. */
	float RdbGpuMs();
	/** @brief Adds a GPU cost measured outside the timestamp-query ranges (e.g. FSR FG's own
		dispatch+wait) onto the current doorbell frame's RdbGpuMs() total. Same thread as Report(). */
	void AddRdbGpuMs(float extraMs);
	/** @brief The serial passed to the latest Begin. */
	std::uint64_t Serial();
	/** @brief ReportSpan/ReportCpu on this thread use this doorbell frame. */
	void BindReportSerial(std::uint64_t serial);
	/** @brief A span already measured on the CPU. Same overlay row. */
	void ReportSpan(const char* name, float ms, bool countInFrameTotal = true);
	/** @brief CPU column for a doorbell span. Wall-clock rows pass countInFrameTotal false. */
	void ReportCpu(const char* name, float ms, bool countInFrameTotal = true);
	/** @brief Wall clock from a QPC tick to now. Uses the serial from BindReportSerial. */
	void ReportWallClock(const char* name, std::int64_t startQpc, bool countInFrameTotal = false);

	/** @brief One begin timestamp on a snapshot list. `slot` is the keep slot. */
	void BeginKeep(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::uint32_t slot);
	/** @brief End timestamp and resolve for that keep slot. */
	void FinishKeep(ID3D12GraphicsCommandList* list, std::uint32_t slot);
	/** @brief After the snapshot fence, publishes VrDb::Keep. */
	void ReportKeep(ID3D12CommandQueue* queue, std::uint32_t slot);
}
