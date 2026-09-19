; Exact 1AU candidate. Door ownership plus duplicate bridge retirement.
; The main observer is a leaf. Retirement has a separate unwindable call frame.
EXTERN owner_frame:BYTE
EXTERN native_base:QWORD
EXTERN prior_observer:PROC
EXTERN bridge_retire:PROC
.code
entrance_v5 PROC
    mov QWORD PTR [rsp+8], rcx
    mov QWORD PTR [rsp+16], rdx
    lea r10, owner_frame
    ; Frame bytes: enabled, finished, restricted, fault.
    mov eax, DWORD PTR [r10]
    and eax, 0FF00FFFFh
    cmp eax, 1
    jne early_finished
    nop
    cmp BYTE PTR [r10+5], 2
    ja early_finished
    mov r8, native_base
    test r8, r8
    jz early_finished
    cmp DWORD PTR [r8+01F93430h], 0E0h
    jne early_finished
    mov r9, QWORD PTR [r8+01F93428h]
    test r9, r9
    jz early_finished
    jmp begin_scan
early_finished:
    jmp finished
begin_scan:
    xor r10d, r10d
next_entity:
    test BYTE PTR [r9+4], 5
    jnz advance
    mov eax, DWORD PTR [r9+0Ch]
    cmp eax, -1
    je advance
    and eax, 01FFFh
    cmp eax, r10d
    jne advance
    mov eax, DWORD PTR [r9+088h]
    cmp eax, 080F0C055h
    je bridge
    cmp eax, 080C32CEEh
    jne advance
    lea r11, owner_frame
    cmp BYTE PTR [r11+5], 0
    je advance
    mov eax, DWORD PTR [r9+08Ch]
    cmp eax, 1
    ja advance
    mov r11, 030DB525724EDDB9Fh
    test eax, eax
    jz door_guid
    mov r11, 0AB48F4FA0B44B151h
door_guid:
    cmp QWORD PTR [r9+090h], r11
    jne advance
    lock bts DWORD PTR [r8+026BE0E0h], r10d
    jmp advance
bridge:
    bt DWORD PTR [r8+026BE0E0h], r10d
    jc advance
    mov eax, DWORD PTR [r9+08Ch]
    sub eax, 91
    cmp eax, 33
    ja advance
    mov r11, 0200020807h
    bt r11, rax
    jnc advance
    mov r11, QWORD PTR [r8+01F93428h]
    mov ecx, 8192
find_owned:
    test BYTE PTR [r11+4], 5
    jnz next_match
    cmp DWORD PTR [r11+088h], 080F0C055h
    jne next_match
    mov eax, DWORD PTR [r9+08Ch]
    cmp DWORD PTR [r11+08Ch], eax
    jne next_match
    mov rax, QWORD PTR [r9+090h]
    cmp QWORD PTR [r11+090h], rax
    jne next_match
    mov eax, DWORD PTR [r11+0Ch]
    cmp eax, -1
    je next_match
    and eax, 01FFFh
    mov edx, 8192
    sub edx, ecx
    cmp eax, edx
    jne next_match
    bt DWORD PTR [r8+026BE0E0h], eax
    jc bridge_retire
next_match:
    add r11, 0E0h
    dec ecx
    jnz find_owned
advance:
    add r9, 0E0h
    inc r10d
    cmp r10d, 8192
    jne next_entity
finished:
    mov rcx, QWORD PTR [rsp+8]
    mov rdx, QWORD PTR [rsp+16]
    jmp prior_observer
entrance_v5 ENDP
END
