#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;

namespace routeb {

using PostLogFn = void (*)(const char* fmt, ...);

void PrepareShadowMapForFrame(
    ID3D11Device* dev,
    ID3D11DeviceContext* ctx,
    ID3D11RenderTargetView* scene_rtv,
    const float* view_4x4,
    const float* proj_4x4,
    PostLogFn logger);

void SetWorldShadowCaptureOpen(bool open);
bool IsWorldShadowCaptureOpen();

void SetShadowCasterAlphaCutout(bool alpha_cutout, float alpha_ref);

bool ShouldBypassDrawHookForShadowReplay();

using FnD3D11DrawIndexed = void(__stdcall*)(
    ID3D11DeviceContext*, uint32_t, uint32_t, int32_t);
using FnD3D11Draw = void(__stdcall*)(
    ID3D11DeviceContext*, uint32_t, uint32_t);

void ReplayShadowDrawIndexed(
    ID3D11DeviceContext* ctx,
    FnD3D11DrawIndexed orig_fn,
    uint32_t index_count,
    uint32_t start_index,
    int32_t base_vertex);

void ReplayShadowDraw(
    ID3D11DeviceContext* ctx,
    FnD3D11Draw orig_fn,
    uint32_t vertex_count,
    uint32_t start_vertex);

bool ExecuteShadowResolvePass(
    ID3D11DeviceContext* ctx,
    ID3D11RenderTargetView* target_rtv,
    ID3D11ShaderResourceView* scene_depth_srv,
    const float* proj_4x4,
    int vp_left,
    int vp_top,
    int vp_width,
    int vp_height,
    float shadow_strength,
    PostLogFn logger);

bool VerifyShadowShadersCompile(PostLogFn logger);
void ReleaseShadowPassResources();

}  // namespace routeb
