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
