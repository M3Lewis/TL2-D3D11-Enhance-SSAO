#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace routeb {

class Detour64 {
public:
    Detour64() = default;
    ~Detour64();

    Detour64(const Detour64&) = delete;
    Detour64& operator=(const Detour64&) = delete;

    bool Install(
        void* target,
        const uint8_t* expected_prefix,
        size_t stolen_len,
        void* hook_fn,
        void** out_trampoline,
        const char* expected_mask = nullptr);

    void Remove();
    bool IsInstalled() const { return installed_; }

private:
    void* target_ = nullptr;
    void* trampoline_ = nullptr;
    std::vector<uint8_t> orig_bytes_;
    bool installed_ = false;
};

class RawPatch64 {
public:
    RawPatch64() = default;
    ~RawPatch64();

    RawPatch64(const RawPatch64&) = delete;
    RawPatch64& operator=(const RawPatch64&) = delete;

    bool InstallJumpToAllocatedStub(
        void* target,
        const uint8_t* expected_bytes,
        size_t patch_len,
        const std::vector<uint8_t>& stub_code,
        const char* expected_mask = nullptr);

    void Remove();
    bool IsInstalled() const { return installed_; }

private:
    void* target_ = nullptr;
    void* stub_mem_ = nullptr;
    std::vector<uint8_t> orig_bytes_;
    bool installed_ = false;
};

// Scans all executable PE sections in module_base for a unique byte pattern
// (mask char '?' acts as a wildcard). Returns nullptr if 0 or >=2 matches exist.
void* FindUniqueCodePattern(
    void* module_base,
    const uint8_t* pattern,
    const char* mask,
    size_t len);

// Fast path: returns base + preferred_rva if expected_prefix matches inside an
// executable section; otherwise falls back to FindUniqueCodePattern.
void* ResolveCodeTarget(
    void* module_base,
    uintptr_t preferred_rva,
    const uint8_t* expected_prefix,
    size_t prefix_len,
    const uint8_t* scan_pattern,
    const char* scan_mask,
    size_t scan_len);

class IatHook {
public:
    IatHook() = default;
    ~IatHook();

    IatHook(const IatHook&) = delete;
    IatHook& operator=(const IatHook&) = delete;

    bool Install(
        void* module_base,
        const char* import_dll,
        const char* func_name,
        void* hook_fn,
        void** out_orig_fn);

    void Remove();
    bool IsInstalled() const { return installed_; }

private:
    void** iat_slot_ = nullptr;
    void* orig_fn_ = nullptr;
    bool installed_ = false;
};

}  // namespace routeb
