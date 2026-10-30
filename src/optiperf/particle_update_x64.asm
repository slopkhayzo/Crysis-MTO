; The particle update's serial stubs' common code (particle_update.cpp), x64.
; A stub sets eax = its id and jumps to ParticleSerialCheck64, which reads the calling thread's flag (t_particleSerial,
; optiperf's static TLS: set on the threads making a batch of emitter updates) and goes straight on to the stub's
; function when it is clear, else on to ParticleSerial64. Both start with the stack exactly as the function would see
; it: [rsp] = the return address into the caller, rcx/rdx/r8/r9 and xmm0-xmm3 = the call's arguments, the caller's
; stack arguments from [rsp+28h]. The check touches only rax, r10 and r11 (scratch at a call in the x64 convention).
; ParticleSerial64 makes the call under the serial lock: ParticleSerialEnter(id) takes it, the caller's first 12 stack
; arguments are copied below a frame of our own (none of the functions takes more, or a variable list), the function
; is called with the argument registers as they came, then ParticleSerialLeave() lets the lock go. rax and xmm0 (the
; function's result) are kept across that call.

EXTERN ParticleSerialEnter:PROC
EXTERN ParticleSerialLeave:PROC
EXTERN g_particleSerialTargets:QWORD
EXTERN t_particleSerial:BYTE
EXTERN _tls_index:DWORD

.CODE
ParticleSerial64 PROC FRAME
    sub     rsp, 0F8h                   ; entry rsp is 8 mod 16: this aligns it for the calls
    .allocstack 0F8h
    .endprolog
    mov     [rsp+80h], rcx
    mov     [rsp+88h], rdx
    mov     [rsp+90h], r8
    mov     [rsp+98h], r9
    movaps  xmmword ptr [rsp+0A0h], xmm0
    movaps  xmmword ptr [rsp+0B0h], xmm1
    movaps  xmmword ptr [rsp+0C0h], xmm2
    movaps  xmmword ptr [rsp+0D0h], xmm3
    mov     dword ptr [rsp+0E0h], eax
    mov     ecx, eax
    call    ParticleSerialEnter         ; (the lock, counted per id)
    xor     r10d, r10d
copy_args:
    mov     rax, [rsp+r10*8+120h]       ; the caller's stack arguments: entry rsp + 28h = rsp + 0F8h + 28h
    mov     [rsp+r10*8+20h], rax        ; ... where the function will find them: its rsp + 28h
    inc     r10d
    cmp     r10d, 12
    jb      copy_args
    mov     rcx, [rsp+80h]
    mov     rdx, [rsp+88h]
    mov     r8, [rsp+90h]
    mov     r9, [rsp+98h]
    movaps  xmm0, xmmword ptr [rsp+0A0h]
    movaps  xmm1, xmmword ptr [rsp+0B0h]
    movaps  xmm2, xmmword ptr [rsp+0C0h]
    movaps  xmm3, xmmword ptr [rsp+0D0h]
    mov     eax, dword ptr [rsp+0E0h]
    mov     r11, g_particleSerialTargets  ; (the stubs' table: what each one goes on to)
    call    qword ptr [r11+rax*8]
    mov     [rsp+80h], rax
    movaps  xmmword ptr [rsp+0A0h], xmm0
    call    ParticleSerialLeave
    mov     rax, [rsp+80h]
    movaps  xmm0, xmmword ptr [rsp+0A0h]
    add     rsp, 0F8h
    ret
ParticleSerial64 ENDP

; eax = the stub's id; no frame (a leaf: the stack is the caller's call)
ParticleSerialCheck64 PROC
    mov     r10d, _tls_index
    mov     r11, gs:[58h]               ; the TEB's ThreadLocalStoragePointer
    mov     r11, [r11+r10*8]            ; this module's TLS block
    mov     r10d, SECTIONREL t_particleSerial
    cmp     byte ptr [r11+r10], 0
    jne     ParticleSerial64            ; a batch thread: the call under the lock
    mov     r11, g_particleSerialTargets
    jmp     qword ptr [r11+rax*8]       ; else straight on
ParticleSerialCheck64 ENDP

END
