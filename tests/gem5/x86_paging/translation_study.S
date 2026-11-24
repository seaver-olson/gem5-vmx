/* Copyright (c) 2026 Seaver Olson, Loyola University Chicago
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "vmresume.S"

.text
.global study_split, study_word, study_alias, study_root_read
study_split:
    movzbl 0x800fff, %eax
study_word:
    movzwl 0x800fff, %eax
    mov %rax, study_value(%rip)
    vmcall
    ud2

study_root_read:
    movzbl 0x800fff, %eax
    mov %rax, study_value(%rip)
    vmcall
    ud2

study_alias:
    movzbl 0xc00000, %eax
    // More distinct guest leaves than the four-entry guest TLB.
    lea study_pressure(%rip), %rdx
    mov $8, %ecx
1:  mov (%rdx), %al
    add $4096, %rdx
    dec %ecx
    jnz 1b
    movzbl 0xc00000, %eax
    mov %rax, study_before(%rip)
    movq $0x801067, study_alias_pt(%rip)
    invlpg (0xc00000)
    movzbl 0xc00000, %eax
    mov %rax, study_value(%rip)
    vmcall
    ud2

.global study_loop, study_loop_guest
study_loop_guest:
    call study_loop
    vmcall
    ud2

// Both native and nested execution use these exact instruction bytes.
study_loop:
    mov study_first(%rip), %rdx
    xor %r8d, %r8d
    mov $4096, %ecx
    xor %edi, %edi
    .byte 0x0f, 0x04
    .word 0x21
    // TimingSimple can begin the next instruction while servicing m5_exit.
    // Give each drain a register-only instruction before the next data access.
    nop
    nop
1:  mov (%rdx), %rdx
    rol $7, %r8
    add %rdx, %r8
    dec %ecx
    jnz 1b
    .byte 0x0f, 0x04
    .word 0x21
    nop
    nop
    mov %r8, study_value(%rip)
    ret
