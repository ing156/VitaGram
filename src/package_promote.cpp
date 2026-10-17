#include "package_promote.h"
#include <cstdint>
#include <cstdio>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <sys/stat.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>

namespace {
int g_paf = 0;
// Modules stay loaded afterwards: the process ends soon, and unloading PAF right after a promote has
// been seen to upset the shell.
bool ensure_modules(std::string& detail) {
    static bool loaded_once = false;
    if (loaded_once) return true;
    char text[200];
    // The promoter is built on the system's PAF framework: like VitaShell, load it first (with its
    // heap size) and only then the promoter module.
    static uint32_t paf_args[] = { 0x180000, (uint32_t)-1, (uint32_t)-1, 1, (uint32_t)-1, (uint32_t)-1 };
    static int paf_result = -1;
    static SceSysmoduleOpt paf_option;
    paf_option = SceSysmoduleOpt{};
    paf_option.flags = (int)sizeof(paf_option);
    paf_option.result = &paf_result;
    paf_option.unused[0] = paf_option.unused[1] = -1;
    g_paf = sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof(paf_args), paf_args, &paf_option);
    int rc = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (rc < 0) {
        // Already loaded counts as loaded; the diagnostics line keeps all codes for a bug report.
        int loaded = sceSysmoduleIsLoadedInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
        if (loaded != SCE_SYSMODULE_LOADED) {
            std::snprintf(text, sizeof(text), "promoter module: 0x%08X (loaded: 0x%08X, paf: 0x%08X)", rc, loaded, g_paf);
            detail = text;
            return false;
        }
    }
    loaded_once = true;
    return true;
}
}

bool delete_package(const char* title_id, std::string& detail) {
    if (!ensure_modules(detail)) return false;
    int rc = scePromoterUtilityInit();
    if (rc < 0) { char text[80]; std::snprintf(text, sizeof(text), "promoter init: 0x%08X", rc); detail = text; return false; }
    rc = scePromoterUtilityDeletePkg(title_id);
    if (rc >= 0) {
        for (int waited = 0; waited < 2000; ++waited) { // up to 20 s, until the promoter is idle again
            int state = 0;
            if (scePromoterUtilityGetState(&state) < 0 || state == 0) break;
            sceKernelDelayThread(10 * 1000);
        }
    }
    scePromoterUtilityExit();
    if (rc < 0) { char text[80]; std::snprintf(text, sizeof(text), "promoter delete: 0x%08X", rc); detail = text; return false; }
    return true;
}

bool promote_package(const std::string& package_dir, std::string& detail, const char* installed_eboot, long long expected_size) {
    char text[200];
    if (!ensure_modules(detail)) return false;
    const int paf = g_paf;

    int rc = scePromoterUtilityInit();
    if (rc < 0) {
        std::snprintf(text, sizeof(text), "promoter init: 0x%08X (paf: 0x%08X)", rc, paf);
        detail = text;
        return false;
    }
    // Two ways to promote, tried in turn. 1) Plain asynchronous promote, polled until the state returns to
    // idle - what PSVitaAlive's self-updater does on hardware (it reports that the synchronous form hung).
    // 2) Synchronous promote that also writes a rif - what VitaShell calls.
    struct Attempt { const char* name; bool with_rif; };
    std::string failures;
    bool ok = false;
    // Codes of the last attempt, kept for diagnostics: callers check the installed files afterwards and
    // need to know what the promoter said when it reported success but nothing was installed.
    int last_state_rc = 0, last_state = 0, last_result = 0, waited_ms = 0;
    for (const Attempt& attempt : {Attempt{"promote", false}, Attempt{"promote+rif", true}}) {
        const char* stage = attempt.name;
        if (attempt.with_rif) rc = scePromoterUtilityPromotePkgWithRif(package_dir.c_str(), 1);
        else rc = scePromoterUtilityPromotePkg(package_dir.c_str(), 0);
        if (rc >= 0) {
            stage = "result";
            for (waited_ms = 0; waited_ms < 120000; waited_ms += 10) { // up to 120 s, until the promoter is idle again
                last_state = 0;
                last_state_rc = scePromoterUtilityGetState(&last_state);
                if (last_state_rc < 0 || last_state == 0) break;
                sceKernelDelayThread(10 * 1000);
            }
            int result = 0;
            rc = scePromoterUtilityGetResult(&result);
            last_result = rc < 0 ? rc : result;
            if (rc >= 0) rc = result;
            if (rc < 0 && installed_eboot && expected_size >= 0) {
                struct stat info;
                if (stat(installed_eboot, &info) == 0 && (long long)info.st_size == expected_size) {
                    std::snprintf(text, sizeof(text), "result 0x%08X ignored: %s has the new size", (unsigned)rc, installed_eboot);
                    detail = text; // diagnostics only
                    rc = 0;
                }
            }
        }
        if (rc >= 0) {
            ok = true;
            if (detail.empty()) {
                std::snprintf(text, sizeof(text), "%s ok: result 0x%08X, state %d (rc 0x%08X) after %d ms", stage,
                              (unsigned)last_result, last_state, (unsigned)last_state_rc, waited_ms);
                detail = text; // diagnostics only
            }
            break;
        }
        std::snprintf(text, sizeof(text), "%s%s %s: 0x%08X", failures.empty() ? "" : "; ", attempt.with_rif ? "with rif," : "plain,", stage, rc);
        failures += text;
    }
    scePromoterUtilityExit(); // soft exit only: the modules stay loaded
    if (!ok) {
        // Sizes of the files the promoter looks at first: a missing or empty one explains the code.
        auto size_of = [&](const char* name) {
            struct stat info;
            return stat((package_dir + "/" + name).c_str(), &info) == 0 ? (long long)info.st_size : -1LL;
        };
        std::snprintf(text, sizeof(text), " (paf: 0x%08X; sfo %lld, head %lld, eboot %lld)", paf,
                      size_of("sce_sys/param.sfo"), size_of("sce_sys/package/head.bin"), size_of("eboot.bin"));
        detail = "promoter " + failures + text;
    }
    return ok;
}
#else
bool promote_package(const std::string&, std::string& detail, const char*, long long) {
    detail = "installing is only possible on the console";
    return false;
}
bool delete_package(const char*, std::string& detail) {
    detail = "removing is only possible on the console";
    return false;
}
#endif
