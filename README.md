# Torchlight II D3D11 Graphics & SSAO Enhancement Mod (2026 x64)

> 纯原生 64 位 Direct3D 11 现代管线与 SSAO / SSCS / 实时水面反射 (SSR) / 3D LUT 光影增强插件与双语安装器。  
> Native 64-bit Direct3D 11 engine plugin with SSAO, SSCS contact shadows, SSR water reflections, 3D LUT color grading, and in-game F8 configuration panel.

- **Author / 作者**: M3Lewis
- **Steam Workshop / 创意工坊**: [Item ID: 3840921717](https://steamcommunity.com/sharedfiles/filedetails/?id=3840921717)
- **Target Engine / 适用版本**: 2026 x64 D3D11 Torchlight II (`v1.27` Steam `public_beta` or modern 64-bit build)

---

## Features / 核心特性

1. **Native Direct3D 11 Engine Plugin (纯原生 64 位 C++ 底层注入)**:
   - Replaces third-party injectors (ReShade / ENB) with zero hitching, zero input lag, and no crash on shutdown.
   - Comprehensive overhaul of modern HLSL 4.0 shaders for current GPU architectures.
2. **PBR Lighting & Stylized GGX Specular (物理微表面高光与油画立体感)**:
   - Stylized GGX micro-surface lobe distribution replacing legacy Blinn-Phong: razor-sharp specular cores with soft, physically plausible long tails.
   - Dual-Hemisphere ambient sky light modulating cool sky bounce `(0.96, 1.00, 1.06)` and warm ground bounce `(1.06, 0.99, 0.93)` into dungeon crevices.
   - AgX-style highlight de-saturation preventing 8-bit SDR hard clipping and posterization on intense spell glows and lanterns.
3. **Screen-Space Ambient Occlusion (SSAO) & Contact Shadows (SSCS)**:
   - High-fidelity ambient occlusion in dungeon crevices, stone pavements, and character feet.
   - Sub-pixel Screen-Space Contact Shadows (SSCS) with non-linear distance falloff and normal bias correction.
4. **Adaptive SSR Reflections & Living Water (自适应步进水面反射与动态活水)**:
   - Coarse linear raymarching with binary search refinement, eliminating venetian blind ghosting artifacts.
   - Living Water dual sinusoidal traveling wave perturbation for lively water surfaces.
   - 2-Pass bilateral spatial resolve filter with roughness-aware blurring (sharp contact, diffused distant reflections).
   - Dynamic rain wetness linkage: terrain, armor, and stonework dynamically deepen albedo and gain specular sheen during rain.
5. **Soft Particles & Micro-Normals (屏幕空间软粒子与地表微法线)**:
   - Depth-buffer particle softening eliminating hard clipping seams between fire/glow effects and geometry.
   - Companion `_tl2n` / `_tl2s` auto-discovery for RimLit materials with physical diffuse/specular roll-off.
6. **In-Game F8 Real-Time Configuration Panel ([F8] 游戏内实时控制台)**:
   - Press **`[F8]`** in-game anytime to bring up the transparent interactive control panel.
   - Real-time `0% ~ 200%` intensity sliders for all visual passes with sub-millisecond latency.
   - One-click presets: `Full HD+` / `Dark Dungeon` / `Vanilla` + **10 custom preset slots**.
   - Instant bilingual `[中]` / `[EN]` UI toggle.
7. **Color Grading & 3D LUT (电影级色彩分级)**:
   - Built-in `Hob Vibrant` (high-saturation magical fantasy) and `Dark Film` (cinematic low-saturation).
   - Place any Adobe 3D `.cube` file named `TL2_CustomLUT.cube` in the game root directory to load external LUTs.
8. **Lossless Backup & 1-Click Restore (100% 无损安全备份与原版还原)**:
   - Standalone bilingual installer `TL2_DX11_Enhance_SSAO_Setup.exe` automatically backs up vanilla shaders and configs to `TL2_DX11_Enhance_Backup\`.
   - Click "Uninstall & Restore Vanilla" anytime to restore the game to 100% byte-for-byte vanilla state.

---

## Directory Structure / 源码目录结构

```
tl2_d3d11_route_b/
├── src/                               # C++ Plugin Source Code (Plugin_TL2_DX11_Enhance_SSAO.dll)
│   ├── plugin_main.cpp                # DLL entry point, Ogre D3D11 hook initialization
│   ├── d3d11_post_types.h             # Common constants, CB definitions, and pipeline types
│   ├── d3d11_post_shaders.h/.cpp      # Embedded HLSL 4.0 shaders & compilation subsystem
│   ├── d3d11_post_renderer.h/.cpp     # D3D11 state guard, fullscreen triangle, ping-pong RTs & 3D LUT
│   ├── d3d11_ssao_pass.h/.cpp         # SSAO, SSCS contact shadows, bilateral blur & aerial perspective
│   ├── d3d11_ssr_pass.h/.cpp          # Screen-space reflections, bilateral resolve & living water
│   ├── d3d11_overlay_pass.h/.cpp      # Toast OSD overlay, config panel UI, & soft particle depth snapshot
│   ├── game_time_weather.h/.cpp       # SEH game memory level time scanner & dynamic rain weather controller
│   ├── render_hook_manager.h/.cpp     # OgreMain swapBuffers / _renderScene hooks & pass orchestrator
│   ├── config_panel.cpp               # In-game [F8] interactive configuration UI & preset manager
│   ├── phase3_scene_pipeline.cpp      # Scene rendering interceptor & material override
│   ├── fixed_func_manual_uniforms.cpp # Uniform buffer binder
│   ├── hlsl_cbuffer_fastpath.cpp      # Fast-path constant buffer updater
│   └── route_c_*.h/.cpp               # Backward-compatibility shims for legacy callers
├── installer/                         # Standalone Setup Executable Source
│   ├── installer_main.cpp             # Win32 bilingual setup GUI, PE version checker, backup/restore
│   └── assets/                        # Embedded resources (dialog backgrounds, icons)
├── shaders/                           # Modern HLSL 4.0 Shaders & Material Scripts
│   ├── hlsl4/                         # DiffPass_ps, SkinnedDiff_ps, RimLit_ps, Fxaa_ps, Bloom, SimpleUI
│   ├── shaders.program
│   ├── Phase3_Scene.program
│   └── RenderScale.material
├── scripts/                           # Build validation, environment check and helper scripts
├── tests/                             # Verification test suites (Phase 1/2/3, Route C, Shaders)
├── build.ps1                          # Builds Plugin_TL2_DX11_Enhance_SSAO.dll via MSVC cl.exe
└── build_installer.ps1                # Compiles TL2_DX11_Enhance_SSAO_Setup.exe with fxc gatekeeper
```

---

## Building from Source / 编译构建指南

Prerequisites: Windows 10/11 x64, Visual Studio 2022 (MSVC v143) with C++ Desktop Development tools.

### 1. Build Plugin DLL
```powershell
.\build.ps1
# Output: bin\Release\Plugin_TL2_DX11_Enhance_SSAO.dll
```

### 2. Build Standalone Installer
```powershell
.\build_installer.ps1
# Output: bin\Release\TL2_DX11_Enhance_SSAO_Setup.exe
```

---

## Special Thanks / 特别鸣谢

- **mikuro**: For the exceptional open-source Torchlight II packing tools and reverse-engineering foundation for breaking the 2GB VFS limit.
- **THOUGHTCRIME**: Creator of the original Enhanced Retex mod, pioneering the texture remastering of Torchlight II.
