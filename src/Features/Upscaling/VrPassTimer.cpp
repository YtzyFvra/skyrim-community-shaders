#include "PCH.h"
#include "VrPassTimer.h"

#include "Profiler.h"
#include "Globals.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	using Microsoft::WRL::ComPtr;

	constexpr uint32_t kMaxStamps = 48;
	constexpr float kMinReportMs = 0.01f;

	struct Interval
	{
		const char* name = nullptr;
		uint32_t begin = 0;
		uint32_t end = 0;
	};

	struct KeepSlot
	{
		ComPtr<ID3D12QueryHeap> heap;
		ComPtr<ID3D12Resource> readback;
		bool armed = false;
	};

	ComPtr<ID3D12QueryHeap> heap;
	ComPtr<ID3D12Resource> readback;
	ID3D12CommandQueue* queue = nullptr;
	uint32_t cursor = 0;
	uint32_t openBegin = 0;
	const char* openName = nullptr;
	std::vector<Interval> intervals;
	std::uint64_t querySerial = 0;
	thread_local std::uint64_t reportSerial = 0;
	// Previous resolve is still unread. Begin must not reuse the heap.
	bool holdReport = false;
	bool bypass = false;
	KeepSlot keep[2];
	std::atomic<float> lastRdbGpuMs{ 0.0f };

	void Stamp(ID3D12GraphicsCommandList* list)
	{
		if (!list || !heap || cursor >= kMaxStamps)
			return;
		list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, cursor);
		if (openName)
			intervals.push_back({ openName, openBegin, cursor });
		openBegin = cursor;
		++cursor;
	}

	bool EnsureKeep(ID3D12Device* device, KeepSlot& slot)
	{
		if (slot.heap)
			return slot.readback != nullptr;
		D3D12_QUERY_HEAP_DESC desc{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0 };
		if (FAILED(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&slot.heap))))
			return false;
		slot.heap->SetName(L"VrPassTimer::KeepHeap");
		D3D12_HEAP_PROPERTIES heapProps{};
		heapProps.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC buffer{};
		buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buffer.Width = 2 * sizeof(uint64_t);
		buffer.Height = 1;
		buffer.DepthOrArraySize = 1;
		buffer.MipLevels = 1;
		buffer.SampleDesc.Count = 1;
		buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &buffer,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.readback))))
			return false;
		slot.readback->SetName(L"VrPassTimer::KeepReadback");
		return true;
	}
}

namespace VrPassTimer
{
	void Begin(ID3D12Device* device, ID3D12CommandQueue* inQueue, ID3D12GraphicsCommandList* list, std::uint64_t serial)
	{
		if (holdReport) {
			bypass = true;
			return;
		}
		bypass = false;
		queue = inQueue;
		cursor = 0;
		openBegin = 0;
		openName = nullptr;
		intervals.clear();
		querySerial = serial;
		if (!device || !list)
			return;
		if (!heap) {
			D3D12_QUERY_HEAP_DESC desc{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kMaxStamps, 0 };
			if (FAILED(device->CreateQueryHeap(&desc, IID_PPV_ARGS(&heap))))
				return;
			heap->SetName(L"VrPassTimer::Heap");
			D3D12_HEAP_PROPERTIES heapProps{};
			heapProps.Type = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC buffer{};
			buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			buffer.Width = kMaxStamps * sizeof(uint64_t);
			buffer.Height = 1;
			buffer.DepthOrArraySize = 1;
			buffer.MipLevels = 1;
			buffer.SampleDesc.Count = 1;
			buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &buffer,
					D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
				return;
			readback->SetName(L"VrPassTimer::Readback");
		}
		Stamp(list);
	}

	void Mark(ID3D12GraphicsCommandList* list, const char* name)
	{
		if (bypass)
			return;
		Stamp(list);
		openName = name;
	}

	void Finish(ID3D12GraphicsCommandList* list)
	{
		if (bypass)
			return;
		Stamp(list);
		openName = nullptr;
		if (!list || !heap || !readback || cursor == 0)
			return;
		list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, cursor, readback.Get(), 0);
		holdReport = true;
	}

	void Abort()
	{
		if (bypass) {
			bypass = false;
			return;
		}
		intervals.clear();
		openName = nullptr;
		cursor = 0;
	}

	void Report()
	{
		if (!readback || !queue || intervals.empty() || !globals::profiler)
			return;
		uint64_t frequency = 0;
		if (FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0)
			return;
		void* mapped = nullptr;
		if (FAILED(readback->Map(0, nullptr, &mapped)) || !mapped)
			return;
		auto* ticks = static_cast<uint64_t*>(mapped);
		float rdbSum = 0.0f;
		for (const auto& interval : intervals) {
			if (!interval.name || interval.end >= cursor || interval.begin >= interval.end)
				continue;
			const uint64_t delta = ticks[interval.end] - ticks[interval.begin];
			const float ms = static_cast<float>(static_cast<double>(delta) * 1000.0 / static_cast<double>(frequency));
			if (ms < kMinReportMs)
				continue;
			globals::profiler->AddExternalGpuSample(interval.name, ms, true, querySerial);
			if (std::strncmp(interval.name, "VrDb::", 6) == 0)
				rdbSum += ms;
		}
		readback->Unmap(0, nullptr);
		// GPU-only busy time for this doorbell frame; excludes mailbox/fence wait entirely.
		lastRdbGpuMs.store(rdbSum, std::memory_order_relaxed);
		intervals.clear();
		holdReport = false;
		bypass = false;
	}

	std::uint64_t Serial()
	{
		return querySerial;
	}

	float RdbGpuMs()
	{
		return lastRdbGpuMs.load(std::memory_order_relaxed);
	}

	void AddRdbGpuMs(float extraMs)
	{
		if (extraMs > 0.0f)
			lastRdbGpuMs.store(lastRdbGpuMs.load(std::memory_order_relaxed) + extraMs, std::memory_order_relaxed);
	}

	void BindReportSerial(std::uint64_t serial)
	{
		reportSerial = serial;
	}

	void ReportSpan(const char* name, float ms, bool countInFrameTotal)
	{
		if (globals::profiler && name)
			globals::profiler->AddExternalGpuSample(name, ms, countInFrameTotal, reportSerial);
	}

	void ReportCpu(const char* name, float ms, bool countInFrameTotal)
	{
		if (globals::profiler && name)
			globals::profiler->AddExternalCpuSample(name, ms, countInFrameTotal, reportSerial);
	}

	void ReportWallClock(const char* name, std::int64_t startQpc, bool countInFrameTotal)
	{
		if (!name || startQpc <= 0)
			return;
		LARGE_INTEGER end{};
		LARGE_INTEGER freq{};
		QueryPerformanceCounter(&end);
		QueryPerformanceFrequency(&freq);
		if (!freq.QuadPart || end.QuadPart < startQpc)
			return;
		const float ms = static_cast<float>(static_cast<double>(end.QuadPart - startQpc) * 1000.0 /
			static_cast<double>(freq.QuadPart));
		ReportSpan(name, ms, countInFrameTotal);
		ReportCpu(name, ms, countInFrameTotal);
	}

	void BeginKeep(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::uint32_t slot)
	{
		if (!device || !list || slot > 1)
			return;
		if (!EnsureKeep(device, keep[slot]))
			return;
		list->EndQuery(keep[slot].heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
		keep[slot].armed = true;
	}

	void FinishKeep(ID3D12GraphicsCommandList* list, std::uint32_t slot)
	{
		if (!list || slot > 1 || !keep[slot].armed || !keep[slot].heap || !keep[slot].readback)
			return;
		list->EndQuery(keep[slot].heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
		list->ResolveQueryData(keep[slot].heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
			keep[slot].readback.Get(), 0);
	}

	void ReportKeep(ID3D12CommandQueue* inQueue, std::uint32_t slot)
	{
		if (!inQueue || slot > 1 || !keep[slot].armed || !keep[slot].readback || !globals::profiler)
			return;
		keep[slot].armed = false;
		uint64_t frequency = 0;
		if (FAILED(inQueue->GetTimestampFrequency(&frequency)) || frequency == 0)
			return;
		void* mapped = nullptr;
		if (FAILED(keep[slot].readback->Map(0, nullptr, &mapped)) || !mapped)
			return;
		auto* ticks = static_cast<uint64_t*>(mapped);
		const float ms = static_cast<float>(static_cast<double>(ticks[1] - ticks[0]) * 1000.0 / static_cast<double>(frequency));
		keep[slot].readback->Unmap(0, nullptr);
		if (ms >= kMinReportMs)
			globals::profiler->AddExternalGpuSample("VrDb::Keep", ms, true, reportSerial);
	}
}
