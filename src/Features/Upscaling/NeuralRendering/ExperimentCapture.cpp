#include "ExperimentCapture.h"

#include "Globals.h"
#include "State.h"

#include <DirectXTex.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <winrt/base.h>

namespace NeuralRendering::ExperimentCapture
{
	namespace
	{
		constexpr auto kCaptureDirectory = "Data\\SKSE\\Plugins\\CommunityShaders\\PrivateNR";
		constexpr auto kRequestPath = "Data\\SKSE\\Plugins\\CommunityShaders\\PrivateNR\\capture.request";
		constexpr std::uint32_t kPollIntervalFrames = 30;

		struct CapturePair
		{
			DirectX::ScratchImage srHigh;
			std::uint32_t frame = 0;
			D3D11_TEXTURE2D_DESC desc{};
			std::string route;
		};

		std::unique_ptr<CapturePair> pendingCapture;
		std::uint32_t lastPollFrame = UINT32_MAX;

		bool ConsumeRequest(std::uint32_t frame)
		{
			if (lastPollFrame == frame || frame % kPollIntervalFrames != 0)
				return false;
			lastPollFrame = frame;

			std::error_code error;
			if (!std::filesystem::exists(kRequestPath, error) || error)
				return false;
			if (!std::filesystem::remove(kRequestPath, error) || error) {
				logger::warn("[DLSSNR Experiment] Could not consume capture request: {}", error.message());
				return false;
			}
			return true;
		}

		bool SavePair(const CapturePair& capture, const DirectX::ScratchImage& nrFull)
		{
			std::error_code error;
			std::filesystem::create_directories(kCaptureDirectory, error);
			if (error) {
				logger::error("[DLSSNR Experiment] Could not create capture directory: {}", error.message());
				return false;
			}

			const auto stem = std::format("capture_{}_{}", capture.frame, capture.route);
			const auto directory = std::filesystem::path(kCaptureDirectory);
			const auto srPath = directory / std::format("{}_sr_high.dds", stem);
			const auto nrPath = directory / std::format("{}_nr_full.dds", stem);
			const auto metadataPath = directory / std::format("{}.json", stem);

			HRESULT result = DirectX::SaveToDDSFile(capture.srHigh.GetImages(), capture.srHigh.GetImageCount(),
				capture.srHigh.GetMetadata(), DirectX::DDS_FLAGS_NONE, srPath.c_str());
			if (FAILED(result)) {
				logger::error("[DLSSNR Experiment] Failed to save SR input hr=0x{:08X}", static_cast<std::uint32_t>(result));
				return false;
			}
			result = DirectX::SaveToDDSFile(nrFull.GetImages(), nrFull.GetImageCount(), nrFull.GetMetadata(),
				DirectX::DDS_FLAGS_NONE, nrPath.c_str());
			if (FAILED(result)) {
				logger::error("[DLSSNR Experiment] Failed to save NR output hr=0x{:08X}", static_cast<std::uint32_t>(result));
				return false;
			}

			std::ofstream metadata(metadataPath);
			metadata << "{\n"
				         << "  \"frameId\": " << capture.frame << ",\n"
				         << "  \"route\": \"" << capture.route << "\",\n"
				         << "  \"width\": " << capture.desc.Width << ",\n"
				         << "  \"height\": " << capture.desc.Height << ",\n"
				         << "  \"dxgiFormat\": " << static_cast<std::uint32_t>(capture.desc.Format) << ",\n"
				         << "  \"transfer\": \"LDR pipeline value; verify UNORM versus SRGB view\",\n"
				         << "  \"workingScale\": 1.0,\n"
				         << "  \"srHigh\": \"" << srPath.filename().string() << "\",\n"
				         << "  \"nrFull\": \"" << nrPath.filename().string() << "\"\n"
				         << "}\n";
			if (!metadata) {
				logger::error("[DLSSNR Experiment] Failed to save capture metadata");
				return false;
			}

			logger::info("[DLSSNR Experiment] Saved paired frame {} to {}", capture.frame, directory.string());
			return true;
		}
	}

	bool BeginIfRequested(ID3D11Texture2D* source, const char* route)
	{
		if (!source || pendingCapture)
			return false;

		const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
		if (!ConsumeRequest(frame))
			return false;

		auto capture = std::make_unique<CapturePair>();
		capture->frame = frame;
		capture->route = route ? route : "unknown";
		source->GetDesc(&capture->desc);
		const HRESULT result = DirectX::CaptureTexture(
			globals::d3d::device, globals::d3d::context, source, capture->srHigh);
		if (FAILED(result)) {
			logger::error("[DLSSNR Experiment] Failed to capture SR input hr=0x{:08X}", static_cast<std::uint32_t>(result));
			return false;
		}

		pendingCapture = std::move(capture);
		logger::info("[DLSSNR Experiment] Captured SR input for frame {} route={}", frame, pendingCapture->route);
		return true;
	}

	void Complete(ID3D11Texture2D* source, bool nrSucceeded)
	{
		if (!pendingCapture)
			return;
		if (!nrSucceeded || !source) {
			logger::warn("[DLSSNR Experiment] Discarded frame {} because NR did not complete", pendingCapture->frame);
			pendingCapture.reset();
			return;
		}

		D3D11_TEXTURE2D_DESC desc{};
		source->GetDesc(&desc);
		if (desc.Width != pendingCapture->desc.Width || desc.Height != pendingCapture->desc.Height ||
			desc.Format != pendingCapture->desc.Format) {
			logger::error("[DLSSNR Experiment] Output texture contract changed during paired capture");
			pendingCapture.reset();
			return;
		}

		DirectX::ScratchImage nrFull;
		const HRESULT result = DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, source, nrFull);
		if (FAILED(result)) {
			logger::error("[DLSSNR Experiment] Failed to capture NR output hr=0x{:08X}", static_cast<std::uint32_t>(result));
			pendingCapture.reset();
			return;
		}

		SavePair(*pendingCapture, nrFull);
		pendingCapture.reset();
	}

	namespace
	{
		struct FloatBand
		{
			std::string name;
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			std::uint32_t dxgiFormat = 0;
			std::vector<float> rgba;
		};

		bool IsDepthFormat(DXGI_FORMAT format)
		{
			switch (format) {
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT:
			case DXGI_FORMAT_R24G8_TYPELESS:
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
			case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
				return true;
			default:
				return false;
			}
		}

		// Depth formats are not convertible through DirectXTex; unpack them by hand into a grey RGBA float band.
		bool UnpackDepth(const DirectX::Image& image, FloatBand& band)
		{
			band.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
			for (std::size_t y = 0; y < image.height; ++y) {
				const std::uint8_t* row = image.pixels + y * image.rowPitch;
				for (std::size_t x = 0; x < image.width; ++x) {
					float depth = 0.0f;
					switch (image.format) {
					case DXGI_FORMAT_R32_TYPELESS:
					case DXGI_FORMAT_D32_FLOAT:
					case DXGI_FORMAT_R32_FLOAT:
						std::memcpy(&depth, row + x * 4, sizeof(float));
						break;
					case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
					case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
						std::memcpy(&depth, row + x * 8, sizeof(float));
						break;
					case DXGI_FORMAT_R24G8_TYPELESS:
					case DXGI_FORMAT_D24_UNORM_S8_UINT:
					case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: {
						std::uint32_t packed = 0;
						std::memcpy(&packed, row + x * 4, sizeof(packed));
						depth = static_cast<float>(packed & 0xFFFFFFu) / 16777215.0f;
						break;
					}
					default:
						return false;
					}
					float* out = band.rgba.data() + (y * image.width + x) * 4;
					out[0] = out[1] = out[2] = depth;
					out[3] = 1.0f;
				}
			}
			return true;
		}

		bool CaptureFloatBand(const char* name, ID3D11Resource* resource, BandKind kind, FloatBand& band)
		{
			band.name = name;
			DirectX::ScratchImage captured;
			HRESULT result = DirectX::CaptureTexture(globals::d3d::device, globals::d3d::context, resource, captured);
			if (FAILED(result)) {
				logger::error("[DLSSNR Experiment] Band {} capture failed hr=0x{:08X}", name, static_cast<std::uint32_t>(result));
				return false;
			}
			const DirectX::Image* image = captured.GetImage(0, 0, 0);
			band.width = static_cast<std::uint32_t>(image->width);
			band.height = static_cast<std::uint32_t>(image->height);
			band.dxgiFormat = static_cast<std::uint32_t>(image->format);
			if (kind == BandKind::kDepth || IsDepthFormat(image->format)) {
				if (!UnpackDepth(*image, band)) {
					logger::error("[DLSSNR Experiment] Band {} has unsupported depth format {}", name, band.dxgiFormat);
					return false;
				}
				return true;
			}
			DirectX::ScratchImage converted;
			if (image->format != DXGI_FORMAT_R32G32B32A32_FLOAT) {
				result = DirectX::Convert(*image, DXGI_FORMAT_R32G32B32A32_FLOAT, DirectX::TEX_FILTER_DEFAULT,
					DirectX::TEX_THRESHOLD_DEFAULT, converted);
				if (FAILED(result)) {
					logger::error("[DLSSNR Experiment] Band {} convert failed hr=0x{:08X}", name, static_cast<std::uint32_t>(result));
					return false;
				}
				image = converted.GetImage(0, 0, 0);
			}
			band.rgba.resize(static_cast<std::size_t>(image->width) * image->height * 4);
			const std::size_t rowBytes = static_cast<std::size_t>(image->width) * 4 * sizeof(float);
			for (std::size_t row = 0; row < image->height; ++row)
				std::memcpy(band.rgba.data() + row * image->width * 4, image->pixels + row * image->rowPitch, rowBytes);
			return true;
		}

		float PreviewValue(float value, BandKind kind)
		{
			switch (kind) {
			case BandKind::kGain: return value * 0.5f;
			case BandKind::kSigned: return 0.5f + value * 4.0f;
			case BandKind::kDepth: return std::pow(std::clamp(1.0f - value, 0.0f, 1.0f), 0.25f);
			default: return value;
			}
		}

		bool WriteBand(const std::filesystem::path& directory, const std::string& stem, const FloatBand& band, BandKind kind,
			std::string& manifest, bool& first)
		{
			const auto rawName = std::format("{}_{}.f32", stem, band.name);
			std::ofstream raw(directory / rawName, std::ios::binary);
			raw.write(reinterpret_cast<const char*>(band.rgba.data()), static_cast<std::streamsize>(band.rgba.size() * sizeof(float)));
			if (!raw) {
				logger::error("[DLSSNR Experiment] Band {} raw write failed", band.name);
				return false;
			}

			DirectX::ScratchImage preview;
			if (FAILED(preview.Initialize2D(DXGI_FORMAT_R32G32B32A32_FLOAT, band.width, band.height, 1, 1)))
				return false;
			const DirectX::Image* previewImage = preview.GetImage(0, 0, 0);
			for (std::uint32_t y = 0; y < band.height; ++y) {
				float* row = reinterpret_cast<float*>(previewImage->pixels + y * previewImage->rowPitch);
				for (std::uint32_t x = 0; x < band.width; ++x) {
					const float* source = band.rgba.data() + (static_cast<std::size_t>(y) * band.width + x) * 4;
					for (int c = 0; c < 3; ++c)
						row[x * 4 + c] = std::clamp(PreviewValue(source[c], kind), 0.0f, 1.0f);
					row[x * 4 + 3] = 1.0f;
				}
			}
			DirectX::ScratchImage eightBit;
			HRESULT result = DirectX::Convert(*previewImage, DXGI_FORMAT_B8G8R8X8_UNORM, DirectX::TEX_FILTER_DEFAULT,
				DirectX::TEX_THRESHOLD_DEFAULT, eightBit);
			if (FAILED(result)) {
				logger::error("[DLSSNR Experiment] Band {} preview convert failed hr=0x{:08X}", band.name, static_cast<std::uint32_t>(result));
				return false;
			}
			const auto bmpName = std::format("{}_{}.bmp", stem, band.name);
			result = DirectX::SaveToWICFile(*eightBit.GetImage(0, 0, 0), DirectX::WIC_FLAGS_NONE,
				DirectX::GetWICCodec(DirectX::WIC_CODEC_BMP), (directory / bmpName).c_str());
			if (FAILED(result)) {
				logger::error("[DLSSNR Experiment] Band {} BMP write failed hr=0x{:08X}", band.name, static_cast<std::uint32_t>(result));
				return false;
			}
			manifest += std::format("{}    \"{}\": {{ \"file\": \"{}\", \"bmp\": \"{}\", \"width\": {}, \"height\": {}, \"dxgiFormat\": {}, \"kind\": {} }}",
				first ? "" : ",\n", band.name, rawName, bmpName, band.width, band.height, band.dxgiFormat, static_cast<int>(kind));
			first = false;
			return true;
		}
	}

	bool SaveBands(std::span<const BandTexture> textures, std::uint32_t engineWidth, std::uint32_t engineHeight)
	{
		std::error_code error;
		std::filesystem::create_directories(kCaptureDirectory, error);
		if (error) {
			logger::error("[DLSSNR Experiment] Could not create capture directory: {}", error.message());
			return false;
		}
		const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
		const auto directory = std::filesystem::path(kCaptureDirectory);
		const auto stem = std::format("bands_{}", frame);
		std::string manifest = std::format("{{\n  \"frameId\": {},\n  \"engineWidth\": {},\n  \"engineHeight\": {},\n  \"layout\": \"rgba float32 row-major\",\n  \"kinds\": \"0 color, 1 gain, 2 signed, 3 depth\",\n  \"textures\": {{\n",
			frame, engineWidth, engineHeight);
		bool first = true;
		const FloatBand* input = nullptr;
		const FloatBand* output = nullptr;
		std::vector<FloatBand> bands;
		bands.reserve(textures.size());
		for (const auto& texture : textures) {
			if (!texture.resource)
				continue;
			FloatBand band;
			if (!CaptureFloatBand(texture.name, texture.resource, texture.kind, band) ||
				!WriteBand(directory, stem, band, texture.kind, manifest, first))
				return false;
			bands.push_back(std::move(band));
			if (bands.back().name == "I")
				input = &bands.back();
			else if (bands.back().name == "N")
				output = &bands.back();
		}
		if (input && output && input->width == output->width && input->height == output->height) {
			FloatBand residual;
			residual.name = "Residual";
			residual.width = input->width;
			residual.height = input->height;
			residual.dxgiFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R32G32B32A32_FLOAT);
			residual.rgba.resize(input->rgba.size());
			for (std::size_t index = 0; index < residual.rgba.size(); ++index)
				residual.rgba[index] = (index % 4 == 3) ? 1.0f : output->rgba[index] - input->rgba[index];
			if (!WriteBand(directory, stem, residual, BandKind::kSigned, manifest, first))
				return false;
		}
		manifest += "\n  }\n}\n";
		std::ofstream manifestFile(directory / std::format("{}.json", stem));
		manifestFile << manifest;
		if (!manifestFile) {
			logger::error("[DLSSNR Experiment] Band manifest write failed");
			return false;
		}
		logger::info("[DLSSNR Experiment] Saved band capture {} to {}", stem, directory.string());
		return true;
	}

	void Reset()
	{
		pendingCapture.reset();
		lastPollFrame = UINT32_MAX;
	}
}