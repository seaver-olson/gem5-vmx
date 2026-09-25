/* SPDX-License-Identifier: BSD-3-Clause */
.section .text.boot
.global _start, gdb_ready
_start:
    cli
    lea stack_top(%rip), %rsp
    mov %cr3, %rax
    mov (%rax), %rax
    mov %rax, root(%rip)
    mov $pdpt+7, %rax
    mov %rax, root+8(%rip)
    mov $pd+7, %rax
    mov %rax, pdpt(%rip)
    mov $pt+7, %rax
    mov %rax, pd(%rip)
    mov $first+7, %rax
    mov %rax, pt(%rip)
    mov %rax, pt+24(%rip)
    mov $second+7, %rax
    mov %rax, pt+8(%rip)
    movabs $0x0123456789abcdef, %rax
    mov %rax, first(%rip)
    movabs $0xfedcba9876543210, %rax
    mov %rax, second(%rip)
    movw $0xbbaa, first+4094(%rip)
    mov $root, %rax
    mov %rax, %cr3
gdb_ready:
    nop
    /* Debugger reads must not change guest page-table A/D bits. */
    mov root+8(%rip), %rax
    or pdpt(%rip), %rax
    or pd(%rip), %rax
    or pt(%rip), %rax
    or pt+8(%rip), %rax
    or pt+24(%rip), %rax
    xor %esi, %esi
    test $0x60, %rax
    setnz %sil
    xor %edi, %edi
    .byte 0x0f, 0x04
    .word 0x22
    ud2
.section .bss
.balign 4096
root: .skip 4096
pdpt: .skip 4096
pd: .skip 4096
pt: .skip 4096
first: .skip 4096
second: .skip 4096
stack: .skip 4096
stack_top:
.section .note.GNU-stack,"",@progbits
