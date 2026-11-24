#!/usr/bin/env python3
"""Offline multi-scale normal (_tl2n.dds) and material/cavity (_tl2s.dds) generator for Torchlight II.

Produces Ogre 1.7 + D3D11 compatible -dx9 linear DDS maps:
  - <stem>_tl2n.dds : BC3_UNORM (DXT5nm: A = nx, G = ny; R=G=B=ny for clean BC3 green precision)
  - <stem>_tl2s.dds : BC1_UNORM (DXT1: R = Smoothness, G = Metalness, B = CavityAO)
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path
from typing import Iterable

import numpy as np
from PIL import Image

SKIP_NAME_SUBSTRS = (
    "_GLOW",
    "_ILLUM",
    "_SPEC",
    "_MASK",
    "_SHADOW",
    "_FOG",
    "_ALPHATOP",
    "_TL2N",
    "_TL2S",
    "_NORM",
    "_NRM",
)

SKIP_EXACT_STEMS = {
    "BLACK",
    "WHITE",
    "GRADIENT",
    "ALPHATOP",
    "FOGBANK",
    "FOGBANKMASK",
    "NOISE",
}

SKIP_PATH_PARTS = (
    "/UI/",
    "/PARTICLES/",
    "/TOWNSPLASH/",
    "/PAPERDOLL/",
    "/CURSORS/",
    "/FONTS/",
    "/ICONS/",
    "/SKY/",
)


def is_eligible_albedo(rel_posix: str) -> bool:
    upper = rel_posix.upper()
    if not upper.endswith(".PNG"):
        return False
    for part in SKIP_PATH_PARTS:
        if part in upper:
            return False
    stem = Path(upper).stem
    if stem in SKIP_EXACT_STEMS:
        return False
    if stem.startswith("ALPHA_") or stem == "ALPHA":
        return False
    for tag in SKIP_NAME_SUBSTRS:
        if tag in stem:
            return False
    return True


def gaussian_kernel_1d(sigma: float) -> np.ndarray:
    if sigma <= 0.15:
        return np.array([1.0], dtype=np.float32)
    radius = max(1, int(np.ceil(sigma * 2.8)))
    x = np.arange(-radius, radius + 1, dtype=np.float32)
    k = np.exp(-0.5 * (x / sigma) ** 2)
    k /= k.sum()
    return k


def blur_1d_separable(arr: np.ndarray, sigma: float, wrap: bool) -> np.ndarray:
    if sigma <= 0.15:
        return arr
    # Fast pyramid blur for wide Gaussians (sigma > 3.5): downsample, blur at smaller sigma, bilinearly upsample.
    if sigma > 3.5 and min(arr.shape[0], arr.shape[1]) >= 64:
        factor = 4 if (sigma > 10.0 and min(arr.shape[0], arr.shape[1]) >= 128) else 2
        h, w = arr.shape[:2]
        dw, dh = max(8, w // factor), max(8, h // factor)
        small = np.asarray(
            Image.fromarray(arr, mode="F").resize((dw, dh), Image.Resampling.BILINEAR),
            dtype=np.float32,
        )
        blurred_small = blur_1d_separable(small, sigma / float(factor), wrap=wrap)
        return np.asarray(
            Image.fromarray(blurred_small, mode="F").resize((w, h), Image.Resampling.BILINEAR),
            dtype=np.float32,
        )

    k = gaussian_kernel_1d(sigma)
    r = len(k) // 2
    if r == 0:
        return arr
    pad_mode = "wrap" if wrap else "edge"

    px = np.pad(arr, ((0, 0), (r, r)), mode=pad_mode)
    tmp = np.zeros_like(arr, dtype=np.float32)
    for i, w_val in enumerate(k):
        tmp += w_val * px[:, i : i + arr.shape[1]]

    py = np.pad(tmp, ((r, r), (0, 0)), mode=pad_mode)
    out = np.zeros_like(arr, dtype=np.float32)
    for i, w_val in enumerate(k):
        out += w_val * py[i : i + arr.shape[0], :]
    return out


def bleed_transparent_pixels(rgb: np.ndarray, alpha: np.ndarray) -> np.ndarray:
    """Prevent hard background color bleeding across alpha-cutout edges."""
    if alpha is None:
        return rgb
    opaque = alpha >= 0.45
    if np.all(opaque) or not np.any(opaque):
        return rgb
    work = rgb.copy()
    mask = opaque.astype(np.float32)
    for _ in range(3):
        sum_rgb = np.zeros_like(work)
        sum_w = np.zeros_like(mask)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            sr = np.roll(work * mask[..., None], shift=(dy, dx), axis=(0, 1))
            sw = np.roll(mask, shift=(dy, dx), axis=(0, 1))
            sum_rgb += sr
            sum_w += sw
        fill = (~opaque) & (sum_w > 0.01)
        if not np.any(fill):
            break
        work[fill] = sum_rgb[fill] / sum_w[fill, None]
        mask[fill] = 1.0
        opaque = opaque | fill
    return work


def smoothstep(e0: float, e1: float, x: np.ndarray) -> np.ndarray:
    t = np.clip((x - e0) / max(e1 - e0, 1e-6), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def scharr_grad(arr: np.ndarray, wrap: bool) -> tuple[np.ndarray, np.ndarray]:
    pad_mode = "wrap" if wrap else "edge"
    p = np.pad(arr, ((1, 1), (1, 1)), mode=pad_mode)
    gx = (
        3.0 * (p[:-2, 2:] - p[:-2, :-2])
        + 10.0 * (p[1:-1, 2:] - p[1:-1, :-2])
        + 3.0 * (p[2:, 2:] - p[2:, :-2])
    ) / 32.0
    gy = (
        3.0 * (p[2:, :-2] - p[:-2, :-2])
        + 10.0 * (p[2:, 1:-1] - p[:-2, 1:-1])
        + 3.0 * (p[2:, 2:] - p[:-2, 2:])
    ) / 32.0
    return gx, gy


def compute_target_size(w: int, h: int, max_dim: int, half_large: bool) -> tuple[int, int]:
    if half_large and max(w, h) >= 1024:
        tw, th = max(4, w // 2), max(4, h // 2)
    else:
        tw, th = w, h
    if max(tw, th) > max_dim:
        scale = max_dim / float(max(tw, th))
        tw = max(4, int(round(tw * scale)))
        th = max(4, int(round(th * scale)))
    # Align to DXT 4x4 block size
    tw = max(4, (tw + 3) // 4 * 4)
    th = max(4, (th + 3) // 4 * 4)
    return tw, th


def synthesize_maps(
    src_path: Path,
    rel_posix: str,
    max_norm_dim: int = 1024,
    max_spec_dim: int = 512,
    half_large: bool = True,
) -> tuple[Image.Image, Image.Image, Image.Image | None]:
    """Generate (dxt5nm_rgba_img, spec_rgb_img, preview_xyz_normal_img)."""
    with Image.open(src_path) as im:
        has_alpha = im.mode in ("RGBA", "LA", "PA") or (
            im.mode == "P" and "transparency" in im.info
        )
        orig_w, orig_h = im.size
        nw, nh = compute_target_size(orig_w, orig_h, max_norm_dim, half_large=half_large)
        sw, sh = compute_target_size(orig_w, orig_h, max_spec_dim, half_large=True)
        work_im = im.convert("RGBA" if has_alpha else "RGB")
        if (orig_w, orig_h) != (nw, nh):
            work_im = work_im.resize((nw, nh), Image.Resampling.BILINEAR)
        rgba = np.asarray(work_im, dtype=np.float32) / np.float32(255.0)

    if has_alpha:
        rgb = bleed_transparent_pixels(rgba[..., :3], rgba[..., 3])
        alpha_mask = rgba[..., 3]
    else:
        rgb = rgba
        alpha_mask = None

    h, w = rgb.shape[:2]
    upper_rel = rel_posix.upper()
    is_world_tile = ("LEVELSETS/" in upper_rel) and ("/PROPS/" not in upper_rel)

    # Luminance + gentle chromatic boundary term so painted brass/stone borders carve crisp seams
    luma = 0.299 * rgb[..., 0] + 0.587 * rgb[..., 1] + 0.114 * rgb[..., 2]
    scale = float(np.clip(min(w, h) / 1024.0, 0.5, 1.8))

    g_fine = blur_1d_separable(luma, 1.0 * scale, wrap=is_world_tile)
    g_meso = blur_1d_separable(luma, 3.0 * scale, wrap=is_world_tile)
    g_macro = blur_1d_separable(luma, 9.0 * scale, wrap=is_world_tile)
    g_broad = blur_1d_separable(luma, 22.0 * scale, wrap=is_world_tile)

    hf_fine = luma - g_fine
    hf_meso = g_fine - g_meso
    hf_macro = g_meso - g_macro
    hf_broad = g_macro - g_broad

    # Combine octaves into balanced height field; preserve dark crevices while taming bright glare spikes
    height = 0.28 * hf_fine + 0.45 * hf_meso + 0.58 * hf_macro + 0.24 * hf_broad
    height = np.where(height < 0.0, height * np.float32(1.18), np.tanh(height * np.float32(2.2)) / np.float32(2.2))

    gx, gy = scharr_grad(height, wrap=is_world_tile)
    strength = 10.5 * ((max(w, h) / 1024.0) ** 0.30)
    raw_nx = -gx * strength
    raw_ny = -gy * strength
    grad_mag = np.sqrt(raw_nx * raw_nx + raw_ny * raw_ny) + 1e-8
    capped_mag = 1.65 * np.tanh(grad_mag / 1.65)
    slope_gain = capped_mag / grad_mag
    nx = raw_nx * slope_gain
    ny = raw_ny * slope_gain
    nz = np.ones_like(nx, dtype=np.float32)

    if alpha_mask is not None:
        # Soften normal perturbation on thin alpha fringes to avoid silhouette ringing
        edge_weight = smoothstep(0.25, 0.85, alpha_mask)
        nx *= edge_weight
        ny *= edge_weight

    norm_len = np.sqrt(nx * nx + ny * ny + nz * nz) + 1e-8
    nx /= norm_len
    ny /= norm_len
    nz /= norm_len

    # Re-normalize & pack DXT5nm directly (input is already at target normal resolution nw x nh)
    xyz_u8 = np.stack(
        [
            np.clip(nx * 0.5 + 0.5, 0.0, 1.0) * 255.0,
            np.clip(ny * 0.5 + 0.5, 0.0, 1.0) * 255.0,
            np.clip(nz * 0.5 + 0.5, 0.0, 1.0) * 255.0,
        ],
        axis=-1,
    ).astype(np.uint8)
    xyz_img = Image.fromarray(xyz_u8, mode="RGB")

    a_u8 = (np.clip(nx * 0.5 + 0.5, 0.0, 1.0) * 255.0).astype(np.uint8)
    g_u8 = (np.clip(ny * 0.5 + 0.5, 0.0, 1.0) * 255.0).astype(np.uint8)
    # DXT5nm: store Y in RGB (R=G=B=ny -> zero cross-channel BC3 endpoint conflict) and X in Alpha
    dxt5nm_arr = np.stack([g_u8, g_u8, g_u8, a_u8], axis=-1)
    dxt5nm_img = Image.fromarray(dxt5nm_arr, mode="RGBA")

    # Specular / Roughness / Cavity map (_tl2s)
    micro_var = blur_1d_separable(np.abs(luma - g_fine), max(1.2, 2.0 * scale), wrap=is_world_tile)
    maxc = np.max(rgb, axis=-1)
    minc = np.min(rgb, axis=-1)
    sat = np.where(maxc > 1e-4, (maxc - minc) / np.maximum(maxc, 1e-4), 0.0)

    # Neutral steel/iron + bright metallic gold/brass heuristic (avoid tagging warm brown stone as metal)
    steel_mask = (1.0 - smoothstep(0.08, 0.22, sat)) * smoothstep(0.22, 0.38, luma) * (1.0 - smoothstep(0.75, 0.90, luma))
    warm_gold = smoothstep(0.14, 0.28, rgb[..., 0] - rgb[..., 2]) * smoothstep(0.03, 0.12, rgb[..., 0] - rgb[..., 1])
    gold_mask = warm_gold * smoothstep(0.32, 0.50, luma) * smoothstep(0.25, 0.45, sat) * (1.0 - smoothstep(0.72, 0.90, sat))
    metalness = np.clip(np.maximum(steel_mask, gold_mask * 0.85) * (1.0 - smoothstep(0.028, 0.095, micro_var)), 0.0, 1.0)

    # Calibrated smoothness: keep non-metallic painted stone/wood/cloth satin-matte (0.14..0.40), boost worn highlights & metals
    base_smooth = (1.0 - np.clip(micro_var * 10.0, 0.0, 0.88)) * smoothstep(0.07, 0.36, luma)
    bevel_sheen = smoothstep(0.02, 0.14, np.maximum(hf_meso + hf_macro, 0.0))
    smoothness = np.clip(0.14 + 0.24 * base_smooth + 0.14 * bevel_sheen + 0.40 * metalness, 0.05, 0.92)
    smoothness *= smoothstep(0.05, 0.20, luma)

    valley = hf_meso * 0.55 + hf_macro * 0.85
    cavity = np.clip(0.92 + valley * 2.5, 0.18, 1.0)

    spec_u8 = np.stack(
        [
            np.clip(smoothness * 255.0, 0.0, 255.0),
            np.clip(metalness * 255.0, 0.0, 255.0),
            np.clip(cavity * 255.0, 0.0, 255.0),
        ],
        axis=-1,
    ).astype(np.uint8)
    spec_img = Image.fromarray(spec_u8, mode="RGB")
    if (sw, sh) != (w, h):
        spec_img = spec_img.resize((sw, sh), Image.Resampling.BILINEAR)

    return dxt5nm_img, spec_img, xyz_img


def run_texconv_batch(texconv: Path, src_pngs: list[Path], out_dir: Path, fmt: str) -> None:
    if not src_pngs:
        return
    flist = out_dir / f"flist_{fmt}.txt"
    flist.write_text("\n".join(str(p) for p in src_pngs) + "\n", encoding="utf-8")
    cmd = [
        str(texconv),
        "-nologo",
        "-y",
        "-dx9",
        "-ft",
        "dds",
        "-f",
        fmt,
        "-m",
        "0",
        "-o",
        str(out_dir),
        "-flist",
        str(flist),
    ]
    res = subprocess.run(cmd, capture_output=True)
    if res.returncode != 0:
        err_msg = (res.stderr or res.stdout or b"").decode("utf-8", errors="ignore")
        raise RuntimeError(f"texconv batch failed ({fmt}): {err_msg}")


def collect_sources(src_roots: Iterable[Path]) -> list[tuple[Path, str]]:
    seen: set[str] = set()
    items: list[tuple[Path, str]] = []
    for root in src_roots:
        for dp, _, fns in os.walk(root):
            for fn in fns:
                p = Path(dp) / fn
                rel = p.relative_to(root).as_posix()
                if not is_eligible_albedo(rel):
                    continue
                key = rel.upper()
                if key in seen:
                    continue
                seen.add(key)
                items.append((p, rel))
    items.sort(key=lambda x: x[1].upper())
    return items


def process_batch(
    batch: list[tuple[Path, str]],
    out_root: Path,
    texconv: Path,
    max_norm_dim: int,
    max_spec_dim: int,
    preview_dir: Path | None,
    skip_existing: bool,
) -> tuple[int, int, int, list[tuple[str, str]]]:
    done = 0
    total_n = 0
    total_s = 0
    errors: list[tuple[str, str]] = []
    pending: list[tuple[int, str, Path, Path]] = []

    with tempfile.TemporaryDirectory(prefix="tl2ns_") as tmp:
        tmp_dir = Path(tmp)
        n_pngs: list[Path] = []
        s_pngs: list[Path] = []

        for idx, (src_path, rel_posix) in enumerate(batch):
            rel_no_ext = str(Path(rel_posix).with_suffix("")).replace("\\", "/")
            norm_dst = out_root / f"{rel_no_ext}_tl2n.dds"
            spec_dst = out_root / f"{rel_no_ext}_tl2s.dds"

            if skip_existing and norm_dst.exists() and spec_dst.exists():
                done += 1
                total_n += norm_dst.stat().st_size
                total_s += spec_dst.stat().st_size
                continue

            try:
                dxt5nm_img, spec_img, preview_img = synthesize_maps(
                    src_path, rel_posix, max_norm_dim=max_norm_dim, max_spec_dim=max_spec_dim
                )
                if preview_dir is not None and preview_img is not None:
                    preview_dir.mkdir(parents=True, exist_ok=True)
                    safe_tag = rel_no_ext.replace("/", "__")
                    preview_img.save(preview_dir / f"{safe_tag}_norm_preview.png")
                    spec_img.save(preview_dir / f"{safe_tag}_spec_preview.png")

                n_png = tmp_dir / f"n_{idx}.png"
                s_png = tmp_dir / f"s_{idx}.png"
                dxt5nm_img.save(n_png, compress_level=0)
                spec_img.save(s_png, compress_level=0)
                n_pngs.append(n_png)
                s_pngs.append(s_png)
                pending.append((idx, rel_posix, norm_dst, spec_dst))
            except Exception as ex:
                done += 1
                errors.append((rel_posix, str(ex)))

        if pending:
            try:
                run_texconv_batch(texconv, n_pngs, tmp_dir, "BC3_UNORM")
                run_texconv_batch(texconv, s_pngs, tmp_dir, "BC1_UNORM")
                for idx, rel_posix, norm_dst, spec_dst in pending:
                    n_dds = tmp_dir / f"n_{idx}.dds"
                    s_dds = tmp_dir / f"s_{idx}.dds"
                    if not n_dds.exists() or not s_dds.exists():
                        errors.append((rel_posix, "Missing batched texconv output"))
                        done += 1
                        continue
                    norm_dst.parent.mkdir(parents=True, exist_ok=True)
                    shutil.move(str(n_dds), str(norm_dst))
                    shutil.move(str(s_dds), str(spec_dst))
                    total_n += norm_dst.stat().st_size
                    total_s += spec_dst.stat().st_size
                    done += 1
            except Exception as ex:
                for _, rel_posix, _, _ in pending:
                    errors.append((rel_posix, str(ex)))
                    done += 1

    return done, total_n, total_s, errors


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", action="append", required=True, help="Source PNG root directory")
    ap.add_argument("--out", required=True, help="Output MOD directory (contains MEDIA/)")
    ap.add_argument("--texconv", default="texconv.exe", help="Path to texconv.exe")
    ap.add_argument("--filter", default="", help="Only process paths containing substring")
    ap.add_argument("--limit", type=int, default=0, help="Process at most N textures")
    ap.add_argument("--workers", type=int, default=min(12, os.cpu_count() or 4))
    ap.add_argument("--batch-size", type=int, default=16)
    ap.add_argument("--max-norm-dim", type=int, default=1024)
    ap.add_argument("--max-spec-dim", type=int, default=512)
    ap.add_argument("--preview-dir", default="", help="Optional directory to save readable PNG previews")
    ap.add_argument("--no-skip", action="store_true", help="Overwrite existing output DDS files")
    args = ap.parse_args()

    texconv = Path(args.texconv).resolve()
    if not texconv.exists():
        print(f"ERROR: texconv not found: {texconv}", file=sys.stderr)
        return 2

    src_roots = [Path(s).resolve() for s in args.src]
    items = collect_sources(src_roots)
    if args.filter:
        f_up = args.filter.upper()
        items = [it for it in items if f_up in it[1].upper()]
    if args.limit > 0:
        items = items[: args.limit]

    print(f"Selected {len(items)} eligible textures (max_norm={args.max_norm_dim}, max_spec={args.max_spec_dim}, workers={args.workers})")
    if not items:
        return 0

    out_root = Path(args.out).resolve()
    preview_dir = Path(args.preview_dir).resolve() if args.preview_dir else None
    t0 = time.time()
    total_norm_bytes = 0
    total_spec_bytes = 0
    done = 0
    errors: list[tuple[str, str]] = []

    bs = max(1, args.batch_size)
    batches = [items[i : i + bs] for i in range(0, len(items), bs)]

    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        futs = [
            pool.submit(
                process_batch,
                b,
                out_root,
                texconv,
                args.max_norm_dim,
                args.max_spec_dim,
                preview_dir,
                not args.no_skip,
            )
            for b in batches
        ]
        last_report = 0
        for fut in as_completed(futs):
            b_done, nb, sb, b_errs = fut.result()
            done += b_done
            total_norm_bytes += nb
            total_spec_bytes += sb
            errors.extend(b_errs)
            if done - last_report >= 160 or done == len(items):
                last_report = done
                dt = max(time.time() - t0, 1e-3)
                rate = done / dt
                mb = (total_norm_bytes + total_spec_bytes) / (1024.0 * 1024.0)
                print(f"[{done}/{len(items)}] {rate:.1f} tex/s | {mb:.1f} MB | errors={len(errors)}", flush=True)

    if errors:
        for rel, err in errors[:10]:
            print(f"ERR {rel}: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
