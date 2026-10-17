; Export stubs for the version.dll proxy. Each stub tail-jumps through a pointer that
; proxy.cpp fills from the real %SystemRoot%\System32\version.dll, so the stubs are
; signature-agnostic and preserve every argument register and the stack exactly.

STUB MACRO name
    EXTERN g_real_&name:QWORD
    PUBLIC stub_&name
stub_&name PROC
    jmp QWORD PTR [g_real_&name]
stub_&name ENDP
ENDM

.code
STUB GetFileVersionInfoA
STUB GetFileVersionInfoByHandle
STUB GetFileVersionInfoExA
STUB GetFileVersionInfoExW
STUB GetFileVersionInfoSizeA
STUB GetFileVersionInfoSizeExA
STUB GetFileVersionInfoSizeExW
STUB GetFileVersionInfoSizeW
STUB GetFileVersionInfoW
STUB VerFindFileA
STUB VerFindFileW
STUB VerInstallFileA
STUB VerInstallFileW
STUB VerLanguageNameA
STUB VerLanguageNameW
STUB VerQueryValueA
STUB VerQueryValueW
END
