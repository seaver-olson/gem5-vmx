.section .text.boot
.global _start
_start:
    cli
    lea stack_top(%rip), %rsp
    call paging_test
    ud2

/* Exceptions used by the test all push an error code. */
.global fault_entry
fault_entry:
    movq $14, fault_vector(%rip)
    jmp common_fault
.global gp_entry
gp_entry:
    movq $13, fault_vector(%rip)
    jmp common_fault
.global ss_entry
ss_entry:
    movq $12, fault_vector(%rip)
common_fault:
    push %rax
    mov 8(%rsp), %rax
    mov %rax, fault_error(%rip)
    mov resume_ip(%rip), %rax
    test %rax, %rax
    jz unexpected_fault
    mov %rax, 16(%rsp)
    movq $0, resume_ip(%rip)
    pop %rax
    add $8, %rsp
    iretq
unexpected_fault:
    mov $255, %rsi
    xor %edi, %edi
    .byte 0x0f, 0x04
    .word 0x22
    ud2

.global probe_read
probe_read:
    lea 1f(%rip), %rax
    mov %rax, resume_ip(%rip)
    mov (%rdi), %rax
1:  movq $0, resume_ip(%rip)
    ret

/* Jump instead of call, so an instruction-fetch fault has no extra stack. */
.global probe_exec
probe_exec:
    lea 1f(%rip), %rax
    mov %rax, resume_ip(%rip)
    jmp *%rdi
1:  movq $0, resume_ip(%rip)
    ret

.global probe_write
probe_write:
    mfence
    lea 1f(%rip), %rax
    mov %rax, resume_ip(%rip)
    movq $0xc3, (%rdi)
    mfence
1:  movq $0, resume_ip(%rip)
    ret

/* Keep the branch next to WRMSR so stale prefetched fault information is
 * observable when NXE changes without an explicit MOV CR3 afterwards. */
.global probe_efer_exec
probe_efer_exec:
    lea 1f(%rip), %rax
    mov %rax, resume_ip(%rip)
    mov %rdi, %rdx
    shr $32, %rdx
    mov %edi, %eax
    mov $0xc0000080, %ecx
    wrmsr
    jmp *%rsi
1:  movq $0, resume_ip(%rip)
    ret

.macro control_probe number
.global probe_cr\number
probe_cr\number:
    lea 1f(%rip), %rax
    mov %rax, resume_ip(%rip)
    mov %rdi, %cr\number
1:  movq $0, resume_ip(%rip)
    ret
.endm
control_probe 0
control_probe 3
control_probe 4

.section .bss
.balign 16
stack:
    .skip 16384
stack_top:
.section .note.GNU-stack,"",@progbits
