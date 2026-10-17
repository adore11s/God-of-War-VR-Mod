; Entry thunk for the stack probe. Reached by a jmp from a detour relay at a function's first instruction, so [rsp]
; is the caller's return address. Saves argument registers (rcx rdx r8 r9 rax xmm0-5), captures the callee-saved
; registers (still the caller's values at entry), calls StackProbeRecord(regs), restores everything and continues into
; the original function through its trampoline.

EXTERN StackProbeRecord:PROC
EXTERN g_probeTrampoline:QWORD

.code
stack_probe_thunk PROC
    push rcx
    push rdx
    push r8
    push r9                         ; rsp = entry - 20h  (entry rsp is 8 mod 16)
    sub rsp, 0D8h                   ; 0x28 shadow + 0x60 xmm + 8 rax + 0x48 register block; keeps 16-byte alignment
    movdqu xmmword ptr [rsp+28h], xmm0
    movdqu xmmword ptr [rsp+38h], xmm1
    movdqu xmmword ptr [rsp+48h], xmm2
    movdqu xmmword ptr [rsp+58h], xmm3
    movdqu xmmword ptr [rsp+68h], xmm4
    movdqu xmmword ptr [rsp+78h], xmm5
    mov [rsp+88h], rax
    lea rax, [rsp+0F8h]             ; entry rsp (points at the return address)
    mov [rsp+90h], rax
    mov [rsp+98h], rbx
    mov [rsp+0A0h], rbp
    mov [rsp+0A8h], rsi
    mov [rsp+0B0h], rdi
    mov [rsp+0B8h], r12
    mov [rsp+0C0h], r13
    mov [rsp+0C8h], r14
    mov [rsp+0D0h], r15
    lea rcx, [rsp+90h]
    call StackProbeRecord
    movdqu xmm0, xmmword ptr [rsp+28h]
    movdqu xmm1, xmmword ptr [rsp+38h]
    movdqu xmm2, xmmword ptr [rsp+48h]
    movdqu xmm3, xmmword ptr [rsp+58h]
    movdqu xmm4, xmmword ptr [rsp+68h]
    movdqu xmm5, xmmword ptr [rsp+78h]
    mov rax, [rsp+88h]
    add rsp, 0D8h
    pop r9
    pop r8
    pop rdx
    pop rcx
    jmp qword ptr [g_probeTrampoline]
stack_probe_thunk ENDP
END
