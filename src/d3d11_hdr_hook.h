#pragma once

#include "d3d11_post_types.h"

struct ID3D11Device;

namespace routeb {

bool InstallHdrDeviceIatHook(PostLogFn logger);
void RemoveHdrDeviceIatHook();

// Idempotent hook helper for ID3D11Device COM vtable slots (CreateTexture2D / SRV / RTV).
void EnsureHdrDeviceVtableHooked(ID3D11Device* dev, PostLogFn logger);

void BeginHdrUpgradeBypass();
void EndHdrUpgradeBypass();

class HdrUpgradeBypassScope {
public:
    HdrUpgradeBypassScope() { BeginHdrUpgradeBypass(); }
    ~HdrUpgradeBypassScope() { EndHdrUpgradeBypass(); }
    HdrUpgradeBypassScope(const HdrUpgradeBypassScope&) = delete;
    HdrUpgradeBypassScope& operator=(const HdrUpgradeBypassScope&) = delete;
};

bool IsHdrPipelineActive();
int GetHdrUpgradedTextureCount();

}  // namespace routeb
