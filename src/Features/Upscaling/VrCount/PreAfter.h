#pragma once

#include <cstdint>

/**
 * Split P/A counters. Increment sits in front of the action.
 * Returned value is the just-assigned 0-based index.
 */
namespace VrCount
{
	/** @brief Gate is about to Present to notify G. */
	std::uint64_t AddPre();

	/** @brief Wrapper is about to Copy into the metronome buffer. */
	std::uint64_t AddAfter();

	std::uint64_t Pre();
	std::uint64_t After();
}
