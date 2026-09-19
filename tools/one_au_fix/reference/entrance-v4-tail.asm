.code
EXTERN prior_observer:PROC
EXTERN continue_scan:PROC
bridge_retire PROC
    ; Authored placement identity exists before its component bundle is ready.
    ; The native retirement routine dereferences that bundle without checking it.
    ; Skip incomplete/retiring rows and keep scanning the remaining placements.
    test BYTE PTR [r9+4], 5
    jnz continue_scan
    cmp DWORD PTR [r9+04Ch], -1
    je continue_scan
    mov rcx, QWORD PTR [rsp+8]
    mov eax, DWORD PTR [r9+0Ch]
    cmp DWORD PTR [rcx+02Ch], eax
    je skip_retire
    bt DWORD PTR [r8+026BE0E0h], r10d
    jc skip_retire
    mov rdx, QWORD PTR [rsp+16]
    jmp retire_one
skip_retire:
    mov rdx, QWORD PTR [rsp+16]
    jmp prior_observer
bridge_retire ENDP

retire_one PROC FRAME
    sub rsp, 56
    .allocstack 56
    .endprolog
    mov QWORD PTR [rsp+32], rcx
    mov QWORD PTR [rsp+40], rdx
    mov ecx, eax
    lea rax, [r8+056A8F0h]
    call rax
    mov rcx, QWORD PTR [rsp+32]
    mov rdx, QWORD PTR [rsp+40]
    add rsp, 56
    jmp prior_observer
retire_one ENDP
END
