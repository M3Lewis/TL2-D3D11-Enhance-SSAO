#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
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
using MatGetByNameFn = void(__fastcall*)(void* mgr, OgreSharedPtrRaw* out_sp, const std::string& name, const std::string& group);
using ResLoadFn = void(__fastcall*)(void* res, bool background);
using MatGetTechFn = void*(__fastcall*)(void* mat, uint16_t idx);
using TechGetPassFn = void*(__fastcall*)(void* tech, uint16_t idx);
using PassSetNameFn = void(__fastcall*)(void* pass, const std::string& name);
using PassSetAlphaRejectFn = void(__fastcall*)(void* pass, int func, uint8_t val, bool alpha_to_cov);
using PassSetVpFn = void(__fastcall*)(void* pass, const std::string& name, bool reset_params);
using PassCreateTusFn = void*(__fastcall*)(void* pass);
using TusSetTexNameFn = void(__fastcall*)(void* tus, const std::string& name, int tex_type);
using SmCreateMoFn = void*(__fastcall*)(void* sm, const std::string& name);
using SmGetRootNodeFn = void*(__fastcall*)(void* sm);
using SnAttachObjFn = void(__fastcall*)(void* node, void* obj);
using MoBeginFn = void(__fastcall*)(void* mo, const std::string& mat_name, int op_type, const std::string& group_name);
using MoPosFn = void(__fastcall*)(void* mo, float x, float y, float z);
using MoNormFn = void(__fastcall*)(void* mo, float x, float y, float z);
using MoTc2Fn = void(__fastcall*)(void* mo, float u, float v);
using MoEndFn = void*(__fastcall*)(void* mo);
using SmSetPassFn = const void*(__fastcall*)(void* sm, const void* pass, bool even_if_suppressed, bool shadow_deriv);

bool ExercisePhase3PassLifecycle(HMODULE ogre, void* root, void* win) {
    CreateSceneMgrFn create_sm = nullptr;
    CreateCameraFn create_cam = nullptr;
    AddViewportFn add_vp = nullptr;
    RenderOneFrameFn render_one = nullptr;
    MatCreateFn mat_create = nullptr;
    MatGetByNameFn mat_get_by_name = nullptr;
    ResLoadFn res_load = nullptr;
    MatGetTechFn mat_get_tech = nullptr;
    TechGetPassFn tech_get_pass = nullptr;
    PassSetNameFn pass_set_name = nullptr;
    PassSetAlphaRejectFn pass_set_alpha = nullptr;
    PassSetVpFn pass_set_vp = nullptr;
    PassCreateTusFn pass_create_tus = nullptr;
    TusSetTexNameFn tus_set_tex_name = nullptr;
    SmCreateMoFn sm_create_mo = nullptr;
    SmGetRootNodeFn sm_get_root_node = nullptr;
    SnAttachObjFn sn_attach_obj = nullptr;
    MoBeginFn mo_begin = nullptr;
    MoPosFn mo_pos = nullptr;
    MoNormFn mo_norm = nullptr;
    MoTc2Fn mo_tc2 = nullptr;
    MoEndFn mo_end = nullptr;
    SmSetPassFn sm_set_pass = nullptr;
    void** mat_mgr_ptr = nullptr;

    bool ok = true;
    ok &= Resolve(ogre, "?createSceneManager@Root@Ogre@@QEAAPEAVSceneManager@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0@Z", create_sm);
    ok &= Resolve(ogre, "?createCamera@SceneManager@Ogre@@UEAAPEAVCamera@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z", create_cam);
    ok &= Resolve(ogre, "?addViewport@RenderTarget@Ogre@@UEAAPEAVViewport@2@PEAVCamera@2@HMMMM@Z", add_vp);
    ok &= Resolve(ogre, "?renderOneFrame@Root@Ogre@@QEAA_NXZ", render_one);
    ok &= Resolve(ogre, "?create@ResourceManager@Ogre@@UEAA?AV?$SharedPtr@VResource@Ogre@@@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0_NPEAVManualResourceLoader@2@PEBV?$map@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@V12@U?$less@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@2@V?$STLAllocator@U?$pair@$$CBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@V12@@std@@V?$CategorisedAllocPolicy@$07@Ogre@@@Ogre@@@5@@Z", mat_create);
    ok &= Resolve(ogre, "?getByName@ResourceManager@Ogre@@UEAA?AV?$SharedPtr@VResource@Ogre@@@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0@Z", mat_get_by_name);
    ok &= Resolve(ogre, "?load@Resource@Ogre@@UEAAX_N@Z", res_load);
    ok &= Resolve(ogre, "?getTechnique@Material@Ogre@@QEAAPEAVTechnique@2@G@Z", mat_get_tech);
    ok &= Resolve(ogre, "?getPass@Technique@Ogre@@QEAAPEAVPass@2@G@Z", tech_get_pass);
    ok &= Resolve(ogre, "?setName@Pass@Ogre@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z", pass_set_name);
    ok &= Resolve(ogre, "?setAlphaRejectSettings@Pass@Ogre@@QEAAXW4CompareFunction@2@E_N@Z", pass_set_alpha);
    ok &= Resolve(ogre, "?setVertexProgram@Pass@Ogre@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@_N@Z", pass_set_vp);
    ok &= Resolve(ogre, "?createTextureUnitState@Pass@Ogre@@QEAAPEAVTextureUnitState@2@XZ", pass_create_tus);
    ok &= Resolve(ogre, "?setTextureName@TextureUnitState@Ogre@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@W4TextureType@2@@Z", tus_set_tex_name);
    ok &= Resolve(ogre, "?createManualObject@SceneManager@Ogre@@UEAAPEAVManualObject@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z", sm_create_mo);
    ok &= Resolve(ogre, "?getRootSceneNode@SceneManager@Ogre@@UEAAPEAVSceneNode@2@XZ", sm_get_root_node);
    ok &= Resolve(ogre, "?attachObject@SceneNode@Ogre@@UEAAXPEAVMovableObject@2@@Z", sn_attach_obj);
    ok &= Resolve(ogre, "?begin@ManualObject@Ogre@@UEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@W4OperationType@RenderOperation@2@0@Z", mo_begin);
    ok &= Resolve(ogre, "?position@ManualObject@Ogre@@UEAAXMMM@Z", mo_pos);
    ok &= Resolve(ogre, "?normal@ManualObject@Ogre@@UEAAXMMM@Z", mo_norm);
    ok &= Resolve(ogre, "?textureCoord@ManualObject@Ogre@@UEAAXMM@Z", mo_tc2);
    ok &= Resolve(ogre, "?end@ManualObject@Ogre@@UEAAPEAVManualObjectSection@12@XZ", mo_end);
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
        {"Verify/RimLit3Cube", "RIMLIT", false, true, 3},
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
            void* tus = pass_create_tus(pass);
            if (tus && t == 0 && std::string(spec.mat_name) == "Verify/DiffOpaque") {
                tus_set_tex_name(tus, "FLOOR_01.dds", 2 /*TEX_TYPE_2D*/);
            }
            if (tus && std::string(spec.mat_name) == "Verify/RimLit3Cube") {
                if (t < 2) {
                    tus_set_tex_name(tus, "FLOOR_01.dds", 2 /*TEX_TYPE_2D*/);
                } else if (t == 2) {
                    tus_set_tex_name(tus, "RimCube.dds", 4 /*TEX_TYPE_CUBE_MAP*/);
                }
            }
        }
        passes.push_back(pass);
    }

    // Also verify P2 post-processing materials compile cleanly under D3D11 and exercise
    // SceneManager::_setPass P2 filter parameter forwarding with zero exceptions.
    const char* p2_mats[] = {
        "Ogre/Compositor/BrightPass2",
        "Ogre/Compositor/BloomBlend2",
        "TL2/RenderScale/FXAA",
    };
    for (const char* p2_mat_name : p2_mats) {
        OgreSharedPtrRaw sp{};
        mat_get_by_name(*mat_mgr_ptr, &sp, p2_mat_name, "Shaders");
        if (!sp.rep) {
            std::printf("[FAIL] Missing P2 material '%s' in Shaders group\n", p2_mat_name);
            return false;
        }
        res_load(sp.rep, false);
        void* tech = mat_get_tech(sp.rep, 0);
        void* pass = tech ? tech_get_pass(tech, 0) : nullptr;
        if (!pass) {
            std::printf("[FAIL] Missing Technique/Pass 0 on '%s'\n", p2_mat_name);
            return false;
        }
        passes.push_back(pass);
        mats.push_back(sp);
    }

    // Attach a simple textured triangle facing the camera so D3D11RenderSystem::_render executes
    // through DiffPass_ps.hlsl and our 0x179FF companion normal/spec SRV binding hook.
    void* mo = sm_create_mo(sm, "VerifyQuad");
    mo_begin(mo, "Verify/DiffOpaque", 4 /*OT_TRIANGLE_LIST*/, "Shaders");
    mo_pos(mo, -1.0f, -1.0f, -4.0f);
    mo_norm(mo, 0.0f, 0.0f, 1.0f);
    mo_tc2(mo, 0.0f, 1.0f);
    mo_pos(mo, 1.0f, -1.0f, -4.0f);
    mo_norm(mo, 0.0f, 0.0f, 1.0f);
    mo_tc2(mo, 1.0f, 1.0f);
    mo_pos(mo, 0.0f, 1.0f, -4.0f);
    mo_norm(mo, 0.0f, 0.0f, 1.0f);
    mo_tc2(mo, 0.5f, 0.0f);
    mo_end(mo);
    sn_attach_obj(sm_get_root_node(sm), mo);

    // Bind every Phase 3 explicit PS archetype repeatedly across frames to catch any
    // SharedPtr<GpuProgramParameters> premature destruction or constant buffer corruption.
    for (int iter = 0; iter < 12; ++iter) {
        for (void* pass : passes) {
            sm_set_pass(sm, pass, true, false);
        }
        if (iter == 11) {
            if (HMODULE route_b = ::GetModuleHandleW(L"Plugin_TL2_DX11_Enhance_SSAO.dll")) {
                using TriggerFn = void (*)();
                if (auto fn = reinterpret_cast<TriggerFn>(::GetProcAddress(route_b, "RouteB_TriggerToastForTest"))) {
                    fn();
                }
            }
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
            load_plugin(root, ".\\Plugin_TL2_DX11_Enhance_SSAO");
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
        void* win = create_win(root, "RouteBVerify", is_d3d11 ? 400 : 64, is_d3d11 ? 80 : 64, false, nullptr);

        void* rgm = *rgm_singleton_ptr;
        add_res_loc(rgm, "programs/", "FileSystem", "Shaders", true);
        init_res_grp(rgm, "Shaders");
        if (is_d3d11) {
            char exe_path[MAX_PATH] = {};
            ::GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
            std::string exe_dir(exe_path);
            const size_t slash = exe_dir.find_last_of("\\/");
            if (slash != std::string::npos) {
                exe_dir.resize(slash);
            }
            const std::string test_tex_dir = exe_dir + R"(\test_norm_mod\MEDIA\LEVELSETS\CATACOMB)";
            if (::GetFileAttributesA(test_tex_dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
                add_res_loc(rgm, test_tex_dir, "FileSystem", "General", true);
                init_res_grp(rgm, "General");
            } else {
                std::printf("[WARN] missing test_tex_dir: %s\n", test_tex_dir.c_str());
            }
        }
        if (is_d3d11 && !ExercisePhase3PassLifecycle(ogre, root, win)) {
            exit_code = 5;
        }
        std::printf("[PASS] %s Shaders resource group parsed and initialised with zero exceptions!\n", mode.c_str());
    } catch (const std::exception& ex) {
        std::printf("[FAIL] %s threw exception: %s\n", mode.c_str(), ex.what());
        exit_code = 4;
    }

    root_dtor(root);

    if (is_d3d11 && exit_code == 0) {
        std::ifstream ifs("TL2_RouteB_D3D11.log", std::ios::binary);
        const std::string route_log((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        const size_t diag_pos = route_log.find("[RouteC][Toast] Diag first draw");
        if (diag_pos == std::string::npos || route_log.find("drawn=1", diag_pos) == std::string::npos) {
            std::printf("[FAIL] d3d11 toast diag log missing drawn=1 in TL2_RouteB_D3D11.log\n");
            return 6;
        }
        if (route_log.find("normSpecSrv=1") == std::string::npos ||
            route_log.find("normHits=0") != std::string::npos ||
            route_log.find("drawBinds=0") != std::string::npos ||
            route_log.find("[RouteB][Phase3][Companion]") == std::string::npos) {
            std::printf("[FAIL] d3d11 companion normal/spec SRV binding did not activate in TL2_RouteB_D3D11.log\n");
            return 7;
        }
        if (route_log.find("rim3Cube=0") != std::string::npos ||
            route_log.find("rim3Cube=") == std::string::npos) {
            std::printf("[FAIL] d3d11 RimLit3CubePS 3-stage bind did not activate in TL2_RouteB_D3D11.log\n");
            return 8;
        }
        std::printf("[PASS] d3d11 toast overlay + companion _tl2n/_tl2s + RimLit3CubePS D3D11 draw binding verified cleanly!\n");
    }
    return exit_code;
}
