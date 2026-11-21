#pragma once

#include "d3d11_post_types.h"

namespace routeb {

bool ExecuteP2FilterPass(const P2FilterRequest& req, PostLogFn logger);
bool ExecutePanelOverlay(const PanelOverlayRequest& req, PostLogFn logger);
bool ExecuteToastOverlay(
    ID3D11DeviceContext* ctx,
    ID3D11RenderTargetView* target_rtv,
    bool dll_enhance_on,
    bool ao_on,
    int filter_preset,
    bool ap_on,
    float fade_alpha,
    PostLogFn logger);

bool GetParticleDepthSnapshot(ParticleDepthSnapshot* out);
bool BindParticleState(
    ID3D11DeviceContext* ctx,
    int soft_mode,
    float inv_soft_range,
    const ParticleDepthSnapshot* snap);
void ResetParticleDepthSnapshot();

// Legacy shims
inline bool ExecuteRouteCP2FilterPass(const P2FilterRequest& req, PostLogFn logger) {
    return ExecuteP2FilterPass(req, logger);
}
inline bool ExecuteRouteCPanelOverlay(const PanelOverlayRequest& req, PostLogFn logger) {
    return ExecutePanelOverlay(req, logger);
}
inline bool ExecuteRouteCToastOverlay(
    ID3D11DeviceContext* ctx,
    ID3D11RenderTargetView* target_rtv,
    bool dll_enhance_on,
    bool ao_on,
    int filter_preset,
    bool ap_on,
    float fade_alpha,
    PostLogFn logger) {
    return ExecuteToastOverlay(ctx, target_rtv, dll_enhance_on, ao_on, filter_preset, ap_on, fade_alpha, logger);
}
inline bool GetRouteCParticleDepthSnapshot(ParticleDepthSnapshot* out) {
    return GetParticleDepthSnapshot(out);
}
inline bool BindRouteCParticleState(
    ID3D11DeviceContext* ctx,
    int soft_mode,
    float inv_soft_range,
    const ParticleDepthSnapshot* snap) {
    return BindParticleState(ctx, soft_mode, inv_soft_range, snap);
}
inline void ResetRouteCParticleDepthSnapshot() {
    ResetParticleDepthSnapshot();
}

}  // namespace routeb
