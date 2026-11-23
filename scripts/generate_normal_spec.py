#!/usr/bin/env python3
"""Offline multi-scale normal (_tl2n.dds) and semantic material/cavity (_tl2s.dds) generator for Torchlight II.

Produces Ogre 1.7 + D3D11 compatible -dx9 linear DDS maps:
  - <stem>_tl2n.dds : BC3_UNORM (DXT5nm: A = nx, G = ny; R=G=B=ny for clean BC3 green precision)
  - <stem>_tl2s.dds : BC1_UNORM (DXT1: R = Smoothness, G = Metalness, B = Dark-only CavityAO)

Architecture (Item 1 upgrade, performance-optimized):
  1. Native 2x resolution for normals (--max-norm-dim 1024, --half-large OFF) and crisp 512px
     for material/cavity (--max-spec-dim 512, 4x faster & leaner VRAM while preserving bevels).
  2. Fused in-place CIELab-guided separable bilateral structure decomposition (zero brush/DXT
     sandpaper grit, crisp stone/metal transitions).
  3. Chromatic boundary seam carving for inlaid trim where luminance contrast alone is weak.
  4. C-accelerated Pillow morphological closing for dark-only Cavity AO (zero white rim halos).
  5. Path-prior + CIELab semantic material classifier separating steel, gunmetal, enchanted runic
     metal, bronze/gold filigree, gem/crystal clearcoat, and dielectric stone/wood/cloth.
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
from PIL import Image, ImageFilter

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

FLORA_WOOD_HINTS = (
    "/FOLIAGE/",
    "/FLORA/",
    "/TREE",
    "/LEAF",
    "/LEAVES/",
    "/GRASS",
    "/BUSH",
    "/BARK",
    "/WOOD",
    "/PLANK",
    "/VINE",
    "/SHRUB",
)

MASONRY_TERRAIN_HINTS = (
    "/TERRAIN/",
    "/GROUND/",
    "/DIRT/",
    "/SAND/",
    "/SNOW/",
    "/MUD/",
    "/ROCK",
    "/CLIFF",
    "/STONE",
    "/BRICK",
    "/COBBLE",
    "/FLOOR",
    "/WALL",
    "/RUIN",
    "/CATACOMB",
    "/CAVE",
)

SOFT_ORGANIC_HINTS = (
    "/CLOTH",
    "/CAPE",
    "/BANNER",
    "/FUR",
    "/LEATHER",
    "/SKIN",
    "/HAIR",
    "/FEATHER",
)

GEM_GLASS_HINTS = (
    "/GEM",
    "/CRYSTAL",
    "/EMBER",
    "/GLASS",
    "/ICE",
    "/JEWEL",
    "CRYSTAL",
    "EMBER",
    "GEM",
)

METAL_PROP_HINTS = (
    "/ARMOR",
    "/WEAPON",
    "/SHIELD",
    "/STEEL",
    "/IRON",
    "/METAL",
    "/BRASS",
    "/COPPER",
    "/BRONZE",
    "/GOLD",
    "/DWARF",
    "/DWARVEN",
    "/AUTOMATON",
    "/CLOCKWORK",
    "/ROBOT",
    "/CHEST",
    "/ANVIL",
    "/PIPE",
    "_METAL",
    "METAL_",
    "_STEEL",
    "_IRON",
    "_BRASS",
    "_BRONZE",
    "_COPPER",
    "_ARMOR",
    "DWARVEN",
    "CLOCKWORK",
    "AUTOMATON",
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
    radius = max(1, int(np.ceil(sigma * 2.6)))
    x = np.arange(-radius, radius + 1, dtype=np.float32)
    k = np.exp(-0.5 * (x / sigma) ** 2)
    k /= k.sum()
    return k


def blur_1d_separable(arr: np.ndarray, sigma: float, wrap: bool) -> np.ndarray:
    if sigma <= 0.15:
        return arr
    # Pyramid downsample for wide Gaussians (sigma > 2.6) slashes memory/loop overhead.
    if sigma > 2.6 and min(arr.shape[0], arr.shape[1]) >= 48:
        factor = 4 if (sigma > 9.0 and min(arr.shape[0], arr.shape[1]) >= 128) else 2
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


def bilateral_separable_multi(
    vals: list[np.ndarray],
    guide_l: np.ndarray,
    guide_a: np.ndarray,
    guide_b: np.ndarray,
    radius: int,
    step: int,
    sigma_s: float,
    sigma_r: float,
    wrap: bool,
) -> list[np.ndarray]:
    """Fused in-place 2-pass separable joint bilateral filter sharing range weights across channels."""
    pad_mode = "wrap" if wrap else "edge"
    h, w = guide_l.shape
    pad = radius * step
    inv_2s2 = np.float32(0.5 / max(sigma_s * sigma_s, 1e-6))
    inv_2r2 = np.float32(0.5 / max(sigma_r * sigma_r, 1e-6))
    chroma_w = np.float32(0.55)

    # Preallocate work buffers to eliminate L3 cache thrashing across taps
    d2 = np.empty((h, w), dtype=np.float32)
    diff = np.empty((h, w), dtype=np.float32)
    wt = np.empty((h, w), dtype=np.float32)
    wsum = np.empty((h, w), dtype=np.float32)
    accs = [np.empty((h, w), dtype=np.float32) for _ in vals]

    # --- Horizontal pass ---
    pl = np.pad(guide_l, ((0, 0), (pad, pad)), mode=pad_mode)
    pa = np.pad(guide_a, ((0, 0), (pad, pad)), mode=pad_mode)
    pb = np.pad(guide_b, ((0, 0), (pad, pad)), mode=pad_mode)
    pvs = [np.pad(v, ((0, 0), (pad, pad)), mode=pad_mode) for v in vals]

    wsum.fill(0.0)
    for a in accs:
        a.fill(0.0)

    for k in range(-radius, radius + 1):
        off = pad + k * step
        ws = np.float32(np.exp(-((k * step) ** 2) * inv_2s2))
        if k == 0:
            wt.fill(ws)
        else:
            np.subtract(pa[:, off : off + w], guide_a, out=diff)
            np.multiply(diff, diff, out=d2)
            np.subtract(pb[:, off : off + w], guide_b, out=diff)
            np.multiply(diff, diff, out=diff)
            d2 += diff
            d2 *= chroma_w
            np.subtract(pl[:, off : off + w], guide_l, out=diff)
            np.multiply(diff, diff, out=diff)
            d2 += diff
            d2 *= inv_2r2
            d2 += np.float32(1.0)
            np.divide(ws, d2, out=wt)

        wsum += wt
        for idx, pv in enumerate(pvs):
            np.multiply(wt, pv[:, off : off + w], out=diff)
            accs[idx] += diff

    np.maximum(wsum, np.float32(1e-6), out=wsum)
    tmps = [a / wsum for a in accs]

    # --- Vertical pass ---
    pl = np.pad(guide_l, ((pad, pad), (0, 0)), mode=pad_mode)
    pa = np.pad(guide_a, ((pad, pad), (0, 0)), mode=pad_mode)
    pb = np.pad(guide_b, ((pad, pad), (0, 0)), mode=pad_mode)
    pvs = [np.pad(t, ((pad, pad), (0, 0)), mode=pad_mode) for t in tmps]

    wsum.fill(0.0)
    for a in accs:
        a.fill(0.0)

    for k in range(-radius, radius + 1):
        off = pad + k * step
        ws = np.float32(np.exp(-((k * step) ** 2) * inv_2s2))
        if k == 0:
            wt.fill(ws)
        else:
            np.subtract(pa[off : off + h, :], guide_a, out=diff)
            np.multiply(diff, diff, out=d2)
            np.subtract(pb[off : off + h, :], guide_b, out=diff)
            np.multiply(diff, diff, out=diff)
            d2 += diff
            d2 *= chroma_w
            np.subtract(pl[off : off + h, :], guide_l, out=diff)
            np.multiply(diff, diff, out=diff)
            d2 += diff
            d2 *= inv_2r2
            d2 += np.float32(1.0)
            np.divide(ws, d2, out=wt)

        wsum += wt
        for idx, pv in enumerate(pvs):
            np.multiply(wt, pv[off : off + h, :], out=diff)
            accs[idx] += diff

    np.maximum(wsum, np.float32(1e-6), out=wsum)
    return [a / wsum for a in accs]


def morph_closing_fast(arr: np.ndarray, filter_size: int, wrap: bool) -> np.ndarray:
    """Fast C-level morphological closing via Pillow MaxFilter -> MinFilter."""
    pad = filter_size // 2 + 1
    pad_mode = "wrap" if wrap else "edge"
    h, w = arr.shape
    u8 = np.clip(arr * np.float32(255.0) + np.float32(0.5), 0.0, 255.0).astype(np.uint8)
    pu8 = np.pad(u8, ((pad, pad), (pad, pad)), mode=pad_mode)
    closed = (
        Image.fromarray(pu8, mode="L")
        .filter(ImageFilter.MaxFilter(size=filter_size))
        .filter(ImageFilter.MinFilter(size=filter_size))
    )
    return np.asarray(closed, dtype=np.float32)[pad : pad + h, pad : pad + w] / np.float32(255.0)


def resize_f32(arr: np.ndarray, tw: int, th: int) -> np.ndarray:
    if arr.shape[1] == tw and arr.shape[0] == th:
        return arr
    return np.asarray(
        Image.fromarray(arr, mode="F").resize((tw, th), Image.Resampling.BILINEAR),
        dtype=np.float32,
    )


def bleed_transparent_pixels(rgb: np.ndarray, alpha: np.ndarray) -> np.ndarray:
    """Prevent hard background color bleeding across alpha-cutout edges."""
    if alpha is None:
        return rgb
    opaque = alpha >= 0.45
    if np.all(opaque) or not np.any(opaque):
        return rgb
    work = rgb.copy()
    mask = opaque.astype(np.float32)
    for _ in range(4):
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


def smoothstep(e0: float, e1: float, x: np.ndarray | float) -> np.ndarray:
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


def path_material_priors(upper_rel: str) -> tuple[float, float, float]:
    """Return (metal_cap, smooth_bias, crystal_hint) from asset path semantics."""
    metal_cap = 0.65
    smooth_bias = 0.0
    crystal_hint = 0.0

    if any(h in upper_rel for h in METAL_PROP_HINTS):
        metal_cap = 1.0
        smooth_bias = 0.06
    elif any(h in upper_rel for h in GEM_GLASS_HINTS):
        metal_cap = 0.15
        smooth_bias = 0.25
        crystal_hint = 0.85
    elif any(h in upper_rel for h in FLORA_WOOD_HINTS):
        metal_cap = 0.06
        smooth_bias = -0.06
    elif any(h in upper_rel for h in SOFT_ORGANIC_HINTS):
        metal_cap = 0.12
        smooth_bias = -0.05
    elif any(h in upper_rel for h in MASONRY_TERRAIN_HINTS):
        metal_cap = 0.22
        smooth_bias = -0.02

    return metal_cap, smooth_bias, crystal_hint


def synthesize_maps(
    src_path: Path,
    rel_posix: str,
    max_norm_dim: int = 1024,
    max_spec_dim: int = 512,
    half_large: bool = False,
    want_preview: bool = False,
) -> tuple[Image.Image, Image.Image, Image.Image | None]:
    """Generate (dxt5nm_rgba_img, spec_rgb_img, optional preview_xyz_normal_img)."""
    with Image.open(src_path) as im:
        has_alpha = im.mode in ("RGBA", "LA", "PA") or (
            im.mode == "P" and "transparency" in im.info
        )
        orig_w, orig_h = im.size
        nw, nh = compute_target_size(orig_w, orig_h, max_norm_dim, half_large=half_large)
        sw, sh = compute_target_size(orig_w, orig_h, max_spec_dim, half_large=half_large)
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
    scale = float(np.clip(min(w, h) / 1024.0, 0.5, 1.8))

    # Perceptual sRGB opponent coordinates (sRGB is already ~1/2.2 compressed, avoiding costly cbrt/pow)
    luma = np.float32(0.299) * rgb[..., 0] + np.float32(0.587) * rgb[..., 1] + np.float32(0.114) * rgb[..., 2]
    a_op = np.float32(0.75) * (rgb[..., 0] - rgb[..., 1])
    b_op = np.float32(0.45) * (np.float32(0.5) * (rgb[..., 0] + rgb[..., 1]) - rgb[..., 2])

    # Fused Pass 1 (3x3 fine cleanup): filter (luma, a_op, b_op) together using shared algebraic bilateral weights
    step_s1 = max(1, int(round(scale)))
    struct_l, a_s1, b_s1 = bilateral_separable_multi(
        [luma, a_op, b_op],
        luma,
        a_op,
        b_op,
        radius=1,
        step=step_s1,
        sigma_s=1.5 * scale,
        sigma_r=0.060,
        wrap=is_world_tile,
    )
    # Pass 2 (strided 7x7 effective support): flatten broad painted gradients while freezing Pass-1 seams
    step_s2 = max(2, int(round(2.0 * scale)))
    [struct_l] = bilateral_separable_multi(
        [struct_l],
        struct_l,
        a_s1,
        b_s1,
        radius=1,
        step=step_s2,
        sigma_s=2.8 * scale,
        sigma_r=0.070,
        wrap=is_world_tile,
    )

    # Soft-shrink residual brush grit, retaining real deep pores/weave (> 0.015)
    raw_grain = luma - struct_l
    clean_grain = np.sign(raw_grain) * np.maximum(np.abs(raw_grain) - np.float32(0.015), np.float32(0.0))
    clean_grain = np.clip(clean_grain, np.float32(-0.075), np.float32(0.075))

    # Multi-scale DoG height pyramid computed over edge-preserved structure luminance
    g_fine = blur_1d_separable(struct_l, 1.15 * scale, wrap=is_world_tile)
    g_meso = blur_1d_separable(struct_l, 3.2 * scale, wrap=is_world_tile)
    g_macro = blur_1d_separable(struct_l, 8.8 * scale, wrap=is_world_tile)
    g_broad = blur_1d_separable(struct_l, 21.0 * scale, wrap=is_world_tile)

    hf_fine = (struct_l - g_fine) + np.float32(0.38) * clean_grain
    hf_meso = g_fine - g_meso
    hf_macro = g_meso - g_macro
    hf_broad = g_macro - g_broad

    # Balanced octave height + chromatic boundary seam carving
    height = np.float32(0.24) * hf_fine + np.float32(0.46) * hf_meso + np.float32(0.58) * hf_macro + np.float32(0.22) * hf_broad
    height = np.where(height < 0.0, height * np.float32(1.25), np.tanh(height * np.float32(2.35)) / np.float32(2.35))

    agx, agy = scharr_grad(a_s1, wrap=is_world_tile)
    bgx, bgy = scharr_grad(b_s1, wrap=is_world_tile)
    chroma_edge = np.sqrt(agx * agx + agy * agy + bgx * bgx + bgy * bgy)
    seam_groove = smoothstep(0.018, 0.065, chroma_edge) * smoothstep(0.0, 0.025, np.maximum(g_meso - struct_l + 0.008, 0.0))
    height -= seam_groove * np.float32(0.016)

    gx, gy = scharr_grad(height, wrap=is_world_tile)
    strength = 10.8 * ((max(w, h) / 1024.0) ** 0.30)
    raw_nx = -gx * strength
    raw_ny = -gy * strength
    grad_mag = np.sqrt(raw_nx * raw_nx + raw_ny * raw_ny) + np.float32(1e-8)
    capped_mag = np.float32(1.65) * np.tanh(grad_mag / np.float32(1.65))
    slope_gain = capped_mag / grad_mag
    nx = raw_nx * slope_gain
    ny = raw_ny * slope_gain
    nz = np.ones_like(nx, dtype=np.float32)

    if alpha_mask is not None:
        edge_weight = smoothstep(0.25, 0.85, alpha_mask)
        nx *= edge_weight
        ny *= edge_weight

    norm_len = np.sqrt(nx * nx + ny * ny + nz * nz) + np.float32(1e-8)
    nx /= norm_len
    ny /= norm_len
    nz /= norm_len

    if want_preview:
        xyz_u8 = np.stack(
            [
                np.clip(nx * 0.5 + 0.5, 0.0, 1.0) * 255.0,
                np.clip(ny * 0.5 + 0.5, 0.0, 1.0) * 255.0,
                np.clip(nz * 0.5 + 0.5, 0.0, 1.0) * 255.0,
            ],
            axis=-1,
        ).astype(np.uint8)
        xyz_img: Image.Image | None = Image.fromarray(xyz_u8, mode="RGB")
    else:
        xyz_img = None

    a_u8 = (np.clip(nx * 0.5 + 0.5, 0.0, 1.0) * 255.0).astype(np.uint8)
    g_u8 = (np.clip(ny * 0.5 + 0.5, 0.0, 1.0) * 255.0).astype(np.uint8)
    dxt5nm_arr = np.stack([g_u8, g_u8, g_u8, a_u8], axis=-1)
    dxt5nm_img = Image.fromarray(dxt5nm_arr, mode="RGBA")

    # Compute _tl2s directly at target spec resolution (sw, sh) for 4x speed when max_spec_dim=512
    spec_l = resize_f32(struct_l, sw, sh)
    spec_luma = resize_f32(luma, sw, sh)
    spec_a = resize_f32(a_s1, sw, sh)
    spec_b = resize_f32(b_s1, sw, sh)
    spec_hf_meso = resize_f32(hf_meso, sw, sh)
    spec_hf_macro = resize_f32(hf_macro, sw, sh)
    if (sw, sh) != (w, h):
        rgb_s = np.stack(
            [resize_f32(rgb[..., 0], sw, sh), resize_f32(rgb[..., 1], sw, sh), resize_f32(rgb[..., 2], sw, sh)],
            axis=-1,
        )
    else:
        rgb_s = rgb

    spec_scale = float(np.clip(min(sw, sh) / 1024.0, 0.4, 1.8))
    metal_cap, smooth_bias, crystal_hint = path_material_priors(upper_rel)
    micro_var = blur_1d_separable(np.abs(spec_luma - spec_l), max(1.0, 2.2 * spec_scale), wrap=is_world_tile)

    maxc = np.max(rgb_s, axis=-1)
    minc = np.min(rgb_s, axis=-1)
    sat = np.where(maxc > 1e-4, (maxc - minc) / np.maximum(maxc, 1e-4), 0.0)
    chroma_mag = np.sqrt(spec_a * spec_a + spec_b * spec_b)

    low_grain = 1.0 - smoothstep(0.025, 0.085, micro_var)
    is_explicit_metal = metal_cap >= 0.9

    # 1. Neutral Steel / Iron / Gunmetal / Silver
    steel_l_lo = 0.09 if is_explicit_metal else 0.16
    steel_l_hi = 0.22 if is_explicit_metal else 0.32
    steel_mask = (
        (1.0 - smoothstep(0.09, 0.24, sat))
        * smoothstep(steel_l_lo, steel_l_hi, spec_l)
        * (1.0 - smoothstep(0.76, 0.92, spec_l))
    )

    # 2. Cool Enchanted / Runic Steel
    cool_l_lo = 0.11 if is_explicit_metal else 0.20
    cool_l_hi = 0.24 if is_explicit_metal else 0.38
    cool_steel = (
        smoothstep(0.020, 0.08, rgb_s[..., 2] - rgb_s[..., 0])
        * smoothstep(0.09, 0.22, sat)
        * (1.0 - smoothstep(0.32, 0.50, sat))
        * smoothstep(cool_l_lo, cool_l_hi, spec_l)
        * (1.0 - crystal_hint * 0.85)
    )

    # 3. Warm Gold / Brass / Bronze / Copper filigree
    warm_l_lo = 0.10 if is_explicit_metal else 0.24
    warm_l_hi = 0.24 if is_explicit_metal else 0.42
    warm_rb_lo = 0.05 if is_explicit_metal else 0.11
    warm_rb_hi = 0.16 if is_explicit_metal else 0.25
    warm_gold = (
        smoothstep(warm_rb_lo, warm_rb_hi, rgb_s[..., 0] - rgb_s[..., 2])
        * smoothstep(0.005, 0.09, rgb_s[..., 0] - rgb_s[..., 1])
        * smoothstep(warm_l_lo, warm_l_hi, spec_l)
        * smoothstep(0.16, 0.38, sat)
        * (1.0 - smoothstep(0.68, 0.86, sat))
    )

    raw_metal = np.maximum(steel_mask, np.maximum(cool_steel * 0.82, warm_gold * 0.92)) * low_grain * np.float32(metal_cap)
    if is_explicit_metal:
        metal_base_lift = smoothstep(0.08, 0.18, spec_l) * (1.0 - smoothstep(0.55, 0.78, sat)) * low_grain * np.float32(0.55)
        raw_metal = np.maximum(raw_metal, metal_base_lift)

    [metalness] = bilateral_separable_multi(
        [raw_metal],
        spec_l,
        spec_a,
        spec_b,
        radius=1,
        step=1,
        sigma_s=1.4 * spec_scale,
        sigma_r=0.06,
        wrap=is_world_tile,
    )
    metalness = np.clip(metalness, 0.0, 1.0)

    # Dark-only Morphological Cavity AO via fast Pillow C MaxFilter -> MinFilter
    morph_sz = 5 if min(sw, sh) >= 512 else 3
    closed_l = morph_closing_fast(spec_l, filter_size=morph_sz, wrap=is_world_tile)
    crevice_depth = np.maximum(closed_l - spec_l, np.float32(0.0))
    valley_dark = np.maximum(-(spec_hf_meso * np.float32(0.50) + spec_hf_macro * np.float32(0.78)), np.float32(0.0))
    cavity = np.clip(np.float32(1.0) - crevice_depth * np.float32(2.85) - valley_dark * np.float32(1.55), np.float32(0.16), np.float32(1.0))

    base_smooth = (1.0 - np.clip(micro_var * 9.5, 0.0, 0.88)) * smoothstep(0.07, 0.36, spec_l)
    bevel_sheen = smoothstep(0.015, 0.12, np.maximum(spec_hf_meso + spec_hf_macro, 0.0))
    gem_sheen = crystal_hint * smoothstep(0.22, 0.50, spec_l) * smoothstep(0.06, 0.16, chroma_mag)
    smoothness = 0.12 + 0.24 * base_smooth + 0.14 * bevel_sheen + 0.42 * metalness + 0.45 * gem_sheen + smooth_bias
    smoothness = np.clip(smoothness, 0.05, 0.92)
    smoothness *= smoothstep(0.04, 0.18, spec_l) * np.clip(cavity * 0.90 + 0.12, 0.15, 1.0)

    spec_u8 = np.stack(
        [
            np.clip(smoothness * 255.0, 0.0, 255.0),
            np.clip(metalness * 255.0, 0.0, 255.0),
            np.clip(cavity * 255.0, 0.0, 255.0),
        ],
        axis=-1,
    ).astype(np.uint8)
    spec_img = Image.fromarray(spec_u8, mode="RGB")

    return dxt5nm_img, spec_img, xyz_img


def run_texconv_batch(texconv: Path, src_files: list[Path], out_dir: Path, fmt: str) -> None:
    if not src_files:
        return
    flist = out_dir / f"flist_{fmt}.txt"
    flist.write_text("\n".join(p.name for p in src_files) + "\n", encoding="ascii")
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
        ".",
        "-flist",
        flist.name,
    ]
    res = subprocess.run(cmd, cwd=str(out_dir), capture_output=True)
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
    tmp_root: Path,
    texconv: Path,
    max_norm_dim: int,
    max_spec_dim: int,
    half_large: bool,
    preview_dir: Path | None,
    skip_existing: bool,
) -> tuple[int, int, int, list[tuple[str, str]]]:
    done = 0
    total_n = 0
    total_s = 0
    errors: list[tuple[str, str]] = []
    pending: list[tuple[int, str, Path, Path]] = []

    tmp_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="tl2ns_", dir=str(tmp_root)) as tmp:
        tmp_dir = Path(tmp)
        n_tgas: list[Path] = []
        s_tgas: list[Path] = []
        want_preview = preview_dir is not None

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
                    src_path,
                    rel_posix,
                    max_norm_dim=max_norm_dim,
                    max_spec_dim=max_spec_dim,
                    half_large=half_large,
                    want_preview=want_preview,
                )
                if preview_dir is not None and preview_img is not None:
                    preview_dir.mkdir(parents=True, exist_ok=True)
                    safe_tag = rel_no_ext.replace("/", "__")
                    preview_img.save(preview_dir / f"{safe_tag}_norm_preview.png")
                    spec_img.save(preview_dir / f"{safe_tag}_spec_preview.png")

                # Uncompressed TGA avoids slow PNG zlib wrap/WIC decode while keeping 100% lossless 32-bit/24-bit input
                n_tga = tmp_dir / f"n_{idx}.tga"
                s_tga = tmp_dir / f"s_{idx}.tga"
                dxt5nm_img.save(n_tga)
                spec_img.save(s_tga)
                n_tgas.append(n_tga)
                s_tgas.append(s_tga)
                pending.append((idx, rel_posix, norm_dst, spec_dst))
            except Exception as ex:
                done += 1
                errors.append((rel_posix, str(ex)))

        if pending:
            try:
                run_texconv_batch(texconv, n_tgas, tmp_dir, "BC3_UNORM")
                run_texconv_batch(texconv, s_tgas, tmp_dir, "BC1_UNORM")
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
    ap.add_argument("--workers", type=int, default=min(6, os.cpu_count() or 4))
    ap.add_argument("--batch-size", type=int, default=16)
    ap.add_argument("--max-norm-dim", type=int, default=1024)
    ap.add_argument("--max-spec-dim", type=int, default=512)
    ap.add_argument("--half-large", action="store_true", help="Downsample >=1024px textures by 2x before synthesis")
    ap.add_argument("--preview-dir", default="", help="Optional directory to save readable PNG previews")
    ap.add_argument("--tmp-root", default="", help="Optional workspace temp directory (avoids %TEMP%)")
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

    print(
        f"Selected {len(items)} eligible textures "
        f"(max_norm={args.max_norm_dim}, max_spec={args.max_spec_dim}, half_large={args.half_large}, "
        f"workers={args.workers}, batch_size={args.batch_size})"
    )
    if not items:
        return 0

    out_root = Path(args.out).resolve()
    tmp_root = Path(args.tmp_root).resolve() if args.tmp_root else (out_root.parent / "_tmp_ns_work")
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
                tmp_root,
                texconv,
                args.max_norm_dim,
                args.max_spec_dim,
                args.half_large,
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

    try:
        if tmp_root.exists() and not any(tmp_root.iterdir()):
            tmp_root.rmdir()
    except OSError:
        pass

    if errors:
        for rel, err in errors[:10]:
            print(f"ERR {rel}: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
