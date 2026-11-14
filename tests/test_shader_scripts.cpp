#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace {

using RootCtorFn = void*(__fastcall*)(void* mem, const std::string& pluginCfg, const std::string& cfgFile, const std::string& logFile);
using RootDtorFn = void(__fastcall*)(void* root);
using LoadPluginFn = void(__fastcall*)(void* root, const std::string& pluginName);
using GetRsByNameFn = void*(__fastcall*)(void* root, const std::string& name);
using SetRsFn = void(__fastcall*)(void* root, void* rs);
using InitRootFn = void*(__fastcall*)(void* root, bool autoCreateWindow, const std::string& title, const std::string& customCaps);
using CreateWinFn = void*(__fastcall*)(void* root, const std::string& name, unsigned int w, unsigned int h, bool fullScreen, const void* miscParams);
using AddResLocFn = void(__fastcall*)(void* rgm, const std::string& path, const std::string& type, const std::string& group, bool recursive);
using InitResGroupFn = void(__fastcall*)(void* rgm, const std::string& group);

template <typename Fn>
bool Resolve(HMODULE mod, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(::GetProcAddress(mod, name));
    if (!out) {
        std::printf("FAILED GetProcAddress: %s\n", name);
        return false;
    }
    return true;
}

struct OgreSharedPtrRaw {
    void* vftable = nullptr;
    void* rep = nullptr;
    long* use_count = nullptr;
    int free_method = 0;
};

using CreateSceneMgrFn = void*(__fastcall*)(void* root, const std::string& type_name, const std::string& inst_name);
using CreateCameraFn = void*(__fastcall*)(void* sm, const std::string& name);
using AddViewportFn = void*(__fastcall*)(void* rt, void* cam, int zorder, float left, float top, float width, float height);
using RenderOneFrameFn = bool(__fastcall*)(void* root);
using MatCreateFn = void(__fastcall*)(void* mgr, OgreSharedPtrRaw* out_sp, const std::string& name, const std::string& group, bool is_manual, void* loader, const void* create_params);
using MatGetTechFn = void*(__fastcall*)(void* mat, uint16_t idx);
using TechGetPassFn = void*(__fastcall*)(void* tech, uint16_t idx);
using PassSetNameFn = void(__fastcall*)(void* pass, const std::string& name);
using PassSetAlphaRejectFn = void(__fastcall*)(void* pass, int func, uint8_t val, bool alpha_to_cov);
using PassSetVpFn = void(__fastcall*)(void* pass, const std::string& name, bool reset_params);
using PassCreateTusFn = void*(__fastcall*)(void* pass);
using SmSetPassFn = const void*(__fastcall*)(void* sm, const void* pass, bool even_if_suppressed, bool shadow_deriv);

bool ExercisePhase3PassLifecycle(HMODULE ogre, void* root, void* win) {
    CreateSceneMgrFn create_sm = nullptr;
    CreateCameraFn create_cam = nullptr;
    AddViewportFn add_vp = nullptr;
    RenderOneFrameFn render_one = nullptr;
    MatCreateFn mat_create = nullptr;
    MatGetTechFn mat_get_tech = nullptr;
    TechGetPassFn tech_get_pass = nullptr;
    PassSetNameFn pass_set_name = nullptr;
    PassSetAlphaRejectFn pass_set_alpha = nullptr;
    PassSetVpFn pass_set_vp = nullptr;
    PassCreateTusFn pass_create_tus = nullptr;
    SmSetPassFn sm_set_pass = nullptr;
    void** mat_mgr_ptr = nullptr;

    bool ok = true;
    ok &= Resolve(ogre, "?createSceneManager@Root@Ogre@@QEAAPEAVSceneManager@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0@Z", create_sm);
    ok &= Resolve(ogre, "?createCamera@SceneManager@Ogre@@UEAAPEAVCamera@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z", create_cam);
    ok &= Resolve(ogre, "?addViewport@RenderTarget@Ogre@@UEAAPEAVViewport@2@PEAVCamera@2@HMMMM@Z", add_vp);
    ok &= Resolve(ogre, "?renderOneFrame@Root@Ogre@@QEAA_NXZ", render_one);
    ok &= Resolve(ogre, "?create@ResourceManager@Ogre@@UEAA?AV?$SharedPtr@VResource@Ogre@@@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0_NPEAVManualResourceLoader@2@PEBV?$map@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@V12@U?$less@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@2@V?$STLAllocator@U?$pair@$$CBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@V12@@std@@V?$CategorisedAllocPolicy@$07@Ogre@@@Ogre@@@5@@Z", mat_create);
    ok &= Resolve(ogre, "?getTechnique@Material@Ogre@@QEAAPEAVTechnique@2@G@Z", mat_get_tech);
    ok &= Resolve(ogre, "?getPass@Technique@Ogre@@QEAAPEAVPass@2@G@Z", tech_get_pass);
    ok &= Resolve(ogre, "?setName@Pass@Ogre@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z", pass_set_name);
    ok &= Resolve(ogre, "?setAlphaRejectSettings@Pass@Ogre@@QEAAXW4CompareFunction@2@E_N@Z", pass_set_alpha);
    ok &= Resolve(ogre, "?setVertexProgram@Pass@Ogre@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@_N@Z", pass_set_vp);
    ok &= Resolve(ogre, "?createTextureUnitState@Pass@Ogre@@QEAAPEAVTextureUnitState@2@XZ", pass_create_tus);
    ok &= Resolve(ogre, "?_setPass@SceneManager@Ogre@@UEAAPEBVPass@2@PEBV32@_N1@Z", sm_set_pass);
    ok &= Resolve(ogre, "?ms_Singleton@?$Singleton@VMaterialManager@Ogre@@@Ogre@@1PEAVMaterialManager@2@EA", mat_mgr_ptr);
    if (!ok || !*mat_mgr_ptr) {
        std::printf("[FAIL] Could not resolve SceneManager/MaterialManager exports for Phase 3 lifecycle check\n");
        return false;
    }

    void* sm = create_sm(root, "DefaultSceneManager", "RouteBVerifySM");
    void* cam = create_cam(sm, "VerifyCam");
    add_vp(win, cam, 0, 0.0f, 0.0f, 1.0f, 1.0f);

    struct PassSpec {
        const char* mat_name;
        const char* pass_name;
        bool alpha_reject;
        bool skinned;
        int tus_count;
    };
    const PassSpec specs[] = {
        {"Verify/DiffOpaque", "DIFFPASS", false, false, 1},
        {"Verify/DiffAlpha", "DIFFPASS", true, false, 1},
        {"Verify/SkinnedOpaque", "DIFFPASS", false, true, 1},
        {"Verify/SkinnedAlpha", "DIFFPASS", true, true, 1},
        {"Verify/LightPass", "LIGHTPASS", false, false, 1},
        {"Verify/RimLit2D", "RIMLIT", false, false, 2},
    };

    std::vector<void*> passes;
    std::vector<OgreSharedPtrRaw> mats(std::size(specs));
    for (size_t i = 0; i < std::size(specs); ++i) {
        const auto& spec = specs[i];
        mat_create(*mat_mgr_ptr, &mats[i], spec.mat_name, "Shaders", false, nullptr, nullptr);
        void* tech = mat_get_tech(mats[i].rep, 0);
        void* pass = tech_get_pass(tech, 0);
        pass_set_name(pass, spec.pass_name);
        if (spec.alpha_reject) {
            pass_set_alpha(pass, 7 /*CMPF_GREATER*/, 128, false);
        }
        if (spec.skinned) {
            pass_set_vp(pass, "Ogre/HardwareSkinningOne", true);
        }
        for (int t = 0; t < spec.tus_count; ++t) {
            pass_create_tus(pass);
        }
        passes.push_back(pass);
    }

    // Bind every Phase 3 explicit PS archetype repeatedly across frames to catch any
    // SharedPtr<GpuProgramParameters> premature destruction or constant buffer corruption.
    for (int iter = 0; iter < 12; ++iter) {
        for (void* pass : passes) {
            sm_set_pass(sm, pass, true, false);
        }
        render_one(root);
    }

    for (auto& sp : mats) {
        if (sp.use_count && ::InterlockedDecrement(sp.use_count) == 0) {
            auto** vtbl = static_cast<void(**)(void*)>(sp.vftable);
            if (vtbl && vtbl[1]) {
                vtbl[1](&sp);
            }
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("Usage: test_shader_scripts.exe <d3d9|d3d11>\n");
        return 1;
    }
    const std::string mode = argv[1];
    const bool is_d3d11 = (mode == "d3d11");
    if (!is_d3d11 && mode != "d3d9") {
        std::printf("Unknown mode: %s\n", mode.c_str());
        return 1;
    }

    ::SetDllDirectoryW(LR"(H:\Steam\steamapps\Common\Torchlight II)");
    ::SetCurrentDirectoryW(LR"(H:\Steam\steamapps\Common\Torchlight II)");

    HMODULE ogre = ::LoadLibraryW(L"OgreMain.dll");
    if (!ogre) {
        std::printf("FAILED LoadLibrary OgreMain.dll err=%lu\n", ::GetLastError());
        return 1;
    }

    RootCtorFn root_ctor = nullptr;
    RootDtorFn root_dtor = nullptr;
    LoadPluginFn load_plugin = nullptr;
    GetRsByNameFn get_rs_by_name = nullptr;
    SetRsFn set_rs = nullptr;
    InitRootFn init_root = nullptr;
    CreateWinFn create_win = nullptr;
    AddResLocFn add_res_loc = nullptr;
    InitResGroupFn init_res_grp = nullptr;
    void** rgm_singleton_ptr = nullptr;

    bool ok = true;
    ok &= Resolve(ogre, "??0Root@Ogre@@QEAA@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@00@Z", root_ctor);
    ok &= Resolve(ogre, "??1Root@Ogre@@QEAA@XZ", root_dtor);
    ok &= Resolve(ogre, "?loadPlugin@Root@Ogre@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z", load_plugin);
    ok &= Resolve(ogre, "?getRenderSystemByName@Root@Ogre@@QEAAPEAVRenderSystem@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z", get_rs_by_name);
    ok &= Resolve(ogre, "?setRenderSystem@Root@Ogre@@QEAAXPEAVRenderSystem@2@@Z", set_rs);
    ok &= Resolve(ogre, "?initialise@Root@Ogre@@QEAAPEAVRenderWindow@2@_NAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@1@Z", init_root);
    ok &= Resolve(ogre, "?createRenderWindow@Root@Ogre@@QEAAPEAVRenderWindow@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@II_NPEBV?$map@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@V12@U?$less@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@2@V?$STLAllocator@U?$pair@$$CBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@V12@@std@@V?$CategorisedAllocPolicy@$07@Ogre@@@Ogre@@@5@@Z", create_win);
    ok &= Resolve(ogre, "?addResourceLocation@ResourceGroupManager@Ogre@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@00_N@Z", add_res_loc);
    ok &= Resolve(ogre, "?initialiseResourceGroup@ResourceGroupManager@Ogre@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z", init_res_grp);
    ok &= Resolve(ogre, "?ms_Singleton@?$Singleton@VResourceGroupManager@Ogre@@@Ogre@@1PEAVResourceGroupManager@2@EA", rgm_singleton_ptr);
    if (!ok) {
        return 2;
    }

    std::vector<uint8_t> root_mem(65536, 0);
    void* root = root_mem.data();
    const std::string empty;
    const std::string log_path = is_d3d11
        ? R"(J:\Software\TL2-RE\自制2x高清mod 20260705\源文件\mods\tl2_d3d11_route_b\obj\ogre_verify_d3d11.log)"
        : R"(J:\Software\TL2-RE\自制2x高清mod 20260705\源文件\mods\tl2_d3d11_route_b\obj\ogre_verify_d3d9.log)";

    int exit_code = 0;
    try {
        root_ctor(root, empty, empty, log_path);
        if (is_d3d11) {
            load_plugin(root, ".\\RenderSystem_Direct3D11");
            load_plugin(root, ".\\Plugin_TL2_RouteB");
        } else {
            load_plugin(root, ".\\RenderSystem_Direct3D9");
            load_plugin(root, ".\\Plugin_CgProgramManager");
        }

        const std::string rs_name = is_d3d11
            ? "Direct3D11 Rendering Subsystem"
            : "Direct3D9 Rendering Subsystem";
        void* rs = get_rs_by_name(root, rs_name);
        if (!rs) {
            std::printf("FAILED getRenderSystemByName(%s)\n", rs_name.c_str());
            root_dtor(root);
            return 3;
        }
        set_rs(root, rs);
        init_root(root, false, "RouteBVerify", empty);
        void* win = create_win(root, "RouteBVerify", 64, 64, false, nullptr);

        void* rgm = *rgm_singleton_ptr;
        add_res_loc(rgm, "programs/", "FileSystem", "Shaders", true);
        init_res_grp(rgm, "Shaders");
        if (is_d3d11 && !ExercisePhase3PassLifecycle(ogre, root, win)) {
            exit_code = 5;
        }
        std::printf("[PASS] %s Shaders resource group parsed and initialised with zero exceptions!\n", mode.c_str());
    } catch (const std::exception& ex) {
        std::printf("[FAIL] %s threw exception: %s\n", mode.c_str(), ex.what());
        exit_code = 4;
    }

    root_dtor(root);
    return exit_code;
}
