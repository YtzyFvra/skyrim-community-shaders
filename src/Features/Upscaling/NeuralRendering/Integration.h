#pragma once

#include <cstdint>

namespace NeuralRendering
{
	struct Act2BackendStats
	{
		std::uint64_t nisSucceeded = 0;
		std::uint64_t dlssSucceeded = 0;
		std::uint64_t fsrSucceeded = 0;
	};

	/** Runs DLSS Neural Rendering on the LDR foveated regions immediately before UI composite. */
	bool ApplyFoveatedLdr();
	Act2BackendStats GetAct2BackendStats();

	/** Releases all runtime and shared-resource state. */
	void Reset();
}