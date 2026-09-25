#include "entry.S"
.text
.global ud_entry
ud_entry:
    pushq $0
    movq $6, fault_vector(%rip)
    jmp common_fault

/* Same ABI for every probe: (address, field, value). Seed all six VMX
 * status flags so both successful clearing and preservation on #PF are tested. */
.macro vmx_probe name, instruction
.global \name
\name:
    lea 1f(%rip), %rax
    mov %rax, resume_ip(%rip)
    pushq $0x8d7
    popfq
    \instruction
1:  pushfq
    pop %rax
    movq $0, resume_ip(%rip)
    ret
.endm
vmx_probe op_vmxon, "vmxon (%rdi)"
vmx_probe op_vmclear, "vmclear (%rdi)"
vmx_probe op_vmptrld, "vmptrld (%rdi)"
vmx_probe op_vmptrst, "vmptrst (%rdi)"
vmx_probe op_vmwrite_mem, "vmwrite (%rdi), %rsi"
vmx_probe op_vmwrite_reg, "vmwrite %rdx, %rsi"
vmx_probe op_vmread_mem, "vmread %rsi, (%rdi)"
vmx_probe op_vmread_reg, "vmread %rsi, %rdx"
vmx_probe op_vmxoff, "vmxoff"

.global read_field
read_field:
    vmread %rdi, %rax
    ret
/* Exercise real base/index and RIP-relative dependencies, with no intervening
 * serializing instructions supplied by the test itself. */
.global op_vmxon_sib
op_vmxon_sib:
    mov $3, %r8
    sub $40, %rdi
    vmxon 16(%rdi,%r8,8)
    pushfq
    pop %rax
    ret
.global op_vmptrst_rip
op_vmptrst_rip:
    vmptrst rip_pointer(%rip)
    pushfq
    pop %rax
    ret
.global op_vmread_rip
op_vmread_rip:
    vmread %rsi, rip_pointer(%rip)
    pushfq
    pop %rax
    ret
.global op_vmwrite_rip
op_vmwrite_rip:
    vmwrite rip_pointer(%rip), %rsi
    pushfq
    pop %rax
    ret
