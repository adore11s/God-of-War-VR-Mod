// RenderDoc in-app API bridge. RenderDoc itself is injected by `renderdoccmd capture` (tools/rdc_launch.ps1);
// this only finds the already-loaded renderdoc.dll and triggers/labels captures from the command channel.
#include "rdoc.h"
#include "../core/log.h"
#include "../../third_party/renderdoc/renderdoc_app.h"
#include <windows.h>
#include <cstdio>

namespace gowvr::diag {
namespace {
RENDERDOC_API_1_6_0* g_api = nullptr;

bool Api() {
    if (g_api) return true;
    HMODULE m = GetModuleHandleA("renderdoc.dll");
    if (!m) { Log("rdoc: renderdoc.dll not loaded (launch with tools/rdc_launch.ps1)"); return false; }
    auto get = (pRENDERDOC_GetAPI)GetProcAddress(m, "RENDERDOC_GetAPI");
    if (!get || !get(eRENDERDOC_API_Version_1_6_0, (void**)&g_api)) { Log("rdoc: GetAPI failed"); return false; }
    int ma, mi, pa;
    g_api->GetAPIVersion(&ma, &mi, &pa);
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    *strrchr(path, '\\') = 0;
    strcat_s(path, "\\gowvr_logs\\captures\\gow");
    g_api->SetCaptureFilePathTemplate(path);
    g_api->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
    Log("rdoc: API %d.%d.%d, captures -> %s", ma, mi, pa, path);
    return true;
}
}  // namespace

void RdocTrigger(int frames) {
    if (!Api()) return;
    if (frames <= 1) g_api->TriggerCapture();
    else g_api->TriggerMultiFrameCapture((uint32_t)frames);
    Log("rdoc: triggered %d frame(s)", frames);
}

void RdocList() {
    if (!Api()) return;
    uint32_t n = g_api->GetNumCaptures();
    for (uint32_t i = 0; i < n; ++i) {
        char name[MAX_PATH];
        uint32_t len = MAX_PATH;
        uint64_t ts = 0;
        if (g_api->GetCapture(i, name, &len, &ts)) Log("rdoc: capture[%u] %s", i, name);
    }
}

}  // namespace gowvr::diag
