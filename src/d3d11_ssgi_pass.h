#pragma once

#include <cstdint>
#include "d3d11_post_types.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;

namespace routeb {

struct SsgiRequest {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* target_rtv = nullptr;
    ID3D11ShaderResourceView* scene_srv = nullptr;
    ID3D11ShaderResourceView* depth_srv = nullptr;
    ID3D11ShaderResourceView* water_depth_srv = nullptr;
    uint32_t rt_w = 0;
    uint32_t rt_h = 0;
    int vp_left = 0;
    int vp_top = 0;
    int vp_width = 0;
    int vp_height = 0;
    float strength_scale = 1.0f;
    bool use_msaa_depth = false;
    bool is_desert = false;
    bool is_menu = false;
};

struct SsgiStats {
    uint64_t executed = 0;
};

bool ExecuteSsgiPass(const SsgiRequest& req, PostLogFn logger);
bool VerifySsgiShadersCompile(PostLogFn logger);
void ReleaseSsgiTargets();
void ReleaseSsgiAllObjects();
SsgiStats GetSsgiStats();

}  // namespace routeb
