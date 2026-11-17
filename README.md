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
   - Comprehensive overhaul of 16 HLSL 4.0 shaders for modern GPU architectures.
2. **Screen-Space Ambient Occlusion (SSAO) & Contact Shadows (SSCS)**:
   - High-fidelity ambient occlusion in dungeon crevices, stone pavements, and character feet.
   - Depth-buffer based Screen-Space Contact Shadows (SSCS) with smooth attenuation.
3. **Real-Time Screen-Space Reflections (SSR 水面动态反射)**:
   - Real-time reflections on water pools and wet surfaces reflecting surrounding torches, crystal spires, and spell effects.
4. **Micro-Normals, Specular Highlights & Rim Lighting (地表微法线与高光)**:
   - Physical diffuse and specular roll-off for cobblestones and metallic armor.
5. **Soft Particles (屏幕空间软粒子)**:
   - Eliminates hard particle clipping seams between fire, spell glows, and terrain meshes.
6. **In-Game F8 Real-Time Configuration Panel ([F8] 游戏内实时控制台)**:
   - Press **`[F8]`** in-game anytime to bring up the transparent control panel.
   - Real-time `0% ~ 200%` intensity sliders for all 10 visual passes with millisecond latency.
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
├── src/                    # C++ Plugin Source Code (Plugin_TL2_DX11_Enhance_SSAO.dll)
│   ├── plugin_main.cpp     # DLL entry point, Ogre D3D11 hook initialization
│   ├── config_panel.cpp    # In-game F8 ImGui/Win32 configuration UI & hotkeys
│   ├── phase3_scene_pipeline.cpp # Scene rendering interceptor & material override
│   ├── route_c_ao.cpp      # SSAO & SSCS render pass implementation
│   ├── route_c_d3d11.cpp   # Direct3D 11 device/context hooks & depth buffer management
│   ├── fixed_func_manual_uniforms.cpp # Uniform buffer binder
│   └── hlsl_cbuffer_fastpath.cpp      # Fast-path constant buffer updater
├── installer/              # Standalone Setup Executable Source
│   ├── installer_main.cpp  # Win32 bilingual setup GUI, PE version checker, backup/restore
│   └── assets/             # Embedded resources (dialog backgrounds, icons)
├── shaders/                # 16 Modern HLSL 4.0 Shaders & Material Scripts
│   ├── hlsl4/              # DiffPass_ps, RimLit_ps, Fxaa_ps, Bloom, SimpleUI
│   ├── shaders.program
│   ├── Phase3_Scene.program
│   └── RenderScale.material
├── scripts/                # Environment check and helper scripts
├── tests/                  # Verification test suites
├── build.ps1               # Builds Plugin_TL2_DX11_Enhance_SSAO.dll via MSVC cl.exe
└── build_installer.ps1     # Compiles TL2_DX11_Enhance_SSAO_Setup.exe
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
