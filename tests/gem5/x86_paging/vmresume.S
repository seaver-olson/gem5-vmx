// Reuse the disk-free boot, exception handlers and VMX operand probes.
#include "vmx_operands.S"
.text
vmx_probe op_vmresume, "vmresume"

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
