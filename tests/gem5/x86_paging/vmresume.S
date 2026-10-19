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

#ifdef VMX_MIGRATE_TEST
// A real INIT/SIPI starts CPU 1 here, not at a copied ThreadContext.
// The BSP copies this position-independent bootstrap to physical 0x8000.
.code16
.global ap_boot, ap_boot_end, ap_gdtr, ap_root
ap_boot:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    lgdtl (0x8000 + ap_gdtr - ap_boot)
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmpl $40, $(0x8000 + ap_protected - ap_boot)
.code32
ap_protected:
    mov $16, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov (0x8000 + ap_root - ap_boot), %eax
    mov %eax, %cr3
    mov %cr4, %eax
    or $0x2020, %eax
    mov %eax, %cr4
    mov $0xc0000080, %ecx
    rdmsr
    or $0x100, %eax
    wrmsr
    mov %cr0, %eax
    or $0x80000021, %eax
    mov %eax, %cr0
    ljmp $8, $(0x8000 + ap_long - ap_boot)
.code64
ap_long:
    movabs $(ap_stack + 8192), %rsp
    movabs $migration_ap, %rax
    call *%rax
1:  hlt
    jmp 1b
.balign 8
ap_gdtr:
    .word 47
    .long 0
ap_root:
    .long 0
ap_boot_end:
#endif

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

#ifdef VMX_NMI_TEST
// Check blocking before queuing a second NMI, then require non-nested
// delivery through IRET. Guest clobbers remain caller-saved.
.global nmi_guest, nmi_handler, nmi_handler_exit, nmi_done
.global nmi_priority, nmi_fixed_handler
nmi_priority:
    mov $0xfee00300, %eax
    movl $0x40400, (%rax)
    movl $0x40040, (%rax) // Fixed interrupt must pass the blocked NMI.
    jmp nmi_wait
nmi_fixed_handler:
    incq nmi_fixed(%rip)
    push %rax
    mov $0xfee000b0, %eax
    movl $0, (%rax) // EOI before the next fixed-interrupt test.
    pop %rax
    iretq
nmi_guest:
    mov $0xfee00300, %eax
    movl $0x40400, (%rax) // Self NMI through the real local APIC.
nmi_wait:
    // Device completion/interrupt dispatch is asynchronous on O3. Wait for
    // actual handler progress, not a fixed number of simulated cycles.
    mov $100000, %ecx
1:  cmpq $2, nmi_events(%rip)
    je nmi_done
    dec %ecx
    jnz 1b
    mov $302, %esi
    xor %edi, %edi
    .byte 0x0f, 0x04; .word 0x22
    ud2
nmi_handler:
    push %rax
    incq nmi_depth(%rip)
    cmpq $1, nmi_depth(%rip)
    jne nmi_nested
    incq nmi_events(%rip)
    cmpq $1, nmi_events(%rip)
    jne nmi_tail
nmi_handler_exit:
    vmcall
    mov $0xfee00300, %eax
    movl $0x40400, (%rax)
nmi_tail:
    decq nmi_depth(%rip)
    pop %rax
    iretq
nmi_nested:
    mov $301, %esi
    xor %edi, %edi
    .byte 0x0f, 0x04; .word 0x22
    ud2
nmi_done:
    vmcall
    ud2

.global nmi_load, nmi_fault_handler, nmi_wrong_path
nmi_load:
    mov (%rsp), %rax
    jmp nmi_done
nmi_fault_handler:
    vmcall
    ud2
nmi_wrong_path:
    xor %eax, %eax
    test %eax, %eax
    jz nmi_done
    iretq // Must not remove NMI blocking on a wrong path.
    ud2

// Isolate the IRET fetch page from the handler and ordinary load probe.
.balign 4096
.global nmi_iret
nmi_iret:
    iretq
#endif

#ifdef VMX_ADDRESS_TEST
.global address_guest, address_exit, address_exit2
address_guest:
    incq guest_runs(%rip)
    mov %cr3, %rax
    or $1, %rax // Legal ignored CR3 bit, including across exit and resume.
    mov %rax, %cr3
address_exit:
    vmcall
    incq guest_runs(%rip)
address_exit2:
    vmcall
    ud2
#endif
