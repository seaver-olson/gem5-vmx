/* SPDX-License-Identifier: BSD-3-Clause */
/* Enter 32-bit protected mode from gem5's long-mode ELF bootstrap. */
.section .text.boot
.global _start
.code64
_start:
    cli
    lea stack_top(%rip), %rsp
    lgdt gdt_pointer(%rip)
    pushq $16
    lea compatibility(%rip), %rax
    push %rax
    lretq

.code32
compatibility:
    mov $24, %eax
    mov %eax, %ds
    mov %eax, %es
    mov %eax, %ss
    mov %cr0, %eax
    and $0x7fffffff, %eax
    mov %eax, %cr0
    mov $0xc0000080, %ecx
    rdmsr
    and $0xfffffeff, %eax
    wrmsr
    mov %cr4, %eax
    and $0xffffffdf, %eax
    or $16, %eax
    mov %eax, %cr4

    /* Identity map 64 MiB with 4 MiB PDEs for code, stack, and tables. */
    xor %ecx, %ecx
1:  mov %ecx, %eax
    shl $22, %eax
    or $0x87, %eax
    mov %eax, directory(,%ecx,4)
    inc %ecx
    cmp $16, %ecx
    jne 1b
    /* An identity PT also lets us disable PSE later. */
    xor %ecx, %ecx
1:  mov %ecx, %eax
    shl $12, %eax
    or $7, %eax
    mov %eax, identity_pt(,%ecx,4)
    inc %ecx
    cmp $1024, %ecx
    jne 1b
    mov $directory, %eax
    mov %eax, %cr3
    mov %cr0, %eax
    or $0x80010000, %eax
    mov %eax, %cr0
    movl $0x12345678, 0x400000
    movl $0x87654321, 0x600000

    /* PSE enabled with PS clear: VA index 3 differs from PDE index 16. */
    mov $table, %eax
    or $7, %eax
    mov %eax, directory + 16*4
    movl $0x400007, table + 3*4
    call flush
    mov $1, %esi
    cmpl $0x12345678, 0x04003000
    jne fail
    mfence
    inc %esi
    testl $32, directory + 16*4
    jz fail
    inc %esi
    testl $32, table + 3*4
    jz fail
    inc %esi
    testl $64, table + 3*4
    jnz fail
    mfence
    movl $0x11223344, 0x04003000
    mfence
    inc %esi
    testl $64, table + 3*4
    jz fail

    /* A 4 MiB cached mapping must retain offsets above 2 MiB. */
    movl $0x400087, directory + 16*4
    call flush
    mov $8, %esi
    cmpl $0x11223344, 0x04000000
    jne fail
    inc %esi
    cmpl $0x87654321, 0x04200000
    jne fail
    mfence
    inc %esi
    testl $32, directory + 16*4
    jz fail
    inc %esi
    testl $64, directory + 16*4
    jnz fail
    mfence
    movl $0x99aabbcc, 0x04200000
    mfence
    inc %esi
    testl $64, directory + 16*4
    jz fail
    inc %esi
    cmpl $0x99aabbcc, 0x600000
    jne fail
    movl $0x400087, directory + 16*4
    call flush
    movl $0x99aabbcc, 0x04200000
    mfence
    inc %esi
    testl $64, directory + 16*4
    jz fail

    /* Repeat 4 KiB A/D with PSE disabled. Negative tests require a
     * translation fixture: legacy error-code delivery is unimplemented. */
    mov $identity_pt, %eax
    or $7, %eax
    mov %eax, directory
    mov $table, %eax
    or $7, %eax
    mov %eax, directory + 16*4
    movl $0x400007, table + 3*4
    mov %cr4, %eax
    and $~16, %eax
    mov %eax, %cr4
    call flush
    mov $16, %esi
    cmpl $0x11223344, 0x04003000
    jne fail
    mfence
    inc %esi
    testl $32, directory + 16*4
    jz fail
    inc %esi
    testl $32, table + 3*4
    jz fail
    mfence
    movl $0x11223344, 0x04003000
    mfence
    inc %esi
    testl $64, table + 3*4
    jz fail
    xor %esi, %esi
fail:
    xor %edi, %edi
    .byte 0x0f, 0x04
    .word 0x22
    ud2
flush:
    mov $directory, %eax
    mov %eax, %cr3
    ret
.section .rodata
.balign 8
gdt:
    .quad 0
    .quad 0x00af9a000000ffff /* 64-bit code */
    .quad 0x00cf9a000000ffff /* 32-bit code */
    .quad 0x00cf92000000ffff /* data */
gdt_pointer:
    .word 31
    .quad gdt
.section .bss
.balign 4096
directory: .skip 4096
table: .skip 4096
identity_pt: .skip 4096
.balign 16
stack: .skip 16384
stack_top:
.section .note.GNU-stack,"",@progbits
