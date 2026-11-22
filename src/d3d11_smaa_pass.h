#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <cstdint>

#include "d3d11_post_types.h"

namespace routeb {

struct SmaaRequest {
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* fallback_rtv = nullptr;
    int vp_left = 0;
    int vp_top = 0;
    int vp_width = 0;
    int vp_height = 0;
    int smaa_mode = 0;  // 0=OFF, 1=Color, 2=Depth+Color
    float proj_22 = 0.0f;
    float proj_23 = 0.0f;
};

void SetSmaaMode(int mode);
int GetSmaaMode();
int CycleSmaaMode();
const char* SmaaModeName(int mode);

struct SmaaStats {
    uint64_t dispatches = 0;
    uint64_t executed = 0;
};
SmaaStats GetSmaaStats();

bool ExecuteSmaaPass(const SmaaRequest& req, PostLogFn logger);
void ReleaseSmaaTargets();
void ReleaseSmaaAllObjects();
bool VerifySmaaShadersCompile(ID3D11Device* dev, PostLogFn logger);

}  // namespace routeb
