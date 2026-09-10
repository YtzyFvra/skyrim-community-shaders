#!/usr/bin/env python3
"""Offline band analysis for a Guidance band capture (Export bands button).

Reads the bands_<frame>.json manifest written by ExperimentCapture::SaveBands, brings
G / I / N / RC / Lo / D onto one grid, and reports where each picture carries its energy:

  * per-band RMS of luma for I, N, G, the NR edit E = N - I and the G detail G - I
  * radial power spectrum (log-log) of I, N, G
  * "erase" pairs: N with only its low-band edit kept, N with only its high-band edit kept,
    each next to the real N, so the eye can see what each band of NR's edit buys
  * protocol 6.5 (needs G / Lo / D, and RC + DepthR when present):
      detail-band recovery ||D-Lo||_[N,D] / ||G-Lo||_[N,D]
      RD-normal 10-90% edge width of G / RC / D
      low-frequency fidelity ||D-RC||_[0,N]

Usage:
  python tools/nr_band_analysis.py <bands_123.json> [--grid N|G|D] [--split-sigma 2.0]
                                   [--sigmas 1,2,4,8,16] [--crop x,y,w,h] [--out DIR]

Requires numpy, Pillow, scipy. matplotlib is optional (spectrum / bar plots).
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def load_manifest(path: Path) -> tuple[dict, dict[str, np.ndarray]]:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    images: dict[str, np.ndarray] = {}
    for name, entry in manifest["textures"].items():
        raw = np.fromfile(path.parent / entry["file"], dtype=np.float32)
        images[name] = raw.reshape(entry["height"], entry["width"], 4)[:, :, :3].copy()
    return manifest, images


def resample(image: np.ndarray, size: tuple[int, int]) -> np.ndarray:
    """Area filter when shrinking, Lanczos when enlarging; matches the DS/UD roles in the pipeline."""
    height, width = image.shape[:2]
    target_w, target_h = size
    if (width, height) == (target_w, target_h):
        return image
    shrinking = target_w < width or target_h < height
    method = Image.BOX if shrinking else Image.LANCZOS
    channels = [Image.fromarray(image[:, :, c], mode="F").resize((target_w, target_h), method) for c in range(3)]
    return np.stack([np.asarray(c, dtype=np.float32) for c in channels], axis=-1)


def luma(image: np.ndarray) -> np.ndarray:
    return image @ LUMA


def band_energies(signal: np.ndarray, sigmas: list[float]) -> list[float]:
    """RMS of successive Gaussian differences; the last entry is the residual low pass."""
    energies = []
    previous = signal
    for sigma in sigmas:
        blurred = ndimage.gaussian_filter(signal, sigma)
        energies.append(float(np.sqrt(np.mean((previous - blurred) ** 2))))
        previous = blurred
    energies.append(float(np.sqrt(np.mean(previous ** 2))))
    return energies


def radial_spectrum(signal: np.ndarray, bins: int = 64) -> tuple[np.ndarray, np.ndarray]:
    windowed = signal - signal.mean()
    spectrum = np.abs(np.fft.fftshift(np.fft.fft2(windowed))) ** 2
    height, width = spectrum.shape
    yy, xx = np.indices(spectrum.shape)
    radius = np.sqrt((xx - width / 2) ** 2 + (yy - height / 2) ** 2)
    normalized = radius / (min(width, height) / 2)
    edges = np.linspace(0.0, 1.0, bins + 1)
    power = np.zeros(bins)
    for index in range(bins):
        mask = (normalized >= edges[index]) & (normalized < edges[index + 1])
        power[index] = spectrum[mask].mean() if mask.any() else np.nan
    centers = (edges[:-1] + edges[1:]) * 0.5
    return centers, power


def to_uint8(image: np.ndarray) -> np.ndarray:
    return (np.clip(image, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def rms(signal: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(signal))))


def highpass_above_n(signal: np.ndarray, sigma_n: float) -> np.ndarray:
    """[N, D] band: remove content at and below the N pixel pitch."""
    return signal - ndimage.gaussian_filter(signal, sigma_n)


def lowpass_to_n(signal: np.ndarray, sigma_n: float) -> np.ndarray:
    """[0, N] band."""
    return ndimage.gaussian_filter(signal, sigma_n)


def rise_width_px(profile: np.ndarray) -> float | None:
    """10%–90% rise of a 1D step, using the plateau means at each end."""
    if profile.size < 5:
        return None
    smoothed = ndimage.gaussian_filter1d(profile.astype(np.float64), 0.7)
    pad = max(profile.size // 5, 1)
    left = float(smoothed[:pad].mean())
    right = float(smoothed[-pad:].mean())
    delta = right - left
    if abs(delta) < 1e-4:
        return None
    t10 = left + 0.10 * delta
    t90 = left + 0.90 * delta
    if delta > 0:
        i10 = int(np.argmax(smoothed >= t10))
        i90 = int(np.argmax(smoothed >= t90))
        if smoothed[0] < t90 and i90 == 0:
            return None
    else:
        i10 = int(np.argmax(smoothed <= t10))
        i90 = int(np.argmax(smoothed <= t90))
        if smoothed[0] > t90 and i90 == 0:
            return None
    return float(abs(i90 - i10))


def summarize(values: list[float]) -> dict[str, float]:
    if not values:
        return {"count": 0.0, "p10": math.nan, "median": math.nan, "p90": math.nan}
    array = np.asarray(values, dtype=np.float64)
    return {
        "count": float(array.size),
        "p10": float(np.percentile(array, 10)),
        "median": float(np.percentile(array, 50)),
        "p90": float(np.percentile(array, 90)),
    }


def rd_cliffs(depth: np.ndarray, threshold: float = 0.08) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Cliff mask and unit normals. Inverse-raw is a camera-less linear proxy."""
    linear = 1.0 / np.maximum(depth, 1e-5)
    gx = np.zeros_like(linear)
    gy = np.zeros_like(linear)
    gx[:, 1:] = (linear[:, 1:] - linear[:, :-1]) / np.maximum(np.minimum(linear[:, 1:], linear[:, :-1]), 1.0)
    gy[1:, :] = (linear[1:, :] - linear[:-1, :]) / np.maximum(np.minimum(linear[1:, :], linear[:-1, :]), 1.0)
    cliff = (np.abs(gx) > threshold) | (np.abs(gy) > threshold)
    nx = ndimage.sobel(linear, axis=1)
    ny = ndimage.sobel(linear, axis=0)
    length = np.hypot(nx, ny)
    nx = nx / np.maximum(length, 1e-8)
    ny = ny / np.maximum(length, 1e-8)
    return cliff, nx, ny


def protocol_65(images: dict[str, np.ndarray], out: Path) -> dict:
    """Write and return the 6.5 recovery / edge-width / atmosphere numbers on the D grid."""
    needed = [name for name in ("G", "Lo", "D") if name not in images]
    if needed:
        print(f"6.5 skipped (need G, Lo, D; missing {needed})")
        return {}

    n_source = images["N"] if "N" in images else images["I"]
    d_h, d_w = images["D"].shape[:2]
    n_h, n_w = n_source.shape[:2]
    scale = max(d_w / max(n_w, 1), d_h / max(n_h, 1))
    sigma_n = max(scale / 2.35, 0.6)
    size = (d_w, d_h)
    guide = resample(images["G"], size)
    low = resample(images["Lo"], size)
    result = images["D"] if images["D"].shape[:2] == (d_h, d_w) else resample(images["D"], size)
    recon = resample(images["RC"], size) if "RC" in images else None

    detail_d = luma(result - low)
    detail_g = luma(guide - low)
    band_d = highpass_above_n(detail_d, sigma_n)
    band_g = highpass_above_n(detail_g, sigma_n)
    recovery = rms(band_d) / max(rms(band_g), 1e-9)

    energy_g = ndimage.uniform_filter(detail_g ** 2, 5)
    textured = energy_g >= np.percentile(energy_g, 75)
    metrics: dict = {
        "sigma_n_px": sigma_n,
        "scale_d_over_n": scale,
        "detail_recovery": recovery,
        "detail_recovery_textured": rms(band_d[textured]) / max(rms(band_g[textured]), 1e-9),
    }
    if "N" in images and "I" in images:
        edit = luma(resample(images["N"], size) - resample(images["I"], size))
        energy_e = ndimage.uniform_filter(edit ** 2, 5)
        flattened = textured & (energy_e <= np.percentile(energy_e, 40))
        if flattened.any():
            metrics["detail_recovery_flattened"] = rms(band_d[flattened]) / max(rms(band_g[flattened]), 1e-9)
            metrics["flattened_pixels"] = int(flattened.sum())

    if recon is not None:
        atmosphere = rms(lowpass_to_n(luma(result - recon), sigma_n))
        metrics["atmosphere_rms_d_minus_rc_low"] = atmosphere

    if "DepthR" in images and recon is not None:
        depth_r = images["DepthR"][:, :, 0]
        depth = np.asarray(
            Image.fromarray(depth_r, mode="F").resize(size, Image.BILINEAR), dtype=np.float32)
        cliff, nx, ny = rd_cliffs(depth)
        ys, xs = np.nonzero(cliff)
        if ys.size > 2000:
            pick = np.linspace(0, ys.size - 1, 2000).astype(np.int64)
            ys, xs = ys[pick], xs[pick]
        radius = int(max(round(2.0 * scale), 6))
        widths = {"G": [], "RC": [], "D": []}
        signals = {"G": luma(guide), "RC": luma(recon), "D": luma(result)}
        for y, x in zip(ys, xs):
            step_y, step_x = float(ny[y, x]), float(nx[y, x])
            if step_y * step_y + step_x * step_x < 1e-6:
                continue
            ts = np.arange(-radius, radius + 1, dtype=np.float64)
            coords = np.vstack([np.clip(y + ts * step_y, 0, d_h - 1), np.clip(x + ts * step_x, 0, d_w - 1)])
            for name, signal in signals.items():
                width = rise_width_px(ndimage.map_coordinates(signal, coords, order=1, mode="nearest"))
                if width is not None:
                    widths[name].append(width)
        metrics["edge_width_px"] = {name: summarize(values) for name, values in widths.items()}
        metrics["rd_cliff_pixels"] = int(cliff.sum())

    (out / "protocol65.json").write_text(json.dumps(metrics, indent=2), encoding="utf-8")
    print()
    print("6.5 protocol (D grid)")
    print(f"  detail recovery ||D-Lo||_[N,D] / ||G-Lo||_[N,D] = {metrics['detail_recovery']:.4f}")
    print(f"    textured (top 25% |G-Lo|) = {metrics['detail_recovery_textured']:.4f}")
    if "detail_recovery_flattened" in metrics:
        print(f"    NR-flattened texture = {metrics['detail_recovery_flattened']:.4f}  (should sit near a)")
    if "atmosphere_rms_d_minus_rc_low" in metrics:
        print(f"  atmosphere ||D-RC||_[0,N] RMS = {metrics['atmosphere_rms_d_minus_rc_low']:.6f}  (want ~0)")
    if "edge_width_px" in metrics:
        print("  RD-normal 10-90% width (D px):")
        for name, stats in metrics["edge_width_px"].items():
            print(f"    {name:2s}  median {stats['median']:.2f}  p10 {stats['p10']:.2f}  p90 {stats['p90']:.2f}  n={int(stats['count'])}")
        print("    D should sit next to G, not RC")
    return metrics


def save_pair(left: np.ndarray, right: np.ndarray, path: Path, crop: tuple[int, int, int, int] | None) -> None:
    if crop is not None:
        x, y, w, h = crop
        left = left[y:y + h, x:x + w]
        right = right[y:y + h, x:x + w]
    divider = np.ones((left.shape[0], 4, 3), dtype=np.float32)
    Image.fromarray(to_uint8(np.concatenate([left, divider, right], axis=1))).save(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--grid", choices=["N", "G", "D"], default="N",
                        help="which texture's size every picture is brought to (default N)")
    parser.add_argument("--sigmas", default="1,2,4,8,16", help="Gaussian sigmas (grid pixels) for the band split")
    parser.add_argument("--split-sigma", type=float, default=2.0,
                        help="sigma separating NR's low-band edit from its high-band edit for the erase pairs")
    parser.add_argument("--crop", default=None, help="x,y,w,h crop (grid pixels) for the pair images")
    parser.add_argument("--out", type=Path, default=None, help="output directory (default beside the manifest)")
    args = parser.parse_args()

    manifest, images = load_manifest(args.manifest)
    missing = [name for name in ("G", "I", "N") if name not in images]
    if missing:
        print(f"manifest lacks {missing}", file=sys.stderr)
        return 1

    grid_source = images[args.grid] if args.grid in images else images["N"]
    size = (grid_source.shape[1], grid_source.shape[0])
    on_grid = {name: resample(image, size) for name, image in images.items()}
    out = args.out or args.manifest.parent / f"{args.manifest.stem}_analysis_{args.grid}"
    out.mkdir(parents=True, exist_ok=True)
    crop = tuple(int(v) for v in args.crop.split(",")) if args.crop else None
    sigmas = [float(s) for s in args.sigmas.split(",")]

    I, N, G = on_grid["I"], on_grid["N"], on_grid["G"]
    edit = N - I
    g_detail = G - I
    signals = {
        "I (NR input)": luma(I),
        "N (NR output)": luma(N),
        "G (guidance)": luma(G),
        "E = N - I (NR edit)": luma(edit),
        "G - I (G detail above N band)": luma(g_detail),
    }
    if "D" in on_grid:
        signals["D (guided result)"] = luma(on_grid["D"])
    if "RC" in on_grid:
        signals["RC (ACT2 UD of N)"] = luma(on_grid["RC"])

    band_labels = [f"{(0.0 if index == 0 else sigmas[index - 1]):g}-{sigma:g}px" for index, sigma in enumerate(sigmas)]
    band_labels.append(f">{sigmas[-1]:g}px (low)")
    rows = []
    print(f"grid {args.grid} {size[0]}x{size[1]}  frame {manifest.get('frameId')}")
    print(f"{'signal':32s} " + " ".join(f"{label:>12s}" for label in band_labels))
    for name, signal in signals.items():
        energies = band_energies(signal, sigmas)
        rows.append([name, *energies])
        print(f"{name:32s} " + " ".join(f"{value:12.5f}" for value in energies))

    # Ratios that answer the two questions directly.
    edit_bands = band_energies(signals["E = N - I (NR edit)"], sigmas)
    n_bands = band_energies(signals["N (NR output)"], sigmas)
    detail_bands = band_energies(signals["G - I (G detail above N band)"], sigmas)
    print()
    print("NR edit as a fraction of N per band (how much of each band NR authored):")
    print("  " + " ".join(f"{label}:{(e / max(n, 1e-9)):.2f}" for label, e, n in zip(band_labels, edit_bands, n_bands)))
    print("G detail vs NR edit per band (>1: G has more to say there than NR did):")
    print("  " + " ".join(f"{label}:{(g / max(e, 1e-9)):.2f}" for label, g, e in zip(band_labels, detail_bands, edit_bands)))

    with (out / "band_energy.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["signal", *band_labels])
        writer.writerows(rows)

    # Erase pairs: what N looks like with only one band of its own edit kept.
    edit_low = np.stack([ndimage.gaussian_filter(edit[:, :, c], args.split_sigma) for c in range(3)], axis=-1)
    edit_high = edit - edit_low
    pairs = {
        "I_vs_N": (I, N),
        "N_lowEditOnly_vs_N": (I + edit_low, N),
        "N_highEditOnly_vs_N": (I + edit_high, N),
        "G_vs_I": (G, I),
        "edit_low_x4": (np.clip(0.5 + edit_low * 4.0, 0, 1), np.clip(0.5 + edit_high * 8.0, 0, 1)),
    }
    if "D" in on_grid:
        pairs["N_vs_D"] = (N, on_grid["D"])
    for name, (left, right) in pairs.items():
        save_pair(left, right, out / f"{name}.png", crop)

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        protocol_65(images, out)
        print(f"matplotlib missing; wrote CSV and pairs to {out}")
        return 0

    figure, axes = plt.subplots(1, 2, figsize=(14, 5))
    for name in ("I (NR input)", "N (NR output)", "G (guidance)"):
        centers, power = radial_spectrum(signals[name])
        axes[0].plot(centers, np.log10(power + 1e-12), label=name)
    axes[0].set_title("radial power spectrum (luma)")
    axes[0].set_xlabel("normalized frequency (1 = grid Nyquist)")
    axes[0].set_ylabel("log10 power")
    axes[0].legend()

    x = np.arange(len(band_labels))
    width = 0.8 / len(rows)
    for index, row in enumerate(rows):
        axes[1].bar(x + index * width, row[1:], width, label=row[0])
    axes[1].set_xticks(x + 0.4)
    axes[1].set_xticklabels(band_labels, rotation=30)
    axes[1].set_title("per-band RMS of luma")
    axes[1].legend(fontsize=7)
    figure.tight_layout()
    figure.savefig(out / "bands.png", dpi=120)
    protocol_65(images, out)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
