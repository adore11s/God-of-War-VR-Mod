#pragma once

struct ID3D11Device;

namespace gowvr::diag {

// D-099: vectored exception logger for exceptions inside GoW.exe (never handles them).
void InstallCrashLog();
// D-105: detours that turn known engine crashes the mod can provoke into logged skips (DLL load, before rendering).
void InstallCrashGuards();
// D-112: crash reports - gowvr_logs\crash_reports\crash_<date>.txt written when the game really crashes (the unhandled
// exception filter, chained in front of the game's own): PC / GPU / headset, mod and game build, the mod's state, the
// crash address, registers, stack and the last log lines - without the user name. Players send that file.
void CrashNoteDevice(ID3D11Device* dev);  // device creation: the GPU and its driver for the report
void CrashFilterKeep();                          // command thread, 1 Hz: our filter stays first (chains a newer one)
void CrashReportTest();                          // command "crashreport": a sample report (no crash)
// D-220g: game thread, once per frame - watches the game's 128 render pass objects and logs the frame one dies.
void PassTableWatch();

}  // namespace gowvr::diag
