cmake_minimum_required(VERSION 3.20)
project(gowvr LANGUAGES CXX ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "gowvr must be built for x64 (GoW.exe is PE32+)")
endif()

execute_process(COMMAND git -C ${CMAKE_SOURCE_DIR} describe --always --dirty
                OUTPUT_VARIABLE GOWVR_GIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT GOWVR_GIT)
  set(GOWVR_GIT "nogit")
endif()

# The release's default gowvr.ini, built into the DLL: the mod writes it next to GoW.exe on its first start, so no
# launcher is needed (owner 2026-10-17). It is files/gowvr.ini of the release folder (public source: ../files, the
# developer's tree: ../GOW_VR_MOD/files); none found = an empty default (the mod's built-in values).
set(GOWVR_DEFAULT_INI "")
foreach(p "${CMAKE_CURRENT_SOURCE_DIR}/../files/gowvr.ini" "${CMAKE_CURRENT_SOURCE_DIR}/../GOW_VR_MOD/files/gowvr.ini")
  if(NOT GOWVR_DEFAULT_INI AND EXISTS "${p}")
    set(GOWVR_DEFAULT_INI "${p}")
  endif()
endforeach()
set(GOWVR_INI_TEXT "")
if(GOWVR_DEFAULT_INI)
  file(READ "${GOWVR_DEFAULT_INI}" GOWVR_INI_TEXT)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GOWVR_DEFAULT_INI}")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gen/default_ini.inc" "R\"GOWVRINI(${GOWVR_INI_TEXT})GOWVRINI\"\n")

add_library(gowvr SHARED
  src/dllmain.cpp
  src/proxy/proxy.cpp
  src/proxy/version_stubs.asm
  src/proxy/version.def
  src/core/log.cpp
  src/core/target.cpp
  src/core/patch.cpp
  src/frame/d3d_hooks.cpp
  src/frame/rendersize.cpp
  src/diag/cmd.cpp
  src/diag/watch.cpp
  src/diag/crashlog.cpp
  src/diag/rdoc.cpp
  src/diag/shot.cpp
  src/input/vpad.cpp
  src/input/turning.cpp
  src/core/detour.cpp
  src/core/addr.cpp
  src/stereo/mutate.cpp
  src/stereo/r1.cpp
  src/stereo/eyes.cpp
  src/stereo/headcam.cpp
  src/stereo/sharpui.cpp
  src/stereo/dlsseyes.cpp
  src/stereo/taaeyes.cpp
  src/stereo/lodprobe.cpp
  src/stereo/postfx.cpp
  src/fp/body.cpp
  src/fp/firstperson.cpp
  src/fp/arms.cpp
  src/fp/movelog.cpp
  src/fp/combat.cpp
  src/fp/walls.cpp
  src/fp/posdiag.cpp
  src/core/config.cpp
  src/input/vrmap.cpp
  src/vr/xr.cpp
  src/vr/panel.cpp
  src/diag/scan.cpp
  src/diag/cbprobe.cpp
  src/diag/stackprobe.cpp
  src/diag/stackprobe_thunk.asm
)
set_target_properties(gowvr PROPERTIES OUTPUT_NAME "version")
target_include_directories(gowvr PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gen")  # default_ini.inc
target_compile_definitions(gowvr PRIVATE GOWVR_VERSION="${GOWVR_GIT}" WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(gowvr PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8>)
target_link_libraries(gowvr PRIVATE d3d11 dxgi dxguid psapi bcrypt windowscodecs ole32)
# Owner 2026-10-19 (Meta Link: "VR doesn't work at all"; the Beyond: Two Souls VR mod, which works there, links it): the
# official OpenXR loader, static (vcpkg x64-windows-static, no extra DLL), finds and starts the runtime like every OpenXR
# game. Without it (a build machine with no vcpkg) the runtime is loaded directly, as before.
list(APPEND CMAKE_PREFIX_PATH "C:/vcpkg/installed/x64-windows-static")
find_package(OpenXR CONFIG QUIET)
if(TARGET OpenXR::openxr_loader)
  target_link_libraries(gowvr PRIVATE OpenXR::openxr_loader)
  target_compile_definitions(gowvr PRIVATE GOWVR_OPENXR_LOADER=1)
  message(STATUS "gowvr: official OpenXR loader linked (static)")
else()
  message(STATUS "gowvr: OpenXR loader not found - the runtime is loaded directly")
endif()
# D-108: release builds keep symbols (version.pdb, archived per build by tools/install.ps1) so a crash offset in the
# mod maps to a function; /Zi does not change code generation, /OPT:REF /OPT:ICF keep the release linker settings
target_compile_options(gowvr PRIVATE $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:/Zi>)
# /PDBALTPATH: the DLL records only "version.pdb", not the build machine's full path (public release)
target_link_options(gowvr PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF /PDBALTPATH:version.pdb>)
# D-215: on the developer's machine every build also refreshes the public mod folder and installs into the game
# (tools/publish.ps1 - not part of the public source, so this step is skipped there)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/publish.ps1")
  add_custom_command(TARGET gowvr POST_BUILD
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/publish.ps1" -Config $<CONFIG>
    COMMENT "Publishing: GOW_VR_MOD + game install (D-215)")
endif()

# D-187: the address resolver run over a mapped GoW.exe without the game (not part of the mod; build target addrtest)
add_executable(addrtest EXCLUDE_FROM_ALL tools/addrtest/addrtest.cpp src/core/addr.cpp src/core/log.cpp src/core/config.cpp)
target_compile_definitions(addrtest PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(addrtest PRIVATE /utf-8)
