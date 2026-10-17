# Contributing

Thanks for wanting to improve the God of War VR Mod. It is open source under the [MIT license](LICENSE): fork it, fix it, publish your own builds. Pull requests and bug reports are welcome.

## Building

You need Windows, **Visual Studio 2022** (or its Build Tools) with the C++ workload, and **CMake 3.20+**.

```
cmake -S source -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

This produces `build\Release\version.dll`. To try it, copy it next to `GoW.exe` and start the game. On its first start
it writes the default `gowvr.ini` (`files\gowvr.ini`, built into the DLL).

There are no other dependencies: the OpenXR declarations are hand-written (`source/src/vr/xr_min.h`) and the runtime is found through the registry.

## How the mod works

- **Loading.** The mod is a `version.dll` **proxy**: GoW.exe imports `version.dll`, Windows finds ours next to the exe first, and our `DllMain` installs the hooks. `src/proxy` forwards the real version API to System32.
- **Any game build.** Every address is written in the source as it is in the recorded build (Steam, PE time stamp
  `0x628e8031`) and is also kept as a byte signature (`src/core/addrsigs.inc`, generated).
  - `src/core/target.cpp` recognises the recorded build by its exe header; there the addresses are used as they are.
  - On any other build (Epic, an update) `src/core/addr.cpp` finds every address again by its signature in one pass
    over the code (~30 ms) and logs each as at its recorded place / MOVED / missing / ambiguous. The mod turns on only
    when every one was found exactly once.
  - All address use goes through `GameVa()`; hook sites also check their original bytes before patching
    (`src/core/detour.cpp` - on another build the bytes the signature leaves open may differ).
- **Stereo.** The engine's frame-graph "kick" runs **twice per frame**, once per eye, with a per-eye camera (`src/stereo`). The two eye images are captured and sent to SteamVR through **OpenXR** (`src/vr/xr.cpp`).

| Folder | What is in it |
|---|---|
| `src/core` | logging, config (`gowvr.ini`), hooking (detours, import patching), target check |
| `src/frame` | D3D11 / DXGI hooks: device, swapchain, Present |
| `src/vr` | OpenXR session, swapchains, head and controller tracking, haptics |
| `src/stereo` | the per-eye rendering (double kick, eye cameras, head tracking), sharp HUD, per-eye DLSS, depth of field off |
| `src/fp` | first person: camera, body, arm IK, weapons, combat rules |
| `src/input` | the virtual pad (XInput and the game's PlayStation pad) and the VR controller mapping / gestures |
| `src/diag` | commands, crash log and crash reports, probes used while reverse-engineering |

### Debugging

- **Log.** Everything the mod does goes to `gowvr_logs\gowvr.log` next to `GoW.exe` (the previous run: `gowvr.prev.log`).
- **Commands.** Write lines into `gowvr_logs\cmd.txt` while the game runs; they are executed within 0.1 s. Examples:
  - `vr on`
  - `dof off`
  - `padmode ps`
  - `sharpui probe`
  - `crashreport` (writes a sample crash report)

  See `src/diag/cmd.cpp` for the full list.
- **Crashes.**
  - Crashes inside the game or the mod are logged with registers and a stack.
  - A real crash also writes `gowvr_logs\crash_reports\crash_<date>.txt`, the file players send.
  - Release builds keep `version.pdb` next to `version.dll`, so a `VERSION.dll+0x...` offset can be mapped to a function.

## Adding a game address

Write it in the source as it is in the recorded build (`0x14xxxxxxx`) and reach it only through `GameVa()` (or a
`HookSite`). Then, with the recorded GoW.exe:

```
python source/tools/re/sigs.py "<game folder>\GoW.exe"      (needs pefile, capstone, numpy)
python source/tools/re/sigcheck.py "<game folder>\GoW.exe"
```

`sigs.py` rewrites `src/core/addrsigs.inc` (every `0x14xxxxxxx` in the source outside comments); `sigcheck.py` must
report every signature at its recorded place. `cmake --build build --config Release --target addrtest` builds a test
that runs the mod's own resolver over a mapped GoW.exe (none of the game's code runs).

## Guidelines

- **Fail safe.** On anything unexpected (wrong build, guard bytes differ, a call fails), log it and leave the game as it is.
- **Threads.** The game runs its render thread, a main thread and many job threads; hooks are called from all of them. Keep shared state atomic or locked, and never block the render thread on the game thread.
- **No game files.** Never add the game's files, decompiled game code or data extracted from the game to this repository.
  The only exception is the short byte signatures in `src/core/addrsigs.inc` (a few dozen bytes per address, with the
  moving parts left open), which every pattern-scanning mod ships.
- **Comments.** Explain *why*, and keep the reason for each fix next to the code.

## Making a release

1. Build Release.
2. Copy `version.dll` into `files\`.
3. Update `files\BUILD.txt`.
4. Test on a clean game folder: `Install VR mod.cmd`, start the game, then `Uninstall VR mod.cmd` (it only moves the
   mod's own files to the Recycle Bin - never an .exe, a folder holding one, or the game folder).
5. The release zip holds this `GOW_VR_MOD` folder.
