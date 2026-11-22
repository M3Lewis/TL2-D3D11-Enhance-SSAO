#pragma once

#include <cstdint>
#include "d3d11_post_types.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;

namespace routeb {

struct KawaseBloomRequest {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* target_rtv = nullptr;
    ID3D11ShaderResourceView* scene_srv = nullptr;  // p2_copy_srv containing current world color
    uint32_t rt_w = 0;
    uint32_t rt_h = 0;
    int vp_left = 0;
    int vp_top = 0;
    int vp_width = 0;
    int vp_height = 0;
    int filter_preset = 1;       // 0=OFF, 1=HOB, 2=DRK
    float strength_scale = 1.0f; // 0.0 .. 2.0
    bool is_desert = false;
    bool is_menu = false;
};

struct BloomStats {
    uint64_t dispatches = 0;
    uint64_t executed = 0;
};

bool ExecuteKawaseBloomPass(const KawaseBloomRequest& req, PostLogFn logger);
bool VerifyBloomShadersCompile(PostLogFn logger);
void ReleaseBloomTargets();
void ReleaseBloomAllObjects();
BloomStats GetBloomStats();

}  // namespace routeb
