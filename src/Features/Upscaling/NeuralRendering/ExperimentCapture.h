#pragma once

#include <cstdint>
#include <span>

struct ID3D11Resource;
struct ID3D11Texture2D;

namespace NeuralRendering::ExperimentCapture
{
	/** How a band is mapped into the 8-bit BMP preview; the raw float32 dump is always unmapped. */
	enum class BandKind : std::uint8_t
	{
		kColor,   ///< [0,1] color, written as is.
		kGain,    ///< Positive gain, BMP shows gain * 0.5 (mid grey = 1x).
		kSigned,  ///< Signed residual, BMP shows 0.5 + 4 * value.
		kDepth,   ///< Raw device depth (any depth format), BMP shows a near-stretched view.
	};

	struct BandTexture
	{
		const char* name;
		ID3D11Resource* resource;
		BandKind kind = BandKind::kColor;
	};

	bool BeginIfRequested(ID3D11Texture2D* source, const char* route);
	void Complete(ID3D11Texture2D* source, bool nrSucceeded);
	/**
	 * Writes each band as raw RGBA float32 plus a BMP preview and one JSON manifest for tools/nr_band_analysis.py.
	 * When both "I" and "N" are present their difference is written as an extra "Residual" band.
	 */
	bool SaveBands(std::span<const BandTexture> textures, std::uint32_t engineWidth, std::uint32_t engineHeight);
	void Reset();
}