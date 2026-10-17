#pragma once

struct ID3D11DeviceContext;

namespace gowvr::diag {

// NOTE (D-104): d3d11 rewrites the context's function table (Swap / per frame) - these table hooks stop firing once VR
// runs; sharpui.cpp hooks the executor call sites instead.
// Hooks the immediate context's UpdateSubresource/Map/Unmap (vtable 48/14/15). While armed, any buffer upload whose
// contents hold view A's camera position in the transposed viewData layout is attributed to its caller (return address).
void CbProbeInstall(ID3D11DeviceContext* immediate);
void CbProbeArm(int frames);
void CbProbeReport();
void CbProbeOnPresent();
// Call-site hooks on the engine's UpdateSubresource calls (render-thread command interpreter).
void CbProbeInstallSites();
void CbProbeSiteReport();

}  // namespace gowvr::diag
