#include "PCH.h"
#include "PreAfter.h"

#include <atomic>

namespace
{
	std::atomic<std::uint64_t> pre{ 0 };
	std::atomic<std::uint64_t> after{ 0 };
}

std::uint64_t VrCount::AddPre()
{
	const auto index = pre.fetch_add(1, std::memory_order_acq_rel);
	if (index < 8 || ((index + 1) % 120) == 0)
		logger::info("[VrCount] P={} A={}", index + 1, after.load(std::memory_order_acquire));
	return index;
}

std::uint64_t VrCount::AddAfter()
{
	const auto index = after.fetch_add(1, std::memory_order_acq_rel);
	if (index < 8 || ((index + 1) % 120) == 0)
		logger::info("[VrCount] A={} P={}", index + 1, pre.load(std::memory_order_acquire));
	return index;
}

std::uint64_t VrCount::Pre()
{
	return pre.load(std::memory_order_acquire);
}

std::uint64_t VrCount::After()
{
	return after.load(std::memory_order_acquire);
}
