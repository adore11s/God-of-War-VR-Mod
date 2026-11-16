// D-336: the parts the mod set up (hooks, patches, modules) and whether each loaded - see parts.h.
#include "parts.h"
#include "log.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace gowvr {
namespace {

struct Part {
    std::string name, why;
    bool ok = false;
    int tries = 0;
};
std::mutex g_mu;
std::vector<Part> g_parts;

}  // namespace

void PartNote(const char* name, bool ok, const char* why) {
    if (!name || !*name) return;
    bool firstFail = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Part* p = nullptr;
        for (auto& e : g_parts)
            if (e.name == name) { p = &e; break; }
        if (!p) {
            g_parts.push_back(Part{});
            p = &g_parts.back();
            p->name = name;
        }
        firstFail = !ok && (p->tries == 0 || p->ok);
        p->ok = ok;
        p->why = ok ? std::string() : std::string(why && *why ? why : "failed");
        ++p->tries;
    }
    if (firstFail) Log("parts: %s NOT loaded - %s (D-336)", name, why && *why ? why : "failed");
}

int PartsText(char* out, size_t n) {
    if (!out || !n) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    int loaded = 0, failed = 0;
    for (const auto& p : g_parts) (p.ok ? loaded : failed)++;
    int len = sprintf_s(out, n, "%d parts loaded, %d NOT loaded", loaded, failed);
    if (len < 0) len = 0;
    bool first = true;
    for (const auto& p : g_parts) {
        if (p.ok) continue;
        if ((size_t)len + p.name.size() + p.why.size() + 16 >= n) {
            sprintf_s(out + len, n - len, " ...");
            break;
        }
        const int w = sprintf_s(out + len, n - len, "%s%s (%s)", first ? ": " : "; ", p.name.c_str(), p.why.c_str());
        if (w > 0) len += w;
        first = false;
    }
    return failed;
}

void PartsCount(int* loaded, int* failed) {
    std::lock_guard<std::mutex> lk(g_mu);
    int l = 0, f = 0;
    for (const auto& p : g_parts) (p.ok ? l : f)++;
    if (loaded) *loaded = l;
    if (failed) *failed = f;
}

}  // namespace gowvr
