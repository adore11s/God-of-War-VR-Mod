#include "prompts.h"
#include "combat.h"
#include "firstperson.h"
#include "../core/addr.h"
#include "../core/log.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace gowvr::fp {
namespace {

// STATIC (reverse_engineering/src/ui/ui_003.c sub_140843730, unattributed_140800000_033.c sub_140845140)
constexpr uint64_t kMsgTableVa = 0x142ccbc38ull;  // char** -> records {char* text @0; int32 id @8; u16 lines @0xc}, 0x10 each
constexpr uint64_t kMsgCountVa = 0x142ccbc50ull;  // int32 record count

// the game's English sentences (r_lang_en.wad MSGS_TXT ids 18031, 20390, 26429, 40956) and the first-person wording.
// [RightButton] / [LeftButton] are the game's button tokens, replaced by the icon after the lookup
struct Swap { const char* from; const char* to; };
const Swap kSwaps[] = {
    {"Press [RightButton] to equip the Leviathan Axe.", "Reach over your right shoulder to equip the Leviathan Axe."},
    {"Press [RightButton] to equip the axe.", "Reach over your right shoulder to equip the axe."},
    // 40956 shows both weapons in one box: the Blades are drawn over the LEFT shoulder in F1 (vrmap UpdateDraw)
    {"Press [LeftButton] to equip the Blades of Chaos.", "Reach over your left shoulder to equip the Blades of Chaos."},
};

struct Patched { uint8_t* rec; char* orig; const char* mine; int id; };
std::vector<Patched> g_patched;
std::deque<std::string> g_texts;  // our wordings (never freed: the game may hold a pointer it looked up)
uintptr_t g_table = 0;
int g_count = 0;
bool g_on = false;

bool ReadTable(uintptr_t* table, int* count) {
    __try {
        *table = *(const uintptr_t*)GameVa(kMsgTableVa);
        *count = *(const int*)GameVa(kMsgCountVa);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// one record: its text pointer, id and a copy of the text (len -1 = none / too long)
bool ReadRecord(uint8_t* rec, char** text, int* id, char* buf, size_t cap, int* len) {
    __try {
        *text = *(char**)rec;
        *id = *(const int*)(rec + 8);
        *len = -1;
        if (!*text) return true;
        const size_t n = strnlen(*text, cap);
        if (n >= cap) return true;
        memcpy(buf, *text, n);
        *len = (int)n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// our wording when the text holds one of the sentences (empty = none)
std::string Reword(const char* text, int len) {
    std::string s(text, (size_t)len);
    bool hit = false;
    for (const Swap& w : kSwaps)
        for (size_t p; (p = s.find(w.from)) != std::string::npos; hit = true) s.replace(p, strlen(w.from), w.to);
    return hit ? s : std::string();
}

bool WriteText(uint8_t* rec, char* expect, char* to) {
    __try {
        if (*(char**)rec != expect) return false;  // the table changed under us
        *(char* volatile*)rec = to;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Rescan(uintptr_t table, int count) {
    g_patched.clear();
    g_table = table;
    g_count = count;
    g_on = false;
    if (!table || count <= 0 || count > 200000) return;
    static char buf[4096];
    for (int i = 0; i < count; ++i) {
        uint8_t* rec = (uint8_t*)table + (size_t)i * 0x10;
        char* text = nullptr;
        int id = 0, len = -1;
        if (!ReadRecord(rec, &text, &id, buf, sizeof(buf), &len)) return;
        if (len < 0) continue;
        std::string mine = Reword(buf, len);
        if (mine.empty()) continue;
        g_texts.push_back(std::move(mine));
        g_patched.push_back({rec, text, g_texts.back().c_str(), id});
    }
    static int logs = 0;
    if (logs++ < 10) {
        std::string ids;
        for (const Patched& p : g_patched) ids += " " + std::to_string(p.id);
        Log("prompts: game text table %p (%d texts) - equip prompts found:%s (D-312)", (void*)table, count,
            g_patched.empty() ? " none (not English?)" : ids.c_str());
    }
}

}  // namespace

void PromptTextTick() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 200) return;
    last = now;
    uintptr_t table = 0;
    int count = 0;
    if (!ReadTable(&table, &count)) return;
    if (table != g_table || count != g_count) Rescan(table, count);
    if (g_patched.empty()) return;
    const bool want = FpMode() == 1 && MotionControls();
    if (want == g_on) return;
    bool ok = true;
    for (const Patched& p : g_patched)
        ok &= want ? WriteText(p.rec, p.orig, (char*)p.mine) : WriteText(p.rec, (char*)p.mine, p.orig);
    if (!ok) { g_table = 0; g_count = 0; return; }  // rescan next time
    g_on = want;
    static int logs = 0;
    if (logs++ < 40)
        Log("prompts: equip prompts %s (D-312)", want ? "say 'Reach over your right / left shoulder' (first person, motion controls)"
                                                      : "back to the game's own text");
}

}  // namespace gowvr::fp
