#include "hook_utils.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

namespace routeb {

namespace {

void WriteAbsJump14(uint8_t* dst, uint64_t addr) {
    dst[0] = 0xFF;
    dst[1] = 0x25;
    dst[2] = 0x00;
    dst[3] = 0x00;
    dst[4] = 0x00;
    dst[5] = 0x00;
    std::memcpy(dst + 6, &addr, sizeof(addr));
}

bool WriteProtectedBytes(void* dst, const uint8_t* bytes, size_t len) {
    DWORD old_prot = 0;
    if (!::VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old_prot)) {
        return false;
    }
    std::memcpy(dst, bytes, len);
    DWORD unused = 0;
    ::VirtualProtect(dst, len, old_prot, &unused);
    ::FlushInstructionCache(::GetCurrentProcess(), dst, len);
    return true;
}

bool MatchBytesMasked(const uint8_t* actual, const uint8_t* expected, const char* mask, size_t len) {
    if (!mask) {
        return std::memcmp(actual, expected, len) == 0;
    }
    for (size_t i = 0; i < len; ++i) {
        if (mask[i] != '?' && actual[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

const IMAGE_NT_HEADERS64* GetPe64Headers(const uint8_t* base) {
    if (!base) {
        return nullptr;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return nullptr;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return nullptr;
    }
    return nt;
}

bool IsRvaInExecutableSection(const uint8_t* base, const IMAGE_NT_HEADERS64* nt, uintptr_t rva, size_t len) {
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    const WORD num_sec = nt->FileHeader.NumberOfSections;
    for (WORD i = 0; i < num_sec; ++i) {
        if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        const uintptr_t sec_start = sec[i].VirtualAddress;
        const size_t sec_size = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        if (rva >= sec_start && (rva - sec_start) <= sec_size && len <= (sec_size - (rva - sec_start))) {
            return (base + rva) != nullptr;
        }
    }
    return false;
}

}  // namespace

void* FindUniqueCodePattern(
    void* module_base,
    const uint8_t* pattern,
    const char* mask,
    size_t len) {
    if (!module_base || !pattern || len == 0) {
        return nullptr;
    }
    const auto* base = static_cast<const uint8_t*>(module_base);
    const IMAGE_NT_HEADERS64* nt = GetPe64Headers(base);
    if (!nt) {
        return nullptr;
    }

    const uint8_t* unique_match = nullptr;
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    const WORD num_sec = nt->FileHeader.NumberOfSections;
    for (WORD s = 0; s < num_sec; ++s) {
        if ((sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        const size_t sec_size = sec[s].Misc.VirtualSize ? sec[s].Misc.VirtualSize : sec[s].SizeOfRawData;
        if (sec_size < len) {
            continue;
        }
        const uint8_t* sec_bytes = base + sec[s].VirtualAddress;
        const size_t limit = sec_size - len;
        for (size_t i = 0; i <= limit; ++i) {
            if (MatchBytesMasked(sec_bytes + i, pattern, mask, len)) {
                if (unique_match != nullptr) {
                    return nullptr;  // Ambiguous pattern: refuse to guess.
                }
                unique_match = sec_bytes + i;
            }
        }
    }
    return const_cast<uint8_t*>(unique_match);
}

void* ResolveCodeTarget(
    void* module_base,
    uintptr_t preferred_rva,
    const uint8_t* expected_prefix,
    size_t prefix_len,
    const uint8_t* scan_pattern,
    const char* scan_mask,
    size_t scan_len) {
    if (!module_base) {
        return nullptr;
    }
    const auto* base = static_cast<const uint8_t*>(module_base);
    const IMAGE_NT_HEADERS64* nt = GetPe64Headers(base);
    if (!nt) {
        return nullptr;
    }

    if (preferred_rva != 0 && expected_prefix && prefix_len > 0 &&
        IsRvaInExecutableSection(base, nt, preferred_rva, prefix_len)) {
        const uint8_t* candidate = base + preferred_rva;
        const char* prefix_mask = (scan_pattern == expected_prefix) ? scan_mask : nullptr;
        if (MatchBytesMasked(candidate, expected_prefix, prefix_mask, prefix_len)) {
            return const_cast<uint8_t*>(candidate);
        }
    }

    if (scan_pattern && scan_len > 0) {
        return FindUniqueCodePattern(module_base, scan_pattern, scan_mask, scan_len);
    }
    return nullptr;
}

Detour64::~Detour64() {
    Remove();
}

bool Detour64::Install(
    void* target,
    const uint8_t* expected_prefix,
    size_t stolen_len,
    void* hook_fn,
    void** out_trampoline,
    const char* expected_mask) {
    if (installed_ || !target || !expected_prefix || stolen_len < 14 || !hook_fn || !out_trampoline) {
        return false;
    }

    if (!MatchBytesMasked(static_cast<const uint8_t*>(target), expected_prefix, expected_mask, stolen_len)) {
        return false;
    }

    auto* tramp = static_cast<uint8_t*>(
        ::VirtualAlloc(nullptr, stolen_len + 14, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) {
        return false;
    }

    std::memcpy(tramp, target, stolen_len);
    const uint64_t resume_addr = reinterpret_cast<uint64_t>(target) + stolen_len;
    WriteAbsJump14(tramp + stolen_len, resume_addr);
    ::FlushInstructionCache(::GetCurrentProcess(), tramp, stolen_len + 14);

    orig_bytes_.assign(
        static_cast<const uint8_t*>(target),
        static_cast<const uint8_t*>(target) + stolen_len);

    std::vector<uint8_t> patch(stolen_len, 0x90);
    WriteAbsJump14(patch.data(), reinterpret_cast<uint64_t>(hook_fn));

    if (!WriteProtectedBytes(target, patch.data(), patch.size())) {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        orig_bytes_.clear();
        return false;
    }

    target_ = target;
    trampoline_ = tramp;
    *out_trampoline = tramp;
    installed_ = true;
    return true;
}

void Detour64::Remove() {
    if (!installed_) {
        return;
    }
    if (target_ && !orig_bytes_.empty()) {
        (void)WriteProtectedBytes(target_, orig_bytes_.data(), orig_bytes_.size());
    }
    if (trampoline_) {
        ::VirtualFree(trampoline_, 0, MEM_RELEASE);
        trampoline_ = nullptr;
    }
    orig_bytes_.clear();
    target_ = nullptr;
    installed_ = false;
}

RawPatch64::~RawPatch64() {
    Remove();
}

bool RawPatch64::InstallJumpToAllocatedStub(
    void* target,
    const uint8_t* expected_bytes,
    size_t patch_len,
    const std::vector<uint8_t>& stub_code,
    const char* expected_mask) {
    if (installed_ || !target || !expected_bytes || patch_len < 14 || stub_code.empty()) {
        return false;
    }
    if (!MatchBytesMasked(static_cast<const uint8_t*>(target), expected_bytes, expected_mask, patch_len)) {
        return false;
    }

    auto* stub = static_cast<uint8_t*>(
        ::VirtualAlloc(nullptr, stub_code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) {
        return false;
    }
    std::memcpy(stub, stub_code.data(), stub_code.size());
    ::FlushInstructionCache(::GetCurrentProcess(), stub, stub_code.size());

    orig_bytes_.assign(
        static_cast<const uint8_t*>(target),
        static_cast<const uint8_t*>(target) + patch_len);

    std::vector<uint8_t> patch(patch_len, 0x90);
    WriteAbsJump14(patch.data(), reinterpret_cast<uint64_t>(stub));

    if (!WriteProtectedBytes(target, patch.data(), patch.size())) {
        ::VirtualFree(stub, 0, MEM_RELEASE);
        orig_bytes_.clear();
        return false;
    }

    target_ = target;
    stub_mem_ = stub;
    installed_ = true;
    return true;
}

void RawPatch64::Remove() {
    if (!installed_) {
        return;
    }
    if (target_ && !orig_bytes_.empty()) {
        (void)WriteProtectedBytes(target_, orig_bytes_.data(), orig_bytes_.size());
    }
    if (stub_mem_) {
        ::VirtualFree(stub_mem_, 0, MEM_RELEASE);
        stub_mem_ = nullptr;
    }
    orig_bytes_.clear();
    target_ = nullptr;
    installed_ = false;
}

IatHook::~IatHook() {
    Remove();
}

bool IatHook::Install(
    void* module_base,
    const char* import_dll,
    const char* func_name,
    void* hook_fn,
    void** out_orig_fn) {
    if (installed_ || !module_base || !import_dll || !func_name || !hook_fn || !out_orig_fn) {
        return false;
    }

    const auto base = reinterpret_cast<uintptr_t>(module_base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) {
        return false;
    }

    const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        const char* dll = reinterpret_cast<const char*>(base + desc->Name);
        if (::_stricmp(dll, import_dll) != 0) {
            continue;
        }

        const auto* orig_thunk = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
            base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto* first_thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);

        for (; orig_thunk->u1.AddressOfData != 0; ++orig_thunk, ++first_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(orig_thunk->u1.Ordinal)) {
                continue;
            }
            const auto* imp = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + orig_thunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(imp->Name), func_name) != 0) {
                continue;
            }

            void** slot = reinterpret_cast<void**>(&first_thunk->u1.Function);
            void* prev = *slot;
            DWORD old_prot = 0;
            if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
                return false;
            }
            *slot = hook_fn;
            DWORD unused = 0;
            ::VirtualProtect(slot, sizeof(void*), old_prot, &unused);

            iat_slot_ = slot;
            orig_fn_ = prev;
            *out_orig_fn = prev;
            installed_ = true;
            return true;
        }
    }
    return false;
}

void IatHook::Remove() {
    if (!installed_ || !iat_slot_) {
        return;
    }
    DWORD old_prot = 0;
    if (::VirtualProtect(iat_slot_, sizeof(void*), PAGE_READWRITE, &old_prot)) {
        *iat_slot_ = orig_fn_;
        DWORD unused = 0;
        ::VirtualProtect(iat_slot_, sizeof(void*), old_prot, &unused);
    }
    iat_slot_ = nullptr;
    orig_fn_ = nullptr;
    installed_ = false;
}

}  // namespace routeb
