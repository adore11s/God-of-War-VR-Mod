// Developer command channel: <gameDir>\gowvr_logs\cmd.txt is polled every 100 ms; each line is executed in order and
// the file is deleted. Address expressions:
//   0x7ff4...        absolute
//   s:140d4e5e0      static VA rebased onto the running GoW.exe
//   g:142d4e5e0+10   *(static global) + offset (offset hex)
// Commands: watch <slot> <expr> <len> <w|rw> | unwatch | report | wait <ms> | dump <expr> <nfloats> | echo <text>
#include "cmd.h"
#include <atomic>
#include "../core/addr.h"
#include "watch.h"
#include "rdoc.h"
#include "shot.h"
#include "../input/vpad.h"
#include "../input/vrmap.h"
#include "../stereo/mutate.h"
#include "scan.h"
#include "cbprobe.h"
#include "crashlog.h"
#include "../frame/d3d_hooks.h"
#include "stackprobe.h"
#include "../stereo/r1.h"
#include "../stereo/eyes.h"
#include "../stereo/headcam.h"
#include "../stereo/sharpui.h"
#include "../stereo/dlsseyes.h"
#include "../stereo/lodprobe.h"
#include "../stereo/postfx.h"
#include "../fp/body.h"
#include "../fp/firstperson.h"
#include "../fp/arms.h"
#include "../fp/movelog.h"
#include "../fp/combat.h"
#include "../fp/posdiag.h"
#include "../vr/xr.h"
#include "../core/log.h"
#include "../core/config.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace gowvr::diag {
namespace {

uintptr_t g_base = 0;

bool ParseExpr(const char* s, uintptr_t* out) {
    char* end = nullptr;
    if (!strncmp(s, "s:", 2)) {
        *out = strtoull(s + 2, &end, 16) - 0x140000000ull + g_base;
        return true;
    }
    if (!strncmp(s, "g:", 2)) {
        uintptr_t va = strtoull(s + 2, &end, 16) - 0x140000000ull + g_base;
        uintptr_t off = (end && *end == '+') ? strtoull(end + 1, nullptr, 16) : 0;
        uintptr_t p = 0;
        if (IsBadReadPtr((void*)va, 8)) return false;
        p = *(uintptr_t*)va;
        *out = p + off;
        return p != 0;
    }
    *out = strtoull(s, &end, 16);
    return *out != 0;
}

void Dump(uintptr_t a, int n) {
    if (n > 256) n = 256;
    if (IsBadReadPtr((void*)a, (size_t)n * 4)) { Log("dump: %p unreadable", (void*)a); return; }
    const float* f = (const float*)a;
    for (int i = 0; i < n; i += 4) {
        Log("dump: %p +0x%03x %12.5f %12.5f %12.5f %12.5f", (void*)(a + i * 4), i * 4, f[i],
            i + 1 < n ? f[i + 1] : 0.f, i + 2 < n ? f[i + 2] : 0.f, i + 3 < n ? f[i + 3] : 0.f);
    }
}

// Commands added after the main chain hit MSVC's nesting limit (C1061): combat / effect switches (D-059..D-062).
bool ExecuteCombat(const char* verb, const char* a, const char* b, int got) {
    if (!strcmp(verb, "fastrelease")) {
        fp::FastRelease(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "gate")) {  // gate r1|r2 [anim]
        input::GateTest(!strcmp(a, "r2"), got >= 3 && !strcmp(b, "anim"), 40);
    } else if (!strcmp(verb, "combatstate")) {
        char prim[96];
        fp::HeroPrimaryAnim(prim, sizeof(prim));
        Log("combatstate: rage %d sprint %d busy %d idle %llu ms anim '%s' move '%s'", (int)fp::CombatInRage(),
            (int)fp::CombatSprinting(), (int)fp::CombatBusy(), fp::CombatIdleMs(), prim, fp::ActiveMoveAnim());
    } else if (!strcmp(verb, "attackreach")) {
        fp::CombatReach((float)atof(a));
    } else if (!strcmp(verb, "gatespam")) {
        input::GateSpam(got >= 2 ? atoi(a) : 6, got >= 3 ? atoi(b) : 150);
    } else if (!strcmp(verb, "stancecancel")) {
        input::SetStanceCancel(atoi(a));
    } else if (!strcmp(verb, "attackhold")) {
        fp::CombatAttackHold(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "axefxrefresh")) {
        fp::AxeFxRefresh(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "wflaxe")) {
        fp::WflAxe(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "wflaxestats")) {
        fp::WflAxeStats();
    } else if (!strcmp(verb, "flameblade")) {
        fp::FlameBlade(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "bladeattack")) {
        fp::ArmsBladeAttackPose(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "gripreset")) {
        fp::ArmsGripReset();
    } else if (!strcmp(verb, "gripsettle")) {
        fp::ArmsGripSettleV2(strcmp(a, "old") != 0);
    } else if (!strcmp(verb, "axegem")) {
        fp::AxeGem(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "chainmode")) {
        fp::ChainMode(atoi(a));
    } else if (!strcmp(verb, "chaindump")) {
        fp::ChainDump();
    } else if (!strcmp(verb, "chainsync")) {
        fp::ChainSync(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "bladejoints")) {
        fp::BladeJoints();
    } else if (!strcmp(verb, "handjoints")) {
        fp::HandJoints(got >= 2 ? atoi(a) : 400);
    } else if (!strcmp(verb, "chaindiag")) {
        fp::ChainDiag(got >= 2 ? atoi(a) : 600);
    } else if (!strcmp(verb, "fxlead")) {
        fp::FxLeadSet((float)atof(a));
    } else if (!strcmp(verb, "passorder")) {
        fp::PassOrder(got >= 2 ? atoi(a) : 200);
    } else if (!strcmp(verb, "fpright")) {
        fp::SetViewRight((float)atof(a));
    } else if (!strcmp(verb, "fpdown")) {
        fp::SetViewDown((float)atof(a));
    } else if (!strcmp(verb, "loadwatch")) {  // D-083
        stereo::LoadWatch(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "uiscale")) {  // D-083
        stereo::UiScale((float)atof(a));
    } else if (!strcmp(verb, "uiscalefp")) {  // D-102
        stereo::UiScaleFp((float)atof(a));
    } else if (!strcmp(verb, "sharpui")) {  // D-104
        if (!strcmp(a, "probe")) stereo::SharpUiProbe(atoi(b));
        else if (!strcmp(a, "hudshot")) stereo::SharpUiHudShot();
        else if (!strcmp(a, "census")) stereo::SharpUiCensus();  // D-145
        else stereo::SharpUiEnable(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "lodprobe")) {  // D-108
        stereo::LodProbe(got >= 2 ? atoi(a) : 0);
    } else if (!strcmp(verb, "lodscale")) {  // D-108
        stereo::LodScale((float)atof(a));
    } else if (!strcmp(verb, "dlsseyes")) {  // D-108
        stereo::DlssEyesEnable(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "uireticle")) {  // D-104
        stereo::SharpUiReticleScale((float)atof(a));
    } else if (!strcmp(verb, "hudmode") || !strcmp(verb, "combathud")) {
        // D-101: the game's own HUD options - "HUD Mode" (Lua SetUserHUDDisplaySetting 0x1407632b0 stores an int at
        // 0x1422e7950) and "Combat HUD" (SetUserCombatHUDSetting 0x140763130, a byte at 0x141078feb). No argument: log
        // the current value (to map the menu's choices); with one: write it
        uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
        if (!strcmp(verb, "hudmode")) {
            int* v = (int*)((uint8_t*)GameVa(0x1422e7950ull));
            if (got >= 2) *v = atoi(a);
            Log("hudmode: %d", *v);
        } else {
            uint8_t* v = (uint8_t*)GameVa(0x141078febull);
            if (got >= 2) *v = (uint8_t)atoi(a);
            Log("combathud: %d", (int)*v);
        }
    } else if (!strcmp(verb, "screen2d")) {  // D-083: off | on (pause menu) | force
        stereo::Screen2DMode(!strcmp(a, "off") ? 0 : !strcmp(a, "force") ? 2 : 1);
    } else if (!strcmp(verb, "attackaim")) {
        fp::CombatAttackAim(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "ctldump")) {
        fp::CombatCtlDump();
    } else if (!strcmp(verb, "rootpos")) {
        float p[3], y;
        if (fp::GameRoot(p, &y)) Log("rootpos: %.3f %.3f %.3f t %llu", p[0], p[1], p[2], (unsigned long long)GetTickCount64());
    } else if (!strcmp(verb, "walkattack")) {  // walkattack <ly> <ms>: walk, R1 at 40 %, keep walking (D-064 test)
        const float ly = got >= 2 ? (float)atof(a) : 1.f;
        const int ms = got >= 3 ? atoi(b) : 2500;
        input::VpadHold(0, 0, ly, 0, 0, 0, 0, ms);
        Sleep((DWORD)(ms * 2 / 5));
        input::VpadHold(0x0200, 0, ly, 0, 0, 0, 0, 120);
        Sleep(130);
        input::VpadHold(0, 0, ly, 0, 0, 0, 0, ms - ms * 2 / 5 - 130);
        Log("walkattack: walked %d ms with R1 at %d ms", ms, ms * 2 / 5);
    } else if (!strcmp(verb, "nudge")) {
        fp::CombatNudgeAmount((float)atof(a));
    } else if (!strcmp(verb, "bladeaimhold")) {  // D-077 test: what vrmap reports while L2 + R2 are held with the Blades
        const ULONGLONG t0 = GetTickCount64(), ms = got >= 2 ? (ULONGLONG)atoi(a) : 2000;
        while (GetTickCount64() - t0 < ms) { fp::BladeAimHeld(); Sleep(30); }
    } else if (!strcmp(verb, "bladethrow")) {  // D-063 test: what vrmap signals when the Blades throw gesture fires
        fp::BladeThrowStart();
    } else if (!strcmp(verb, "bladeobjdiag")) {
        fp::BladeObjDiag();
    } else if (!strcmp(verb, "wtrace")) {
        fp::WeaponTrace(atoi(a));
    } else if (!strcmp(verb, "tsdump")) {
        fp::TsDump(atoi(a));
    } else if (!strcmp(verb, "hitprobe")) {
        fp::CombatHitProbe(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "tsprobe")) {
        fp::CombatTsProbe(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "hitradius")) {
        fp::CombatHitRadius((float)atof(a));
    } else if (!strcmp(verb, "hitradiusfist")) {  // D-108
        fp::CombatHitRadiusFist((float)atof(a));    } else if (!strcmp(verb, "combattrace")) {
        fp::CombatTrace(got >= 2 ? atoi(a) : 200);
    } else if (!strcmp(verb, "posscan")) {
        fp::PosScan();
    } else if (!strcmp(verb, "posscanall")) {
        fp::PosScanAll();
    } else if (!strcmp(verb, "posfind")) {
        fp::PosFind(atoi(a));
    } else if (!strcmp(verb, "postrack")) {
        fp::PosTrack();
    } else if (!strcmp(verb, "poshold")) {  // poshold <ms> [i|all|live]
        fp::PosHold(got >= 2 ? atoi(a) : 1500, got < 3 || !strcmp(b, "all") ? -1 : !strcmp(b, "live") ? -2 : atoi(b));
    } else if (!strcmp(verb, "satstrict")) {
        fp::SatStrict(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "satlock")) {
        fp::SatLock(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "glueblend")) {
        fp::GlueBlend(strcmp(a, "off") != 0);
    } else {
        return false;
    }
    return true;
}

// the VR settings panel's VR on / off (RequestVr): 0 none, 1 on, 2 off - the command thread runs it like F9
std::atomic<int> g_vrRequest{0};

void Execute(const std::string& line) {
    char verb[32] = {}, a[64] = {}, b[64] = {}, c[64] = {}, d[64] = {};
    int got = sscanf_s(line.c_str(), "%31s %63s %63s %63s %63s", verb, 32, a, 64, b, 64, c, 64, d, 64);
    if (got <= 0 || verb[0] == '#') return;
    Log("cmd: %s", line.c_str());
    if (ExecuteCombat(verb, a, b, got)) return;
    if (!strcmp(verb, "watchstacks")) {
        WatchStacks(strcmp(a, "off") != 0);
        return;
    }
    if (!strcmp(verb, "watchts") && got >= 2) {
        // D-080: write watchpoint (slot 0) on Kratos' creature time-scale slot N (creature+0x308+N*4) - the hit pause
        uint8_t* obj = fp::HeroObject();
        uint8_t* cr = obj && !IsBadReadPtr(obj + 0x220, 8) ? *(uint8_t**)(obj + 0x220) : nullptr;
        if (!cr) { Log("cmd: watchts - no hero creature"); return; }
        WatchArm(0, (uintptr_t)(cr + 0x308 + atoi(a) * 4), 4, false);
        return;
    }
    if (!strcmp(verb, "watch") && got >= 5) {
        uintptr_t addr;
        if (!ParseExpr(b, &addr)) { Log("cmd: bad address %s", b); return; }
        WatchArm(atoi(a), addr, atoi(c), !strcmp(d, "rw"));
    } else if (!strcmp(verb, "unwatch")) {
        WatchDisarmAll();
    } else if (!strcmp(verb, "report")) {
        WatchReport();
    } else if (!strcmp(verb, "wait") && got >= 2) {
        Sleep((DWORD)atoi(a));
    } else if (!strcmp(verb, "dump") && got >= 3) {
        uintptr_t addr;
        if (ParseExpr(a, &addr)) Dump(addr, atoi(b));
        else Log("cmd: bad address %s", a);
    } else if (!strcmp(verb, "dumpq") && got >= 3) {
        uintptr_t addr;
        if (!ParseExpr(a, &addr) || IsBadReadPtr((void*)addr, (size_t)atoi(b) * 8)) { Log("dumpq: bad %s", a); return; }
        for (int i = 0; i < atoi(b); ++i) {
            uintptr_t v = ((uintptr_t*)addr)[i];
            HMODULE m = nullptr;
            char mod[MAX_PATH] = "";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)v, &m) && m) {
                GetModuleFileNameA(m, mod, MAX_PATH);
                const char* s = strrchr(mod, '\\');
                sprintf_s(mod, "%s+0x%llx", s ? s + 1 : mod, (unsigned long long)(v - (uintptr_t)m));
            }
            Log("dumpq: [%p]+0x%03x = %p %s", (void*)addr, i * 8, (void*)v, mod);
        }
    } else if (!strcmp(verb, "shot")) {
        ShotRequest(got >= 2 ? a : "shot", got >= 3 ? atoi(b) : 4);
        ShotFlush(3000);
    } else if (!strcmp(verb, "pad")) {
        input::VpadEnable(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "focus")) {
        input::FocusOverride(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "hold")) {
        // hold <buttons hex> <lx> <ly> <rx> <ry> <lt> <rt> <ms>
        float v[6] = {};
        int ms = 0;
        unsigned buttons = 0;
        if (sscanf_s(line.c_str(), "hold %x %f %f %f %f %f %f %d", &buttons, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                     &ms) == 8)
            input::VpadHold((uint16_t)buttons, v[0], v[1], v[2], v[3], v[4], v[5], ms);
        else
            Log("cmd: hold needs 8 args");
    } else if (!strcmp(verb, "tap")) {
        // tap <name> [ms]: press for ms (default 120), release is implicit when the hold expires
        static const struct { const char* n; uint16_t b; } kButtons[] = {
            {"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008}, {"start", 0x0010},
            {"back", 0x0020}, {"ls", 0x0040}, {"rs", 0x0080}, {"lb", 0x0100}, {"rb", 0x0200},
            {"a", 0x1000}, {"b", 0x2000}, {"x", 0x4000}, {"y", 0x8000}};
        uint16_t btn = 0;
        for (auto& k : kButtons)
            if (!strcmp(a, k.n)) btn = k.b;
        int ms = got >= 3 ? atoi(b) : 120;
        if (btn) {
            input::VpadHold(btn, 0, 0, 0, 0, 0, 0, ms);
            Sleep((DWORD)ms + 150);  // include release time so consecutive taps are distinct edges
        } else {
            Log("cmd: unknown button '%s'", a);
        }
    } else if (!strcmp(verb, "mutinstall")) {
        Log("mutate: hooks %s", stereo::InstallMutationHooks() ? "installed" : "FAILED");
    } else if (!strcmp(verb, "mut") && got >= 5) {
        stereo::MutateArm(a, b, (float)atof(c), atoi(d));
    } else if (!strcmp(verb, "mutoff")) {
        stereo::MutateOff();
    } else if (!strcmp(verb, "freeze")) {
        stereo::Freeze(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "cbprobe")) {
        CbProbeArm(got >= 2 ? atoi(a) : 120);
    } else if (!strcmp(verb, "cbreport")) {
        CbProbeReport();
        CbProbeSiteReport();
    } else if (!strcmp(verb, "cbsites")) {
        CbProbeInstallSites();
    } else if (!strcmp(verb, "findcam")) {
        ScanForCamera(stereo::FrozenCamWorld());
    } else if (!strcmp(verb, "rdc")) {
        RdocTrigger(got >= 2 ? atoi(a) : 1);
    } else if (!strcmp(verb, "rdclist")) {
        RdocList();
    } else if (!strcmp(verb, "stackprobe") && got >= 5) {
        // stackprobe <staticVA hex> <stolen> <guard: 32 hex chars> <budget>   (from tools/re/hookgen.py)
        uint8_t guard[16] = {};
        if (strlen(c) != 32) { Log("stackprobe: guard must be 32 hex chars"); return; }
        for (int i = 0; i < 16; ++i) {
            char byte[3] = {c[i * 2], c[i * 2 + 1], 0};
            guard[i] = (uint8_t)strtoul(byte, nullptr, 16);
        }
        StackProbeInstall(strtoull(a, nullptr, 16), atoi(b), guard, atoi(d));
    } else if (!strcmp(verb, "threads")) {
        DumpAllThreadStacks();
    } else if (!strcmp(verb, "stackrearm")) {
        StackProbeRearm(got >= 2 ? atoi(a) : 8);
    } else if (!strcmp(verb, "r1install")) {
        Log("r1: kick hook %s", stereo::R1Install() ? "installed" : "FAILED");
    } else if (!strcmp(verb, "r1observe")) {
        stereo::R1Observe(got >= 2 ? atoi(a) : 3);
    } else if (!strcmp(verb, "r1eyes")) {
        if (!strcmp(a, "off")) stereo::R1Eyes(false, 0, 0, 0, 0);
        else stereo::R1Eyes(true, (float)atof(a), (float)atof(b), got >= 4 ? (float)atof(c) : 0.f, got >= 5 ? (float)atof(d) : 0.f);
    } else if (!strcmp(verb, "vr")) {
        // Full VR mode: RenderView/kick/eye hooks, OpenXR session, continuous double kick with head-tracked eyes.
        static bool hooks = false;
        bool on = strcmp(a, "off") != 0;
        if (on && !hooks) hooks = stereo::InstallMutationHooks() & stereo::R1Install();
        if (on && !hooks) { Log("vr: hook install failed -> not starting"); return; }
        if (on) {
            input::FocusOverride(true);
            input::VpadEnable(true);            // controllers reach the game through the virtual pad
            input::VrControllersEnable(true);
            stereo::NoDepthOfField(true);
            vr::XrRequest(true);
            stereo::EyesEnable(true);
            stereo::HeadcamRecenter();
            stereo::R1Vr(true);
        } else {
            stereo::R1Vr(false);
            input::VrControllersEnable(false);
            stereo::EyesEnable(false);
            vr::XrRequest(false);
        }
    } else if (!strcmp(verb, "crashreport")) {  // D-112: a sample crash report (no crash) - check what players send
        CrashReportTest();
    } else if (!strcmp(verb, "padmode")) {  // D-111: "padmode xbox|ps" (= F10 / F11, D-217)
        input::SetPadMode(!strcmp(a, "ps") || !strcmp(a, "playstation") || !strcmp(a, "1") ? 1 : 0);
    } else if (!strcmp(verb, "xr")) {
        if (!strcmp(a, "status")) vr::XrStatus();
        else vr::XrRequest(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "key")) {
        input::PostKey((int)strtol(a, nullptr, 16), got >= 3 ? atoi(b) : 120);
    } else if (!strcmp(verb, "findptr") && got >= 2) {
        // findptr <value hex>: every 8-byte slot in GoW.exe .data (0x14105a000..+0x4064bd8) holding that value
        uint64_t v = strtoull(a, nullptr, 16);
        uint8_t* lo = (uint8_t*)(uint8_t*)GameVa(0x14105a000);
        int hits = 0;
        for (uint8_t* p = lo; p + 8 <= lo + 0x4064bd8 && hits < 40; p += 8)
            if (*(uint64_t*)p == v) { Log("findptr: %llx at static 0x%llx", (unsigned long long)v, (unsigned long long)(p - (uint8_t*)g_base + 0x140000000)); ++hits; }
        Log("findptr: %d hits", hits);
    } else if (!strcmp(verb, "handsimroll")) {
        stereo::HeadcamHandSimRoll((float)atof(a));
    } else if (!strcmp(verb, "handsim")) {
        stereo::HeadcamHandSim(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "camsync")) {
        stereo::CamSync(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "armsdiag")) {
        stereo::ArmsDiag(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "headcollapse")) {
        fp::SetHeadCollapsed(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "curltest")) {
        fp::ArmsCurlTest(got >= 2 ? (float)atof(a) : -1.f);
    } else if (!strcmp(verb, "wristpitch")) {
        fp::ArmsAdjustPitch((float)atof(a));
    } else if (!strcmp(verb, "wristyaw")) {
        fp::ArmsAdjustYaw((float)atof(a));
    } else if (!strcmp(verb, "wristroll")) {
        fp::ArmsAdjustRoll((float)atof(a));
    } else if (!strcmp(verb, "arms")) {
        fp::ArmsEnable(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "armtest")) {
        fp::ArmTest(got >= 2 ? (float)atof(a) : 0.f);
    } else if (!strcmp(verb, "xrin")) {
        vr::InputSnapshot s = vr::XrInput();
        for (int h = 0; h < 2; ++h) {
            const vr::HandState& hs = s.hand[h];
            Log("xrin: %s focused=%d active=%d grip=%d(%.3f %.3f %.3f) stick(%.2f %.2f) trig=%.2f sq=%.2f pad=%.2f a=%d b=%d sc=%d profile=%s",
                h ? "R" : "L", s.focused, hs.active, hs.gripValid, hs.grip.p[0], hs.grip.p[1], hs.grip.p[2], hs.stick[0],
                hs.stick[1], hs.trigger, hs.squeeze, hs.trackpadForce, hs.a, hs.b, hs.stickClick, s.profile[h]);
        }
    } else if (!strcmp(verb, "fp")) {
        // fp gameplay|cutscene on|off, fp status
        if (!strcmp(a, "gameplay")) fp::SetGameplayFP(strcmp(b, "off") != 0);
        else if (!strcmp(a, "cutscene")) fp::SetCutsceneFP(strcmp(b, "off") != 0);
        else fp::FpStatus();
    } else if (!strcmp(verb, "fpfwd") && got >= 2) {
        fp::SetViewForward((float)atof(a));
    } else if (!strcmp(verb, "fpeye") && got >= 3) {
        fp::SetEyeOffset((float)atof(a), (float)atof(b));
    } else if (!strcmp(verb, "state")) {
        Log("state: cinematic=%d", fp::InCinematic() ? 1 : 0);
    } else if (!strcmp(verb, "yawtest")) {
        fp::YawTest(got >= 2 ? (float)atof(a) : 45.f);
    } else if (!strcmp(verb, "axetrace")) {
        fp::AxeTrace(got >= 2 ? atoi(a) : 60);
    } else if (!strcmp(verb, "axediag")) {
        fp::AxeDiag(got >= 2 ? atoi(a) : 200);
    } else if (!strcmp(verb, "physjoints")) {
        fp::PhysJoints();
    } else if (!strcmp(verb, "nearobjs")) {
        fp::NearObjects();
    } else if (!strcmp(verb, "combat")) {
        fp::CombatEnable(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "heavystrength")) {  // D-148
        input::SetHeavyByStrength(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "movelog")) {
        fp::MoveLogEnable(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "throwstart")) {  // what vrmap does on an aimed F1 throw (the pad press is separate)
        fp::ThrowStart();
    } else if (!strcmp(verb, "attdiag")) {
        fp::AttDiag(got >= 2 ? atoi(a) : 200);
    } else if (!strcmp(verb, "bodyoff")) {
        fp::BodyTestOffset(got >= 2 ? (float)atof(a) : 0.f);
    } else if (!strcmp(verb, "bowhide")) {
        fp::SetCarriedHide(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "skelnear")) {
        fp::SkelNearStart(got >= 2 ? atoi(a) : 3000);
    } else if (!strcmp(verb, "fplook")) {
        stereo::FpLook(got >= 2 ? (float)atof(a) : 0.f, got >= 3 ? (float)atof(b) : 0.f, got >= 4 ? (float)atof(c) : 0.f,
                       got >= 5 ? (float)atof(d) : 0.f);
    } else if (!strcmp(verb, "handsimfwd")) {
        stereo::HeadcamHandSimFwd(got >= 2 ? (float)atof(a) : 0.38f);
    } else if (!strcmp(verb, "armsquare")) {
        fp::StanceComp(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "fxjump")) {
        fp::FxJump(got >= 2 ? a : "off", got >= 3 ? b : "");
    } else if (!strcmp(verb, "copyfix")) {
        if (!strcmp(a, "stats")) fp::CopyStats();
        else if (!strcmp(a, "scan")) fp::CopyScan(got >= 3 ? atoi(b) : 30);
        else fp::CopyFix(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "flymove")) {
        fp::FlyMove(got >= 2 ? (float)atof(a) : 0.f);
    } else if (!strcmp(verb, "fxfix")) {
        if (!strcmp(a, "stats")) fp::FxStats(); else fp::FxFix(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "wfl")) {
        if (!strcmp(a, "stats")) fp::WflStats(); else fp::WflFix(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "drawtrace")) {
        fp::DrawTrace(got >= 2 ? atoi(a) : 200);
    } else if (!strcmp(verb, "bladeobj")) {
        fp::BladeObjFix(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "ensureblades")) {
        fp::EnsureBlades(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "carryguard")) {
        fp::CarryGuard(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "drawcheck")) {
        fp::DrawCheck(got >= 2 ? a : "off", got >= 3 ? b : "");
    } else if (!strcmp(verb, "attackfull")) {
        fp::ArmsAttackFull(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "atkfilter")) {
        fp::ArmsAttackFilter(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "bladecheck")) {
        fp::ArmsBladeCheck(got >= 2 ? a : "off", got >= 3 ? b : "");
    } else if (!strcmp(verb, "handsimlock")) {
        stereo::HeadcamHandSimLock(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "shrinkj")) {
        fp::ArmsShrink(got >= 2 ? atoi(a) : -1, got >= 3 ? atoi(b) : (got >= 2 ? atoi(a) : -1));
    } else if (!strcmp(verb, "flydiag")) {
        fp::FlyDiag(got >= 2 ? atoi(a) : 60);
    } else if (!strcmp(verb, "fistcenter")) {
        fp::ArmsFistCenter(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "bladediag")) {
        fp::ArmsBladeDiagAll(got >= 3 && !strcmp(b, "all"));
        fp::ArmsBladeDiag(got >= 2 ? atoi(a) : 3);
    } else if (!strcmp(verb, "weapon")) {
        static const char* kW[] = {"unknown", "Axe", "Blades", "Bare"};
        Log("weapon: %s", kW[fp::CurrentWeapon() & 3]);
    } else if (!strcmp(verb, "eyesync")) {
        fp::EyeSync(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "eyeprobe")) {
        fp::EyeProbeStart(got >= 2 ? atoi(a) : 600);
    } else if (!strcmp(verb, "armreach")) {
        fp::ArmsReach(got >= 2 ? atoi(a) : 6);
    } else if (!strcmp(verb, "attjitter")) {
        fp::AttJitter(got >= 2 ? atoi(a) : 400);
    } else if (!strcmp(verb, "armjitter")) {
        fp::ArmsJitter(got >= 2 ? atoi(a) : 600);
    } else if (!strcmp(verb, "armslocal")) {
        fp::ArmsLocal(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "handsimmove")) {
        stereo::HeadcamHandSimMove(got >= 2 ? (float)atof(a) : 1.f);
    } else if (!strcmp(verb, "attachmove")) {
        fp::ArmsAttachMove(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "attachdiag")) {
        fp::ArmsAttachDiag(got >= 2 ? atoi(a) : 40);
    } else if (!strcmp(verb, "attachold")) {
        fp::ArmsAttachOld(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "neckhide")) {
        fp::SetNeckHide(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "axefind")) {
        fp::AxeFind();
    } else if (!strcmp(verb, "facescan")) {
        fp::FacingScan();
    } else if (!strcmp(verb, "objscan")) {
        fp::ScanObjects();
    } else if (!strcmp(verb, "findskel")) {
        fp::FindSkeleton();
    } else if (!strcmp(verb, "joints")) {
        fp::DumpJoints();
    } else if (!strcmp(verb, "overscan")) {
        stereo::HeadcamOverscan((float)atof(a), got >= 3 ? atoi(b) : 1);
    } else if (!strcmp(verb, "headcamera")) {
        stereo::SetHeadCamera(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "hmdfov")) {
        stereo::HeadcamHmdFov(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "xrmono")) {
        d3d::g_xrMono = strcmp(a, "off") != 0;
        Log("xr: mono submission %d", d3d::g_xrMono);
    } else if (!strcmp(verb, "hmdshot")) {
        vr::XrHmdShot(got >= 2 ? a : "hmd", got >= 3 ? atoi(b) : 2);
    } else if (!strcmp(verb, "viewa")) {
        stereo::EyeViewAWrite(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "r1mask")) {
        stereo::R1Mask((unsigned)strtoul(a, nullptr, 16));
    } else if (!strcmp(verb, "fogfix")) {
        stereo::R1FogFix(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "wobble")) {
        stereo::R1Wobble(got >= 2 ? (float)atof(a) : 0.f);
    } else if (!strcmp(verb, "camlog")) {
        stereo::CamLog(got >= 2 ? atoi(a) : 12);
    } else if (!strcmp(verb, "dof")) {
        stereo::NoDepthOfField(!strcmp(a, "off"));
    } else if (!strcmp(verb, "recenter")) {
        stereo::HeadcamRecenter();
    } else if (!strcmp(verb, "headcfg") && got >= 3) {
        stereo::HeadcamConfig((float)atof(a), atoi(b) != 0, got >= 4 ? (float)atof(c) : 0.f, got >= 5 ? (float)atof(d) : 0.f);
    } else if (!strcmp(verb, "eyes")) {
        stereo::EyesEnable(strcmp(a, "off") != 0);
    } else if (!strcmp(verb, "eyeshot")) {
        stereo::EyesShot(got >= 2 ? a : "eyes", got >= 3 ? atoi(b) : 4);
    } else if (!strcmp(verb, "rvprobe")) {
        stereo::RvProbe(got >= 2 ? atoi(a) : 3);
    } else if (!strcmp(verb, "r1bank")) {
        stereo::R1BankLoad();
    } else if (!strcmp(verb, "r1diff")) {
        stereo::R1Diff(got >= 2 ? atoi(a) : 1);
    } else if (!strcmp(verb, "r1double")) {
        stereo::R1Double(got >= 2 ? atoi(a) : 1, got >= 3 ? (unsigned)strtoul(b, nullptr, 16) : 0x1ff);
    } else if (!strcmp(verb, "packets")) {
        d3d::LogPackets(got >= 2 ? atoi(a) : 5);
    } else if (!strcmp(verb, "echo")) {
        Log("echo: %s", line.c_str() + 5);
    } else {
        Log("cmd: unknown '%s'", line.c_str());
    }
}

DWORD WINAPI Poll(void*) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    *wcsrchr(path, L'\\') = 0;
    wcscat_s(path, L"\\gowvr_logs\\cmd.txt");
    int tick = 0;
    // Player controls without the command file: F9 toggles VR, F8 re-centers the view. VR starts by itself as soon as
    // the game draws (D-188); gowvr.ini "autovr=0" next to GoW.exe or env GOWVR_NOAUTOVR=1 (test harness) disables it.
    wchar_t ini[MAX_PATH];
    wcscpy_s(ini, path);
    *wcsrchr(ini, L'\\') = 0;
    *wcsrchr(ini, L'\\') = 0;
    wcscat_s(ini, L"\\gowvr.ini");
    // D-110: through the config reader (the first 256 bytes of the file were searched for "autovr=0": a commented
    // gowvr.ini - the release ships one - hid it)
    bool autovr = config::GetFloat("autovr", 1.f) > 0.5f;
    (void)ini;
    if (char env[8] = {}; GetEnvironmentVariableA("GOWVR_NOAUTOVR", env, sizeof(env)) && env[0] == '1') autovr = false;
    // D-188 (owner: "make VR start right away when the game starts, instead of after 20 s"): no fixed timer any more - VR
    // is turned on as soon as the game draws (its D3D device and swap chain exist: 30 presents, ~0.5 s), plus gowvr.ini
    // autovr_delay seconds (default 0). The OpenXR session is created at a Present anyway, and the boot videos /
    // loading screens / menus show on the flat screen inside the headset
    const float autovrDelay = config::GetFloat("autovr_delay", 0.f);
    Log("cmd: autovr %s", autovr ? "on (VR starts as soon as the game draws)" : "off");
    if (autovr && autovrDelay > 0.f) Log("cmd: autovr waits %.0f s more (gowvr.ini autovr_delay)", autovrDelay);
    bool autovrDone = false;
    bool vrOn = false, f8 = false, f9 = false, f1 = false, f3 = false, f5 = false, f6 = false, f7 = false, f10 = false, f11 = false;
    fp::HideBody();  // load gowvr.ini hide_body
    for (;;) {
        Sleep(100);
        ++tick;
        if (autovr && !autovrDone && d3d::PresentCount() >= 30 && (float)tick * 0.1f >= autovrDelay) {
            autovrDone = true;
            Log("cmd: autovr (%.1f s after the mod loaded, %llu presents)", (float)tick * 0.1f,
                (unsigned long long)d3d::PresentCount());
            Execute("vr on");
            vrOn = true;
        }
        // D-110 (code audit M7): the F-keys act only while a God of War window is in front - they were read globally,
        // so F5 / F6 in a browser turned the VR hands and rewrote gowvr.ini, F1 / F3 / F7 / F9 switched modes
        // (a SteamVR window counts as the game's: the focus override exists because SteamVR's desktop windows take the
        // focus while the player is in the headset - the keys must keep working then)
        const bool gameFront = [] {
            static HWND lastW = nullptr;
            static bool lastOk = false;
            DWORD pid = 0;
            HWND w = GetForegroundWindow();
            if (!w || !GetWindowThreadProcessId(w, &pid)) return false;
            if (pid == GetCurrentProcessId()) return true;
            if (w == lastW) return lastOk;
            lastW = w;
            lastOk = false;
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                wchar_t img[MAX_PATH];
                DWORD len = MAX_PATH;
                if (QueryFullProcessImageNameW(p, 0, img, &len)) {
                    const wchar_t* name = wcsrchr(img, L'\\');
                    name = name ? name + 1 : img;
                    for (const wchar_t* vr : {L"vrmonitor.exe", L"vrcompositor.exe", L"vrserver.exe", L"vrdashboard.exe",
                                              L"vrwebhelper.exe", L"vrstartup.exe"})
                        if (!_wcsicmp(name, vr)) lastOk = true;
                }
                CloseHandle(p);
            }
            return lastOk;
        }();
        auto Key = [gameFront](int vk) { return gameFront && (GetAsyncKeyState(vk) & 0x8000) != 0; };
        bool k8 = Key(VK_F8), k9 = Key(VK_F9);
        bool k7 = Key(VK_F7);
        if (k7 && !f7) fp::SetHideBody(!fp::HideBody());  // F7: arms only in gameplay first person
        f7 = k7;
        bool k1 = Key(VK_F1), k3 = Key(VK_F3);
        bool k5 = Key(VK_F5), k6 = Key(VK_F6);
        // D-111: the Xbox pad / the game's PlayStation pad (PlayStation button prompts, PSVR2). D-217 (owner): F10 = Xbox,
        // F11 = PlayStation (was F11 / F12 - F12 is also Steam's screenshot key)
        const bool k10 = Key(VK_F10), k11 = Key(VK_F11);
        if (k10 && !f10) input::SetPadMode(0);
        if (k11 && !f11) input::SetPadMode(1);
        f10 = k10;
        f11 = k11;
        if ((k5 && !f5) || (k6 && !f6)) {  // F5 / F6: wrist angle down / up for this headset's controllers
            const float d = k5 && !f5 ? -5.f : +5.f;  // Ctrl: turn the hand left/right, Shift: roll it
            if (GetAsyncKeyState(VK_MENU) & 0x8000) stereo::AimAdjustPitch(d * 0.1f);  // Alt: throw aim up/down
            else if (GetAsyncKeyState(VK_CONTROL) & 0x8000) fp::ArmsAdjustYaw(d);
            else if (GetAsyncKeyState(VK_SHIFT) & 0x8000) fp::ArmsAdjustRoll(d);
            else fp::ArmsAdjustPitch(d);
        }
        f5 = k5;
        f6 = k6;
        if (k1 && !f1) fp::SetGameplayFP(!fp::GameplayFP());  // F1: first person in gameplay
        if (k3 && !f3) fp::SetCutsceneFP(!fp::CutsceneFP());  // F3: first person in cutscenes
        f1 = k1;
        f3 = k3;
        if (k8 && !f8) Execute("recenter");
        if (k9 && !f9) {
            vrOn = !vrOn;
            Execute(vrOn ? "vr on" : "vr off");
        }
        if (const int req = g_vrRequest.exchange(0)) {  // the VR settings panel (F9's own state follows it)
            vrOn = req == 1;
            Log("cmd: VR %s from the VR settings panel", vrOn ? "on" : "off");
            Execute(vrOn ? "vr on" : "vr off");
        }
        f8 = k8;
        f9 = k9;
        if (tick % 10 == 0) {
            WatchRefreshThreads();
            CrashFilterKeep();  // D-112
            input::FocusKeepAlive();
        }
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"rb") != 0 || !f) continue;
        std::vector<std::string> lines;
        char buf[512];
        while (fgets(buf, sizeof(buf), f)) {
            std::string s = buf;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            lines.push_back(s);
        }
        fclose(f);
        DeleteFileW(path);
        for (auto& l : lines) Execute(l);
    }
}

}  // namespace

void StartCommandChannel() {
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    if (HANDLE h = CreateThread(nullptr, 0, Poll, nullptr, 0, nullptr)) CloseHandle(h);
}

void RequestVr(bool on) { g_vrRequest = on ? 1 : 2; }

}  // namespace gowvr::diag
