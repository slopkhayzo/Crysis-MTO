; Common entry of the call tracer's per-slot thunks (calltrace.cpp), x64.
; On entry: eax = slot id, [rsp] = return address into the calling module, rcx/rdx/r8/r9 and xmm0-xmm3 = the call's
; arguments (the thunk jumped here, so the stack is exactly as the hooked function would see it).
; Saves the argument registers, lets TraceOnEnter64 count the call, then jumps to the original function, which it
; returns. x64 only counts: swapping the return address (as the x86 version does for timing) would break unwinding.

EXTERN TraceOnEnter64:PROC

.CODE
TraceEnter64 PROC FRAME
    sub     rsp, 88h                    ; entry rsp is 8 mod 16: this aligns it for the call
    .allocstack 88h
    .endprolog
    mov     [rsp+20h], rcx
    mov     [rsp+28h], rdx
    mov     [rsp+30h], r8
    mov     [rsp+38h], r9
    movaps  xmmword ptr [rsp+40h], xmm0
    movaps  xmmword ptr [rsp+50h], xmm1
    movaps  xmmword ptr [rsp+60h], xmm2
    movaps  xmmword ptr [rsp+70h], xmm3
    mov     ecx, eax                    ; slot
    mov     rdx, [rsp+88h]              ; return address
    call    TraceOnEnter64              ; -> original function
    mov     rcx, [rsp+20h]
    mov     rdx, [rsp+28h]
    mov     r8, [rsp+30h]
    mov     r9, [rsp+38h]
    movaps  xmm0, xmmword ptr [rsp+40h]
    movaps  xmm1, xmmword ptr [rsp+50h]
    movaps  xmm2, xmmword ptr [rsp+60h]
    movaps  xmm3, xmmword ptr [rsp+70h]
    add     rsp, 88h
    jmp     rax
TraceEnter64 ENDP

END
