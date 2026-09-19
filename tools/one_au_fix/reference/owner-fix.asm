; 1AU bridge Ghost ownership repair, exact candidate ABI.
; Called by the existing, gated E4A590 observer. This is a leaf: it changes no
; stack or nonvolatile register and tail-calls the original observer unchanged.
; Only the console's entity-authority bit is written. No playback/progress data.
EXTERN owner_frame:BYTE
EXTERN native_base:QWORD
EXTERN prior_observer:PROC
.code
owner_fix PROC
    test rcx, rcx
    jz finished
    cmp DWORD PTR [rcx], 080B3C98Fh
    jne finished
    cmp DWORD PTR [rcx+4], 080804D32h
    jne finished
    cmp QWORD PTR [rcx+8], 0258h
    jne finished
    lea r10, owner_frame
    cmp BYTE PTR [r10], 1
    jne finished
    cmp WORD PTR [r10+1], 0
    jne finished
    cmp BYTE PTR [r10+5], 1
    jne finished
    cmp BYTE PTR [r10+33968], 1
    jne finished
    cmp BYTE PTR [r10+33970], 0
    jne finished
    mov eax, DWORD PTR [r10+8]
    inc eax
    cmp DWORD PTR [rcx+01C0h], eax
    jne finished
    cmp BYTE PTR [rcx+01C4h], 1
    jne finished
    cmp DWORD PTR [rcx+01C8h], 0811C9DC5h
    jne finished
    mov r11, native_base
    test r11, r11
    jz finished
    mov r8d, DWORD PTR [rcx+01DCh]
    cmp r8d, -1
    je finished
    mov r9, QWORD PTR [r11+02439C70h]
    test r9, r9
    jz finished
    ; Validate the salted Weak reference through the native registry metadata.
    mov eax, r8d
    sar eax, 31
    and eax, 03C00h
    or eax, 03FFh
    mov edx, r8d
    shr edx, 13
    and edx, 0FFFFh
    and eax, edx
    movsxd rdx, DWORD PTR [r9+010h]
    test edx, edx
    jle finished
    cmp edx, 01000h
    ja finished
    imul rax, rdx
    add rax, QWORD PTR [r9]
    mov rax, QWORD PTR [rax+010h]
    test rax, rax
    jz finished
    mov rdx, QWORD PTR [rax]
    test rdx, rdx
    jz finished
    mov r10d, r8d
    and r10d, 01FFFh
    cmp r10w, WORD PTR [rdx+01Ch]
    jae finished
    mov edx, DWORD PTR [rax+020h]
    test edx, edx
    jz finished
    cmp edx, 0100000h
    ja finished
    imul r10, rdx
    mov edx, DWORD PTR [rax+01Ch]
    add r10, rdx
    add r10, QWORD PTR [rax+8]
    mov eax, DWORD PTR [r10]
    cmp eax, DWORD PTR [rcx+01D8h]
    jne finished
    ; Resolve the current allocation, including native compaction correction.
    mov eax, r8d
    sar eax, 13
    mov edx, eax
    or edx, 0FFC0000h
    shr edx, 18
    and eax, 0FFFFh
    and edx, eax
    shl rdx, 6
    add rdx, QWORD PTR [r9]
    movsxd rax, DWORD PTR [rdx+030h]
    test eax, eax
    jle finished
    cmp eax, 0100000h
    ja finished
    mov r9d, r8d
    and r9d, 01FFFh
    imul r9, rax
    add r9, QWORD PTR [rdx+8]
    movsxd rax, DWORD PTR [rdx+034h]
    and rax, QWORD PTR [r9+8]
    sub r9, rax
    mov rax, 080804D3A80C3D38Ch
    cmp QWORD PTR [r9], rax
    jne finished
    cmp QWORD PTR [r9+8], 0358h
    jne finished
    cmp DWORD PTR [r9+024h], r8d
    jne finished
    mov eax, DWORD PTR [rcx+01C0h]
    cmp DWORD PTR [r9+0294h], eax
    jne finished
    mov r8d, DWORD PTR [r9+02Ch]
    cmp r8d, -1
    je finished
    mov rax, QWORD PTR [r11+01F93428h]
    test rax, rax
    jz finished
    mov edx, DWORD PTR [r11+01F93430h]
    cmp edx, 050h
    jb finished
    cmp edx, 0100000h
    ja finished
    mov r9d, r8d
    and r9d, 01FFFh
    imul r9, rdx
    add r9, rax
    cmp DWORD PTR [r9+0Ch], r8d
    jne finished
    test BYTE PTR [r9+4], 4
    jnz finished
    lea r10, owner_frame
    mov eax, DWORD PTR [r10+8]
    inc eax
    cmp DWORD PTR [rcx+01C0h], eax
    jne finished
    cmp BYTE PTR [r10], 1
    jne finished
    mov eax, r8d
    and eax, 01FFFh
    shr eax, 5
    and r8d, 31
    bts DWORD PTR [r11+rax*4+026BE0E0h], r8d
finished:
    jmp prior_observer
owner_fix ENDP
END
