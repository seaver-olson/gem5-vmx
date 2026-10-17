// Reuse the disk-free boot, exception handlers and VMX operand probes.
#include "vmx_operands.S"
.text
vmx_probe op_vmresume, "vmresume"
vmx_probe op_vmlaunch, "vmlaunch"
vmx_probe op_vmcall, "vmcall"
vmx_probe op_ud2, "ud2"

// Return zero on a VM exit, or the VMfail flags on failed entry. The guest
// uses no callee-saved registers; VM exit restores this call's host stack.
.global enter_guest
enter_guest:
    mov $0x6c14, %ecx // HOST_RSP
    vmwrite %rsp, %rcx
    lea host_return(%rip), %rax
    mov $0x6c16, %ecx // HOST_RIP
    vmwrite %rax, %rcx
    test %edi, %edi
    jnz resume_guest
    vmlaunch
    jmp entry_failed
resume_guest:
    vmresume
entry_failed:
    pushfq
    pop %rax
    ret
host_return:
    xor %eax, %eax
    ret

.global guest_entry, guest_exit
guest_entry:
    incq guest_runs(%rip)
guest_exit:
    vmcall
    ud2

// Normalize the exception frame, record the injected payload, then exit.
// No callee-saved registers are changed across enter_guest.
.global injected_ud, injected_gp, injected_exit
injected_ud:
    pushq $0
    jmp injected_common
injected_gp:
injected_common:
    push %rax
    mov 8(%rsp), %rax
    mov %rax, injected_error(%rip)
    mov 16(%rsp), %rax
    mov %rax, injected_rip(%rip)
    mov 32(%rsp), %rax
    mov %rax, injected_flags(%rip)
    incq injected_events(%rip)
    pop %rax
    add $8, %rsp
injected_exit:
    vmcall
    ud2

#ifdef EPT_ENTRY_TEST
/* One remapped code page; absolute operands need no guest GPR save/restore
 * in this deliberately minimal VMM when a faulting instruction is resumed. */
.balign 4096
.global ept_guest_read, ept_guest_write, ept_guest_exit
ept_guest_read:
    mov 0x401000, %eax
ept_guest_write:
    movl $0x12345678, 0x401000
ept_guest_exit:
    vmcall
    ud2
#endif

#ifdef VMX_TRANSITION_TEST
// Keep all guest clobbers caller-saved, as required by enter_guest's ABI.
.global probe_wrmsr
probe_wrmsr:
    mov %edi, %ecx
    mov %esi, %eax
    mov %rsi, %rdx
    shr $32, %rdx
    lea 1f(%rip), %r8
    mov %r8, resume_ip(%rip)
    wrmsr
1:  movq $0, resume_ip(%rip)
    ret

.global transition_guest, transition_exit1, transition_exit2
transition_guest:
    mov 0x400000, %rax
    mov %rax, transition_seen(%rip)
    mov transition_root_b(%rip), %rax
    mov %rax, %cr3
    mov 0x400000, %rax
    mov %rax, transition_seen+8(%rip)
    mov %cr3, %rax
    mov %rax, transition_seen+16(%rip)
    movabs $0x535441434b, %rax
    push %rax
    pop %rax
    mov %rax, transition_seen+24(%rip)
transition_exit1:
    vmcall
    incq transition_seen+32(%rip)
transition_exit2:
    vmcall
    ud2

.global transition_clts, transition_clts_exit
transition_clts:
    clts
    mov %cr0, %rax
    mov %rax, transition_seen+40(%rip)
transition_clts_exit:
    vmcall
    ud2

.global transition_cr3, transition_cr3_instruction
transition_cr3:
    mov transition_root_b(%rip), %rax
transition_cr3_instruction:
    mov %rax, %cr3
    vmcall
    ud2

.global transition_pf, transition_getsec
transition_pf:
    mov 0x600000, %rax
    ud2
transition_getsec:
    .byte 0x0f, 0x37
    ud2
.global transition_xsetbv, transition_xsetbv_instruction
transition_xsetbv:
    xor %ecx, %ecx
transition_xsetbv_instruction:
    xsetbv
    ud2
#endif
