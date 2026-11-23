#pragma once

#include <cstdint>
#include "d3d11_post_types.h"

namespace routeb {

bool InstallRenderPipeline(void* ogre_main_base, PostLogFn logger);
void RemoveRenderPipeline();
bool VerifyPostShaders();
PipelineStats GetPipelineStats();
void LogPipelineSurveySummary(PostLogFn logger);

void SetAoRuntimeEnabled(bool enabled);
bool IsAoRuntimeEnabled();
bool ToggleAoRuntimeEnabled();

void SetSscsRuntimeEnabled(bool enabled);
bool IsSscsRuntimeEnabled();
bool ToggleSscsRuntimeEnabled();

void SetSoftParticlesEnabled(bool enabled);
bool IsSoftParticlesEnabled();
bool ToggleSoftParticlesEnabled();

void SetAerialVignetteEnabled(bool enabled);
bool IsAerialVignetteEnabled();
bool ToggleAerialVignetteEnabled();

void SetSsrEnabled(bool enabled);
bool IsSsrEnabled();
bool ToggleSsrEnabled();

void SetLutPreset(int preset);
int GetLutPreset();
const char* LutPresetName(int preset);

void SetAoStrengthPct(int pct);
int GetAoStrengthPct();
void SetSscsStrengthPct(int pct);
int GetSscsStrengthPct();
void SetSoftParticlesStrengthPct(int pct);
int GetSoftParticlesStrengthPct();
void SetAerialVignetteStrengthPct(int pct);
int GetAerialVignetteStrengthPct();
void SetSsrStrengthPct(int pct);
int GetSsrStrengthPct();
void SetLutStrengthPct(int pct);
int GetLutStrengthPct();

bool IsWorldDepthReadyForParticles();
void TriggerToastForTest();

void OnScenePassObserved(
    const char* pass_name,
    bool lighting,
    bool depth_write,
    bool transparent,
    bool alpha_reject,
    bool has_vp,
    bool has_fp);

// Legacy shims
inline bool InstallRouteCAoPipeline(void* b, PostLogFn l) { return InstallRenderPipeline(b, l); }
inline void RemoveRouteCAoPipeline() { RemoveRenderPipeline(); }
inline bool VerifyRouteCShaders() { return VerifyPostShaders(); }
inline PipelineStats GetRouteCAoStats() { return GetPipelineStats(); }
inline void LogRouteCSurveySummary(PostLogFn l) { LogPipelineSurveySummary(l); }

inline void SetRouteCAoRuntimeEnabled(bool e) { SetAoRuntimeEnabled(e); }
inline bool IsRouteCAoRuntimeEnabled() { return IsAoRuntimeEnabled(); }
inline bool ToggleRouteCAoRuntimeEnabled() { return ToggleAoRuntimeEnabled(); }
inline void SetRouteCSscsRuntimeEnabled(bool e) { SetSscsRuntimeEnabled(e); }
inline bool IsRouteCSscsRuntimeEnabled() { return IsSscsRuntimeEnabled(); }
inline bool ToggleRouteCSscsRuntimeEnabled() { return ToggleSscsRuntimeEnabled(); }
inline void SetP1SoftParticlesEnabled(bool e) { SetSoftParticlesEnabled(e); }
inline bool IsP1SoftParticlesEnabled() { return IsSoftParticlesEnabled(); }
inline bool ToggleP1SoftParticlesEnabled() { return ToggleSoftParticlesEnabled(); }
inline void SetP2AerialVignetteEnabled(bool e) { SetAerialVignetteEnabled(e); }
inline bool IsP2AerialVignetteEnabled() { return IsAerialVignetteEnabled(); }
inline bool ToggleP2AerialVignetteEnabled() { return ToggleAerialVignetteEnabled(); }
inline void SetP2SsrEnabled(bool e) { SetSsrEnabled(e); }
inline bool IsP2SsrEnabled() { return IsSsrEnabled(); }
inline bool ToggleP2SsrEnabled() { return ToggleSsrEnabled(); }
inline void SetP2LutPreset(int p) { SetLutPreset(p); }
inline int GetP2LutPreset() { return GetLutPreset(); }
inline const char* P2LutPresetName(int p) { return LutPresetName(p); }

inline void SetRouteCAoStrengthPct(int p) { SetAoStrengthPct(p); }
inline int GetRouteCAoStrengthPct() { return GetAoStrengthPct(); }
inline void SetRouteCSscsStrengthPct(int p) { SetSscsStrengthPct(p); }
inline int GetRouteCSscsStrengthPct() { return GetSscsStrengthPct(); }
inline void SetP1SoftParticlesStrengthPct(int p) { SetSoftParticlesStrengthPct(p); }
inline int GetP1SoftParticlesStrengthPct() { return GetSoftParticlesStrengthPct(); }
inline void SetP2AerialVignetteStrengthPct(int p) { SetAerialVignetteStrengthPct(p); }
inline int GetP2AerialVignetteStrengthPct() { return GetAerialVignetteStrengthPct(); }
inline void SetP2SsrStrengthPct(int p) { SetSsrStrengthPct(p); }
inline int GetP2SsrStrengthPct() { return GetSsrStrengthPct(); }
inline void SetP2LutStrengthPct(int p) { SetLutStrengthPct(p); }
inline int GetP2LutStrengthPct() { return GetLutStrengthPct(); }

inline bool IsP2CustomCubeLutLoaded() { return false; /* overridden in post_renderer */ }
inline bool IsRouteCWorldDepthReadyForParticles() { return IsWorldDepthReadyForParticles(); }
inline void TriggerRouteCToastForTest() { TriggerToastForTest(); }
inline void OnRouteCScenePassObserved(
    const char* pass_name,
    bool lighting,
    bool depth_write,
    bool transparent,
    bool alpha_reject,
    bool has_vp,
    bool has_fp) {
    OnScenePassObserved(pass_name, lighting, depth_write, transparent, alpha_reject, has_vp, has_fp);
}

}  // namespace routeb
