#pragma once

namespace gowvr::stereo {

// Depth of field forced off (engine post-effect constants), for VR comfort. Installs its hook on first use.
void NoDepthOfField(bool on);

}  // namespace gowvr::stereo
