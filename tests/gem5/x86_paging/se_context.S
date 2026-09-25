/* Two binaries deliberately have identical virtual layouts and different data.
 * Their configured process IDs collide when truncated to a 12-bit PCID. */
.global _start
.text
_start:
    mov $20000, %ecx
1:  cmpl $IDENTITY, value(%rip)
    jne failure
    decl %ecx
    jnz 1b
    mov $1, %eax
    mov $1, %edi
    lea success(%rip), %rsi
    mov $5, %edx
    syscall
    xor %edi, %edi
    jmp exit
failure:
    mov $1, %eax
    mov $1, %edi
    lea failed(%rip), %rsi
    mov $5, %edx
    syscall
    mov $85, %edi
exit:
    mov $60, %eax
    syscall
    ud2
.data
.balign 4096
value: .long IDENTITY
success: .ascii "SE"
    .byte '0' + IDENTITY
    .ascii "!\n"
failed: .ascii "FAIL\n"
.section .note.GNU-stack,"",@progbits
