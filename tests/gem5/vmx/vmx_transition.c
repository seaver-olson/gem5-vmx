// SPDX-License-Identifier: GPL-2.0
/*
 * Architectural VM-entry/VM-exit and CR3 translation regression test.
 *
 * The same kernel virtual address maps to three different physical pages in
 * the host, guest root A, and guest root B.  The guest changes CR3 without an
 * exit, then VMCALLs.  The host verifies both guest observations and its own
 * mapping immediately after the exit.  A second entry enables CR3-load
 * exiting and verifies that MOV-to-CR3 is fault-like and does not change the
 * guest CR3 field.
 */

#include <asm/desc.h>
#include <asm/msr.h>
#include <asm/msr-index.h>
#include <asm/page.h>
#include <asm/processor-flags.h>
#include <asm/special_insns.h>
#include <asm/tlbflush.h>
#include <asm/traps.h>
#include <asm/vmx.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

#define IA32_FEATURE_CONTROL 0x3a
#define IA32_VMX_BASIC 0x480
#define IA32_VMX_PINBASED_CTLS 0x481
#define IA32_VMX_PROCBASED_CTLS 0x482
#define IA32_VMX_EXIT_CTLS 0x483
#define IA32_VMX_ENTRY_CTLS 0x484
#define IA32_VMX_TRUE_PINBASED_CTLS 0x48d
#define IA32_VMX_TRUE_PROCBASED_CTLS 0x48e
#define IA32_VMX_TRUE_EXIT_CTLS 0x48f
#define IA32_VMX_TRUE_ENTRY_CTLS 0x490

#define FEATURE_CONTROL_REQUIRED ((1ULL << 0) | (1ULL << 2))
#define VMX_EXIT_REASON_MASK 0xffffU
#define VMX_EXIT_EXCEPTION_OR_NMI 0U
#define VMX_EXIT_VMCALL 18U
#define VMX_EXIT_CR_ACCESS 28U
#define VMX_EXIT_ENTRY_INVALID_GUEST_STATE 33U
#define VMX_EXIT_ENTRY_FAILURE (1U << 31)
#define VMX_ERROR_RESUME_NON_LAUNCHED 5U
#define VMX_ERROR_INVALID_CONTROLS 7U

#define HOST_VALUE 0x484f53545f4d4150ULL
#define GUEST_A_VALUE 0x47554553545f4131ULL
#define GUEST_B_VALUE 0x47554553545f4232ULL
#define GUEST_RESUMED_VALUE 0x524553554d45445fULL
#define GUEST_STACK_VALUE 0x535441434b5f4f4bULL

enum vmx_status {
	VMX_SUCCESS = 0,
	VMX_FAIL_INVALID = 1,
	VMX_FAIL_VALID = 2,
};

struct guest_root {
	void *tables[4];
	unsigned int nr_tables;
	u64 cr3;
};

/* Symbols used by the position-independent guest assembly below. */
u64 vmx_test_target_va;
u64 vmx_test_guest_cr3_b;
u64 vmx_test_fault_va = PAGE_SIZE;
u64 vmx_test_seen_a;
u64 vmx_test_seen_b;
u64 vmx_test_seen_cr3;
u64 vmx_test_seen_cr0;
u64 vmx_test_resumed;
u64 vmx_test_stack_seen;

extern void vmx_transition_guest(void);
extern void vmx_transition_guest_vmcall1(void);
extern void vmx_transition_guest_vmcall2(void);
extern void vmx_transition_cr3_guest(void);
extern void vmx_transition_cr3_instruction(void);
extern void vmx_transition_cr0_guest(void);
extern void vmx_transition_cr0_vmcall(void);
extern void vmx_transition_clts_exit_guest(void);
extern void vmx_transition_pf_guest(void);
extern void vmx_transition_pf_instruction(void);

asm(
".pushsection .text\n"
".global vmx_transition_guest\n"
".global vmx_transition_guest_vmcall1\n"
".global vmx_transition_guest_vmcall2\n"
"vmx_transition_guest:\n"
"    mov vmx_test_target_va(%rip), %rax\n"
"    mov (%rax), %rax\n"
"    mov %rax, vmx_test_seen_a(%rip)\n"
"    mov vmx_test_guest_cr3_b(%rip), %rax\n"
"    mov %rax, %cr3\n"
"    mov vmx_test_target_va(%rip), %rax\n"
"    mov (%rax), %rax\n"
"    mov %rax, vmx_test_seen_b(%rip)\n"
"    mov %cr3, %rax\n"
"    mov %rax, vmx_test_seen_cr3(%rip)\n"
"    movabs $0x535441434b5f4f4b, %rax\n"
"    push %rax\n"
"    pop %rax\n"
"    mov %rax, vmx_test_stack_seen(%rip)\n"
"vmx_transition_guest_vmcall1:\n"
"    vmcall\n"
"    movabs $0x524553554d45445f, %rax\n"
"    mov %rax, vmx_test_resumed(%rip)\n"
"vmx_transition_guest_vmcall2:\n"
"    vmcall\n"
"    ud2\n"
".global vmx_transition_cr0_guest\n"
".global vmx_transition_cr0_vmcall\n"
"vmx_transition_cr0_guest:\n"
"    clts\n"
"    mov %cr0, %rax\n"
"    mov %rax, vmx_test_seen_cr0(%rip)\n"
"vmx_transition_cr0_vmcall:\n"
"    vmcall\n"
"    ud2\n"
".global vmx_transition_clts_exit_guest\n"
"vmx_transition_clts_exit_guest:\n"
"    clts\n"
"    ud2\n"
".global vmx_transition_cr3_guest\n"
".global vmx_transition_cr3_instruction\n"
"vmx_transition_cr3_guest:\n"
"    mov vmx_test_guest_cr3_b(%rip), %rax\n"
"vmx_transition_cr3_instruction:\n"
"    mov %rax, %cr3\n"
"    vmcall\n"
"    ud2\n"
".global vmx_transition_pf_guest\n"
".global vmx_transition_pf_instruction\n"
"vmx_transition_pf_guest:\n"
"    mov vmx_test_fault_va(%rip), %rax\n"
"vmx_transition_pf_instruction:\n"
"    mov (%rax), %rax\n"
"    ud2\n"
".popsection\n");

static void *vmxon_region;
static void *vmcs_region;
static void *guest_stack;
static void *host_mapping;
static struct page *guest_page_a;
static struct page *guest_page_b;
static struct guest_root root_a;
static struct guest_root root_b;
static u64 vmxon_pa;
static u64 vmcs_pa;
static u64 host_cr3;
static unsigned long saved_cr4;
static unsigned long saved_irq_flags;
static bool cpu_pinned;
static bool vmx_active;
static int exit_phase;
static int test_error;

static enum vmx_status
status_from_rflags(unsigned long flags)
{
	if (flags & X86_EFLAGS_CF)
		return VMX_FAIL_INVALID;
	if (flags & X86_EFLAGS_ZF)
		return VMX_FAIL_VALID;
	return VMX_SUCCESS;
}

static bool
status_flags_are_architectural(unsigned long flags, enum vmx_status status)
{
	const unsigned long cleared = X86_EFLAGS_PF | X86_EFLAGS_AF |
		X86_EFLAGS_SF | X86_EFLAGS_OF;

	if (flags & cleared)
		return false;
	if (status == VMX_SUCCESS)
		return !(flags & (X86_EFLAGS_CF | X86_EFLAGS_ZF));
	if (status == VMX_FAIL_INVALID)
		return (flags & X86_EFLAGS_CF) && !(flags & X86_EFLAGS_ZF);
	return !(flags & X86_EFLAGS_CF) && (flags & X86_EFLAGS_ZF);
}

static enum vmx_status
vmxon_checked(u64 pa)
{
	unsigned long flags;

	asm volatile("vmxon %[pa]; pushfq; popq %[flags]"
		     : [flags] "=r" (flags) : [pa] "m" (pa)
		     : "cc", "memory");
	return status_from_rflags(flags);
}

static enum vmx_status
vmclear_checked(u64 pa)
{
	unsigned long flags;

	asm volatile("vmclear %[pa]; pushfq; popq %[flags]"
		     : [flags] "=r" (flags) : [pa] "m" (pa)
		     : "cc", "memory");
	return status_from_rflags(flags);
}

static enum vmx_status
vmptrld_checked(u64 pa)
{
	unsigned long flags;

	asm volatile("vmptrld %[pa]; pushfq; popq %[flags]"
		     : [flags] "=r" (flags) : [pa] "m" (pa)
		     : "cc", "memory");
	return status_from_rflags(flags);
}

static enum vmx_status
vmwrite_checked(unsigned long field, unsigned long value)
{
	unsigned long flags;

	asm volatile("vmwrite %[value], %[field]; pushfq; popq %[flags]"
		     : [flags] "=r" (flags)
		     : [field] "r" (field), [value] "r" (value)
		     : "cc", "memory");
	return status_from_rflags(flags);
}

static enum vmx_status
vmread_checked(unsigned long field, unsigned long *value)
{
	unsigned long flags;
	unsigned long result;

	asm volatile("vmread %[field], %[result]; pushfq; popq %[flags]"
		     : [result] "=r" (result), [flags] "=r" (flags)
		     : [field] "r" (field) : "cc", "memory");
	*value = result;
	return status_from_rflags(flags);
}

static enum vmx_status
vmlaunch_checked(unsigned long *rflags)
{
	unsigned long flags;

	asm volatile("vmlaunch; pushfq; popq %0"
		     : "=r" (flags) :
		     : "rax", "rbx", "rcx", "rdx", "cc", "memory");
	*rflags = flags;
	return status_from_rflags(flags);
}

static enum vmx_status
vmresume_checked(unsigned long *rflags)
{
	unsigned long flags;

	asm volatile("vmresume; pushfq; popq %0"
		     : "=r" (flags) :
		     : "rax", "rbx", "rcx", "rdx", "cc", "memory");
	*rflags = flags;
	return status_from_rflags(flags);
}

static enum vmx_status
vmxoff_checked(void)
{
	unsigned long flags;

	asm volatile("vmxoff; pushfq; popq %0"
		     : "=r" (flags) : : "cc", "memory");
	return status_from_rflags(flags);
}

static u32
adjust_controls(u32 wanted, u32 msr)
{
	u64 capability = native_read_msr(msr);
	u32 required = (u32)capability;
	u32 allowed = (u32)(capability >> 32);

	return (wanted | required) & allowed;
}

static u16
read_cs(void)
{
	u16 value;
	asm volatile("mov %%cs, %0" : "=rm" (value));
	return value;
}

static u16
read_ss(void)
{
	u16 value;
	asm volatile("mov %%ss, %0" : "=rm" (value));
	return value;
}

static u16
read_ds(void)
{
	u16 value;
	asm volatile("mov %%ds, %0" : "=rm" (value));
	return value;
}

static u16
read_es(void)
{
	u16 value;
	asm volatile("mov %%es, %0" : "=rm" (value));
	return value;
}

static u16
read_fs(void)
{
	u16 value;
	asm volatile("mov %%fs, %0" : "=rm" (value));
	return value;
}

static u16
read_gs(void)
{
	u16 value;
	asm volatile("mov %%gs, %0" : "=rm" (value));
	return value;
}

static u16
read_ldtr(void)
{
	u16 value;
	asm volatile("sldt %0" : "=rm" (value));
	return value;
}

static u32
segment_access_rights(const struct desc_ptr *gdt, u16 selector)
{
	u64 descriptor;

	if (!(selector & ~7U))
		return 1U << 16;
	/* Read the hidden-state source directly.  This avoids depending on LAR,
	 * which is unrelated to the VMX behavior under test and is not implemented
	 * by the baseline gem5 x86 ISA.  LDTR and TR descriptors use the same
	 * access-rights positions in their first eight bytes. */
	descriptor = *(const u64 *)(gdt->address + (selector & ~7U));
	return (u32)((descriptor >> 40) & 0xf0ffU);
}

static u32
segment_limit(const struct desc_ptr *gdt, u16 selector)
{
	u64 descriptor;
	u32 limit;

	if (!(selector & ~7U))
		return 0;
	descriptor = *(const u64 *)(gdt->address + (selector & ~7U));
	limit = (descriptor & 0xffffU) | ((descriptor >> 32) & 0xf0000U);
	if (descriptor & (1ULL << 55))
		limit = (limit << 12) | 0xfffU;
	return limit;
}

static u64
tss_base(const struct desc_ptr *gdt, u16 selector)
{
	const struct ldttss_desc *desc =
		(const struct ldttss_desc *)(gdt->address + (selector & ~7U));

	return desc->base0 | ((u64)desc->base1 << 16) |
		((u64)desc->base2 << 24) | ((u64)desc->base3 << 32);
}

static int
clone_root_with_mapping(struct guest_root *root, unsigned long address,
		struct page *mapped_page)
{
	static const unsigned int shifts[] = { 39, 30, 21 };
	u64 *source;
	u64 *dest;
	u64 entry;
	unsigned int i;

	memset(root, 0, sizeof(*root));
	root->tables[0] = (void *)get_zeroed_page(GFP_KERNEL);
	if (!root->tables[0])
		return -ENOMEM;
	root->nr_tables = 1;
	source = __va(host_cr3 & PTE_PFN_MASK);
	dest = root->tables[0];
	memcpy(dest, source, PAGE_SIZE);

	for (i = 0; i < ARRAY_SIZE(shifts); ++i) {
		unsigned int index = (address >> shifts[i]) & 0x1ff;
		void *child;

		entry = source[index];
		if (!(entry & _PAGE_PRESENT) || (entry & _PAGE_PSE))
			return -EINVAL;
		child = (void *)get_zeroed_page(GFP_KERNEL);
		if (!child)
			return -ENOMEM;
		root->tables[root->nr_tables++] = child;
		source = __va(entry & PTE_PFN_MASK);
		memcpy(child, source, PAGE_SIZE);
		dest[index] = (entry & ~PTE_PFN_MASK) | virt_to_phys(child);
		dest = child;
	}

	entry = source[(address >> PAGE_SHIFT) & 0x1ff];
	if (!(entry & _PAGE_PRESENT))
		return -EINVAL;
	/* CR3 writes preserve global translations, so the deliberately
	 * root-dependent mapping must not be global. */
	dest[(address >> PAGE_SHIFT) & 0x1ff] =
		(page_to_phys(mapped_page) & PTE_PFN_MASK) |
		((entry & ~PTE_PFN_MASK) & ~_PAGE_GLOBAL);
	root->cr3 = virt_to_phys(root->tables[0]);
	return 0;
}

static void
free_guest_root(struct guest_root *root)
{
	while (root->nr_tables)
		free_page((unsigned long)root->tables[--root->nr_tables]);
	root->cr3 = 0;
}

#define WRITE_FIELD(field, value) do {                                      \
	if (vmwrite_checked((field), (unsigned long)(value)) != VMX_SUCCESS) { \
		pr_err("vmx_transition: VMWRITE %s failed\n", #field);       \
		return -EIO;                                                  \
	}                                                                      \
} while (0)

static int
configure_vmcs(unsigned long host_rsp, unsigned long host_rip)
{
	struct desc_ptr gdt;
	struct desc_ptr idt;
	u16 cs = read_cs();
	u16 ss = read_ss();
	u16 ds = read_ds();
	u16 es = read_es();
	u16 fs = read_fs();
	u16 gs = read_gs();
	u16 ldtr = read_ldtr();
	u16 tr;
	u64 efer;
	u64 fs_base;
	u64 gs_base;
	u64 sysenter_cs;
	u64 sysenter_esp;
	u64 sysenter_eip;
	u32 pin_controls = adjust_controls(0, IA32_VMX_TRUE_PINBASED_CTLS);
	u32 proc_controls = adjust_controls(0, IA32_VMX_TRUE_PROCBASED_CTLS);
	u32 exit_controls = adjust_controls(VM_EXIT_HOST_ADDR_SPACE_SIZE |
		VM_EXIT_SAVE_IA32_EFER | VM_EXIT_LOAD_IA32_EFER,
		IA32_VMX_TRUE_EXIT_CTLS);
	u32 entry_controls = adjust_controls(VM_ENTRY_IA32E_MODE |
		VM_ENTRY_LOAD_IA32_EFER, IA32_VMX_TRUE_ENTRY_CTLS);

	native_store_gdt(&gdt);
	store_idt(&idt);
	store_tr(tr);
	rdmsrl(MSR_EFER, efer);
	rdmsrl(MSR_FS_BASE, fs_base);
	rdmsrl(MSR_GS_BASE, gs_base);
	rdmsrl(MSR_IA32_SYSENTER_CS, sysenter_cs);
	rdmsrl(MSR_IA32_SYSENTER_ESP, sysenter_esp);
	rdmsrl(MSR_IA32_SYSENTER_EIP, sysenter_eip);

	WRITE_FIELD(PIN_BASED_VM_EXEC_CONTROL, pin_controls);
	WRITE_FIELD(CPU_BASED_VM_EXEC_CONTROL, proc_controls);
	WRITE_FIELD(EXCEPTION_BITMAP, 1U << X86_TRAP_PF);
	WRITE_FIELD(PAGE_FAULT_ERROR_CODE_MASK, 0);
	WRITE_FIELD(PAGE_FAULT_ERROR_CODE_MATCH, 0);
	WRITE_FIELD(CR3_TARGET_COUNT, 0);
	WRITE_FIELD(VM_EXIT_CONTROLS, exit_controls);
	WRITE_FIELD(VM_EXIT_MSR_STORE_COUNT, 0);
	WRITE_FIELD(VM_EXIT_MSR_LOAD_COUNT, 0);
	WRITE_FIELD(VM_ENTRY_CONTROLS, entry_controls);
	WRITE_FIELD(VM_ENTRY_MSR_LOAD_COUNT, 0);
	WRITE_FIELD(VM_ENTRY_INTR_INFO_FIELD, 0);
	WRITE_FIELD(CR0_GUEST_HOST_MASK, 0);
	WRITE_FIELD(CR4_GUEST_HOST_MASK, 0);
	WRITE_FIELD(CR0_READ_SHADOW, read_cr0());
	WRITE_FIELD(CR4_READ_SHADOW, __read_cr4());

	WRITE_FIELD(GUEST_CR0, read_cr0());
	WRITE_FIELD(GUEST_CR3, root_a.cr3);
	WRITE_FIELD(GUEST_CR4, __read_cr4());
	WRITE_FIELD(GUEST_DR7, 0x400);
	WRITE_FIELD(GUEST_RSP, (unsigned long)guest_stack + PAGE_SIZE - 16);
	WRITE_FIELD(GUEST_RIP, (unsigned long)vmx_transition_guest);
	WRITE_FIELD(GUEST_RFLAGS, X86_EFLAGS_FIXED);
	WRITE_FIELD(GUEST_IA32_EFER, efer);
	WRITE_FIELD(GUEST_ACTIVITY_STATE, 0);
	WRITE_FIELD(GUEST_INTERRUPTIBILITY_INFO, 0);
	WRITE_FIELD(VMCS_LINK_POINTER, ~0ULL);

	WRITE_FIELD(GUEST_CS_SELECTOR, cs);
	WRITE_FIELD(GUEST_SS_SELECTOR, ss);
	WRITE_FIELD(GUEST_DS_SELECTOR, ds);
	WRITE_FIELD(GUEST_ES_SELECTOR, es);
	WRITE_FIELD(GUEST_FS_SELECTOR, fs);
	WRITE_FIELD(GUEST_GS_SELECTOR, gs);
	WRITE_FIELD(GUEST_LDTR_SELECTOR, ldtr);
	WRITE_FIELD(GUEST_TR_SELECTOR, tr);
	WRITE_FIELD(GUEST_CS_BASE, 0);
	WRITE_FIELD(GUEST_SS_BASE, 0);
	WRITE_FIELD(GUEST_DS_BASE, 0);
	WRITE_FIELD(GUEST_ES_BASE, 0);
	WRITE_FIELD(GUEST_FS_BASE, fs_base);
	WRITE_FIELD(GUEST_GS_BASE, gs_base);
	WRITE_FIELD(GUEST_LDTR_BASE, 0);
	WRITE_FIELD(GUEST_TR_BASE, tss_base(&gdt, tr));
	WRITE_FIELD(GUEST_CS_LIMIT, segment_limit(&gdt, cs));
	WRITE_FIELD(GUEST_SS_LIMIT, segment_limit(&gdt, ss));
	WRITE_FIELD(GUEST_DS_LIMIT, segment_limit(&gdt, ds));
	WRITE_FIELD(GUEST_ES_LIMIT, segment_limit(&gdt, es));
	WRITE_FIELD(GUEST_FS_LIMIT, segment_limit(&gdt, fs));
	WRITE_FIELD(GUEST_GS_LIMIT, segment_limit(&gdt, gs));
	WRITE_FIELD(GUEST_LDTR_LIMIT, segment_limit(&gdt, ldtr));
	WRITE_FIELD(GUEST_TR_LIMIT, segment_limit(&gdt, tr));
	WRITE_FIELD(GUEST_CS_AR_BYTES, segment_access_rights(&gdt, cs));
	WRITE_FIELD(GUEST_SS_AR_BYTES, segment_access_rights(&gdt, ss));
	WRITE_FIELD(GUEST_DS_AR_BYTES, segment_access_rights(&gdt, ds));
	WRITE_FIELD(GUEST_ES_AR_BYTES, segment_access_rights(&gdt, es));
	WRITE_FIELD(GUEST_FS_AR_BYTES, segment_access_rights(&gdt, fs));
	WRITE_FIELD(GUEST_GS_AR_BYTES, segment_access_rights(&gdt, gs));
	WRITE_FIELD(GUEST_LDTR_AR_BYTES, segment_access_rights(&gdt, ldtr));
	WRITE_FIELD(GUEST_TR_AR_BYTES, segment_access_rights(&gdt, tr));
	WRITE_FIELD(GUEST_GDTR_BASE, gdt.address);
	WRITE_FIELD(GUEST_GDTR_LIMIT, gdt.size);
	WRITE_FIELD(GUEST_IDTR_BASE, idt.address);
	WRITE_FIELD(GUEST_IDTR_LIMIT, idt.size);
	WRITE_FIELD(GUEST_SYSENTER_CS, sysenter_cs);
	WRITE_FIELD(GUEST_SYSENTER_ESP, sysenter_esp);
	WRITE_FIELD(GUEST_SYSENTER_EIP, sysenter_eip);

	WRITE_FIELD(HOST_CR0, read_cr0());
	WRITE_FIELD(HOST_CR3, host_cr3);
	WRITE_FIELD(HOST_CR4, __read_cr4());
	WRITE_FIELD(HOST_RSP, host_rsp);
	WRITE_FIELD(HOST_RIP, host_rip);
	WRITE_FIELD(HOST_IA32_EFER, efer);
	WRITE_FIELD(HOST_CS_SELECTOR, cs & ~7U);
	WRITE_FIELD(HOST_SS_SELECTOR, ss & ~7U);
	WRITE_FIELD(HOST_DS_SELECTOR, ds & ~7U);
	WRITE_FIELD(HOST_ES_SELECTOR, es & ~7U);
	WRITE_FIELD(HOST_FS_SELECTOR, fs & ~7U);
	WRITE_FIELD(HOST_GS_SELECTOR, gs & ~7U);
	WRITE_FIELD(HOST_TR_SELECTOR, tr & ~7U);
	WRITE_FIELD(HOST_FS_BASE, fs_base);
	WRITE_FIELD(HOST_GS_BASE, gs_base);
	WRITE_FIELD(HOST_TR_BASE, tss_base(&gdt, tr));
	WRITE_FIELD(HOST_GDTR_BASE, gdt.address);
	WRITE_FIELD(HOST_IDTR_BASE, idt.address);
	WRITE_FIELD(HOST_IA32_SYSENTER_CS, sysenter_cs);
	WRITE_FIELD(HOST_IA32_SYSENTER_ESP, sysenter_esp);
	WRITE_FIELD(HOST_IA32_SYSENTER_EIP, sysenter_eip);
	return 0;
}

static int
read_field(unsigned long field, unsigned long *value)
{
	return vmread_checked(field, value) == VMX_SUCCESS ? 0 : -EIO;
}

static int
verify_vmcall_exit(unsigned long expected_rip, bool first)
{
	unsigned long reason;
	unsigned long length;
	unsigned long guest_rip;
	unsigned long guest_rsp;
	unsigned long guest_cr3;

	if (read_field(VM_EXIT_REASON, &reason) ||
	    read_field(VM_EXIT_INSTRUCTION_LEN, &length) ||
	    read_field(GUEST_RIP, &guest_rip) ||
	    read_field(GUEST_RSP, &guest_rsp) ||
	    read_field(GUEST_CR3, &guest_cr3))
		return -EIO;
	if ((reason & VMX_EXIT_REASON_MASK) != VMX_EXIT_VMCALL || length != 3 ||
	    guest_rip != expected_rip || guest_cr3 != root_b.cr3 ||
	    guest_rsp != (unsigned long)guest_stack + PAGE_SIZE - 16)
		return -EINVAL;
	if (__read_cr3() != host_cr3 || *(volatile u64 *)host_mapping != HOST_VALUE)
		return -EFAULT;
	if (first && (vmx_test_seen_a != GUEST_A_VALUE ||
		      vmx_test_seen_b != GUEST_B_VALUE ||
		      vmx_test_seen_cr3 != root_b.cr3 ||
		      vmx_test_stack_seen != GUEST_STACK_VALUE))
		return -EBADE;
	return 0;
}

static void
release_test_state(void)
{
	if (vmx_active) {
		if (vmxoff_checked() != VMX_SUCCESS)
			pr_err("vmx_transition: VMXOFF failed during cleanup\n");
		vmx_active = false;
	}
	if (cpu_pinned) {
		if (!(saved_cr4 & X86_CR4_VMXE))
			cr4_clear_bits_irqsoff(X86_CR4_VMXE);
		invalidate_tss_limit();
		local_irq_restore(saved_irq_flags);
		put_cpu();
		cpu_pinned = false;
	}
	free_guest_root(&root_b);
	free_guest_root(&root_a);
	if (guest_page_b) {
		__free_page(guest_page_b);
		guest_page_b = NULL;
	}
	if (guest_page_a) {
		__free_page(guest_page_a);
		guest_page_a = NULL;
	}
	if (host_mapping) {
		vfree(host_mapping);
		host_mapping = NULL;
	}
	if (guest_stack) {
		free_page((unsigned long)guest_stack);
		guest_stack = NULL;
	}
	if (vmcs_region) {
		free_page((unsigned long)vmcs_region);
		vmcs_region = NULL;
	}
	if (vmxon_region) {
		free_page((unsigned long)vmxon_region);
		vmxon_region = NULL;
	}
}

static int __init
vmx_transition_init(void)
{
	u64 basic;
	u32 revision;
	unsigned long host_rsp;
	unsigned long flags = 0;
	unsigned long reason;
	unsigned long qualification;
	unsigned long guest_cr0;
	unsigned long guest_cr3;
	unsigned long guest_rip;
	unsigned long instruction_error;
	unsigned long interruption_info;
	unsigned long interruption_error;
	unsigned long host_cr2_before;
	unsigned long host_cr2_after;
	enum vmx_status status;
	int ret;

	pr_info("vmx_transition: START\n");
	if ((native_read_msr(IA32_FEATURE_CONTROL) & FEATURE_CONTROL_REQUIRED) !=
	    FEATURE_CONTROL_REQUIRED)
		return -EOPNOTSUPP;

	vmxon_region = (void *)get_zeroed_page(GFP_KERNEL);
	vmcs_region = (void *)get_zeroed_page(GFP_KERNEL);
	guest_stack = (void *)get_zeroed_page(GFP_KERNEL);
	host_mapping = vmalloc(PAGE_SIZE);
	guest_page_a = alloc_page(GFP_KERNEL | __GFP_ZERO);
	guest_page_b = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!vmxon_region || !vmcs_region || !guest_stack || !host_mapping ||
	    !guest_page_a || !guest_page_b) {
		ret = -ENOMEM;
		goto fail;
	}
	*(u64 *)host_mapping = HOST_VALUE;
	*(u64 *)page_address(guest_page_a) = GUEST_A_VALUE;
	*(u64 *)page_address(guest_page_b) = GUEST_B_VALUE;
	vmx_test_target_va = (unsigned long)host_mapping;

	get_cpu();
	local_irq_save(saved_irq_flags);
	cpu_pinned = true;
	host_cr3 = __read_cr3();
	saved_cr4 = __read_cr4();
	ret = clone_root_with_mapping(&root_a, (unsigned long)host_mapping,
		guest_page_a);
	if (ret)
		goto fail;
	ret = clone_root_with_mapping(&root_b, (unsigned long)host_mapping,
		guest_page_b);
	if (ret)
		goto fail;
	vmx_test_guest_cr3_b = root_b.cr3;

	basic = native_read_msr(IA32_VMX_BASIC);
	revision = (u32)basic & 0x7fffffffU;
	*(u32 *)vmxon_region = revision;
	*(u32 *)vmcs_region = revision;
	vmxon_pa = virt_to_phys(vmxon_region);
	vmcs_pa = virt_to_phys(vmcs_region);
	cr4_set_bits_irqsoff(X86_CR4_VMXE);

	status = vmxon_checked(vmxon_pa);
	if (status != VMX_SUCCESS) {
		ret = -EIO;
		goto fail;
	}
	vmx_active = true;
	if (vmclear_checked(vmcs_pa) != VMX_SUCCESS ||
	    vmptrld_checked(vmcs_pa) != VMX_SUCCESS) {
		ret = -EIO;
		goto fail;
	}

	asm volatile("mov %%rsp, %0" : "=r" (host_rsp));
	ret = configure_vmcs(host_rsp, (unsigned long)&&vmexit_handler);
	if (ret)
		goto fail;
	exit_phase = 1;
	/* VM exits create architectural control-flow edges invisible to C.  The
	 * empty asm-goto edges make each externally entered label and its live
	 * locals explicit to the optimizer without executing a software branch. */
	asm goto("" : : : "memory" : vmexit_handler);
	status = vmlaunch_checked(&flags);
	pr_err("vmx_transition: VMLAUNCH returned status=%u flags=%#lx\n",
	       status, flags);
	ret = -EIO;
	goto fail;

vmexit_handler:
	/* VM exits do not restore general registers; force the compiler to
	 * discard assumptions across the architectural transition. */
	asm volatile("" : : : "memory");
	if (exit_phase == 1) {
		ret = verify_vmcall_exit(
			(unsigned long)vmx_transition_guest_vmcall1, true);
		if (ret)
			goto fail;
		if (read_field(GUEST_RIP, &guest_rip)) {
			ret = -EIO;
			goto fail;
		}
		if (vmwrite_checked(GUEST_RIP, guest_rip + 3) != VMX_SUCCESS) {
			ret = -EIO;
			goto fail;
		}
		exit_phase = 2;
		status = vmresume_checked(&flags);
		pr_err("vmx_transition: first VMRESUME returned status=%u flags=%#lx\n",
		       status, flags);
		ret = -EIO;
		goto fail;
	}
	if (exit_phase == 2) {
		ret = verify_vmcall_exit(
			(unsigned long)vmx_transition_guest_vmcall2, false);
		if (ret || vmx_test_resumed != GUEST_RESUMED_VALUE) {
			if (!ret)
				ret = -EINVAL;
			goto fail;
		}
		/* With CR0.TS owned by the VMM and a clear read shadow, CLTS
		 * neither exits nor changes the live bit; MOV-from-CR0 observes the
		 * shadow value (SDM 28.1.3). */
		if (vmwrite_checked(GUEST_RIP,
				(unsigned long)vmx_transition_cr0_guest) != VMX_SUCCESS ||
		    vmwrite_checked(GUEST_CR3, root_a.cr3) != VMX_SUCCESS ||
		    vmwrite_checked(GUEST_CR0, read_cr0() | (1UL << 3)) !=
			    VMX_SUCCESS ||
		    vmwrite_checked(CR0_GUEST_HOST_MASK, 1UL << 3) != VMX_SUCCESS ||
		    vmwrite_checked(CR0_READ_SHADOW, read_cr0() & ~(1UL << 3)) !=
			    VMX_SUCCESS) {
			ret = -EIO;
			goto fail;
		}
		exit_phase = 3;
		status = vmresume_checked(&flags);
		pr_err("vmx_transition: CR0-mask VMRESUME returned status=%u flags=%#lx\n",
		       status, flags);
		ret = -EIO;
		goto fail;
	}
	if (exit_phase == 3) {
		if (read_field(VM_EXIT_REASON, &reason) ||
		    read_field(GUEST_RIP, &guest_rip) ||
		    read_field(GUEST_CR0, &guest_cr0)) {
			ret = -EIO;
			goto fail;
		}
		if ((reason & VMX_EXIT_REASON_MASK) != VMX_EXIT_VMCALL ||
		    guest_rip != (unsigned long)vmx_transition_cr0_vmcall ||
		    !(guest_cr0 & (1UL << 3)) || (vmx_test_seen_cr0 & (1UL << 3)) ||
		    __read_cr3() != host_cr3 ||
		    *(volatile u64 *)host_mapping != HOST_VALUE) {
			ret = -EINVAL;
			goto fail;
		}

		/* With the shadow TS bit set, CLTS is intercepted before changing
		 * guest state. Access type 2 occupies qualification bits 5:4. */
		if (vmwrite_checked(GUEST_RIP,
				(unsigned long)vmx_transition_clts_exit_guest) !=
				VMX_SUCCESS ||
		    vmwrite_checked(GUEST_CR0, guest_cr0 | (1UL << 3)) !=
			    VMX_SUCCESS ||
		    vmwrite_checked(CR0_READ_SHADOW, read_cr0() | (1UL << 3)) !=
			    VMX_SUCCESS) {
			ret = -EIO;
			goto fail;
		}
		exit_phase = 4;
		status = vmresume_checked(&flags);
		pr_err("vmx_transition: CLTS-exit VMRESUME returned status=%u flags=%#lx\n",
		       status, flags);
		ret = -EIO;
		goto fail;
	}
	if (exit_phase == 4) {
		if (read_field(VM_EXIT_REASON, &reason) ||
		    read_field(EXIT_QUALIFICATION, &qualification) ||
		    read_field(GUEST_RIP, &guest_rip) ||
		    read_field(GUEST_CR0, &guest_cr0)) {
			ret = -EIO;
			goto fail;
		}
		if ((reason & VMX_EXIT_REASON_MASK) != VMX_EXIT_CR_ACCESS ||
		    qualification != 0x20 ||
		    guest_rip != (unsigned long)vmx_transition_clts_exit_guest ||
		    !(guest_cr0 & (1UL << 3))) {
			ret = -EINVAL;
			goto fail;
		}
		if (vmwrite_checked(GUEST_RIP,
				(unsigned long)vmx_transition_cr3_guest) != VMX_SUCCESS ||
		    vmwrite_checked(GUEST_CR3, root_a.cr3) != VMX_SUCCESS ||
		    vmwrite_checked(CR0_GUEST_HOST_MASK, 0) != VMX_SUCCESS ||
		    vmwrite_checked(CR0_READ_SHADOW, read_cr0()) != VMX_SUCCESS ||
		    vmwrite_checked(CPU_BASED_VM_EXEC_CONTROL,
			    adjust_controls(CPU_BASED_CR3_LOAD_EXITING,
				    IA32_VMX_TRUE_PROCBASED_CTLS)) != VMX_SUCCESS) {
			ret = -EIO;
			goto fail;
		}
		exit_phase = 5;
		status = vmresume_checked(&flags);
		pr_err("vmx_transition: CR3-exit VMRESUME returned status=%u flags=%#lx\n",
		       status, flags);
		ret = -EIO;
		goto fail;
	}
	if (exit_phase != 5 || read_field(VM_EXIT_REASON, &reason) ||
	    read_field(EXIT_QUALIFICATION, &qualification) ||
	    read_field(GUEST_CR3, &guest_cr3) ||
	    read_field(GUEST_RIP, &guest_rip)) {
		ret = -EIO;
		goto fail;
	}
	if ((reason & VMX_EXIT_REASON_MASK) != VMX_EXIT_CR_ACCESS ||
	    qualification != 3 || guest_cr3 != root_a.cr3 ||
	    guest_rip != (unsigned long)vmx_transition_cr3_instruction ||
	    __read_cr3() != host_cr3 || *(volatile u64 *)host_mapping != HOST_VALUE) {
		ret = -EINVAL;
		goto fail;
	}

	/* A directly intercepted #PF reports the faulting linear address in
	 * exit qualification and must not update CR2 (SDM 30.2.1). */
	host_cr2_before = read_cr2();
	if (vmwrite_checked(GUEST_RIP,
			(unsigned long)vmx_transition_pf_guest) != VMX_SUCCESS ||
	    vmwrite_checked(GUEST_CR3, root_a.cr3) != VMX_SUCCESS ||
	    vmwrite_checked(HOST_RIP,
			(unsigned long)&&vmexit_pf_handler) != VMX_SUCCESS ||
	    vmwrite_checked(CPU_BASED_VM_EXEC_CONTROL,
			adjust_controls(0,
				IA32_VMX_TRUE_PROCBASED_CTLS)) != VMX_SUCCESS) {
		ret = -EIO;
		goto fail;
	}
	exit_phase = 6;
	asm goto("" : : : "memory" : vmexit_pf_handler);
	status = vmresume_checked(&flags);
	pr_err("vmx_transition: #PF VMRESUME returned status=%u flags=%#lx\n",
	       status, flags);
	ret = -EIO;
	goto fail;

vmexit_pf_handler:
	/* This phase uses a dedicated host RIP so the direct fault exit cannot
	 * fall through the preceding CR-access checks. */
	host_cr2_after = read_cr2();
	if (exit_phase != 6 || read_field(VM_EXIT_REASON, &reason) ||
	    read_field(EXIT_QUALIFICATION, &qualification) ||
	    read_field(VM_EXIT_INTR_INFO, &interruption_info) ||
	    read_field(VM_EXIT_INTR_ERROR_CODE, &interruption_error) ||
	    read_field(GUEST_RIP, &guest_rip)) {
		ret = -EIO;
		goto fail;
	}
	if ((reason & VMX_EXIT_REASON_MASK) != VMX_EXIT_EXCEPTION_OR_NMI ||
	    qualification != vmx_test_fault_va ||
	    (interruption_info & 0xff) != X86_TRAP_PF ||
	    ((interruption_info >> 8) & 7) != 3 ||
	    !(interruption_info & (1U << 11)) ||
	    !(interruption_info & (1U << 31)) || interruption_error != 0 ||
	    guest_rip != (unsigned long)vmx_transition_pf_instruction ||
	    host_cr2_after != host_cr2_before || __read_cr3() != host_cr3 ||
	    *(volatile u64 *)host_mapping != HOST_VALUE) {
		ret = -EINVAL;
		goto fail;
	}

	/* VMCLEAR removes the current VMCS. VMRESUME must first VMfailInvalid;
	 * after VMPTRLD it must VMfailValid with error 5 because it is clear. */
	if (vmclear_checked(vmcs_pa) != VMX_SUCCESS) {
		ret = -EIO;
		goto fail;
	}
	status = vmresume_checked(&flags);
	if (status != VMX_FAIL_INVALID ||
	    !status_flags_are_architectural(flags, status) ||
	    vmptrld_checked(vmcs_pa) != VMX_SUCCESS) {
		ret = -EINVAL;
		goto fail;
	}
	status = vmresume_checked(&flags);
	if (status != VMX_FAIL_VALID ||
	    !status_flags_are_architectural(flags, status) ||
	    read_field(VM_INSTRUCTION_ERROR, &instruction_error) ||
	    instruction_error != VMX_ERROR_RESUME_NON_LAUNCHED) {
		ret = -EINVAL;
		goto fail;
	}

	/* Guest-state checks occur after the VM-entry commit boundary. A late
	 * VMLAUNCH failure loads host state and changes only exit reason and
	 * qualification, but the VMCS remains clear because launch state changes
	 * only after guest-state and MSR loading succeed (SDM 29.8 and VMLAUNCH). */
	if (vmwrite_checked(PIN_BASED_VM_EXEC_CONTROL,
			adjust_controls(0,
				IA32_VMX_TRUE_PINBASED_CTLS)) != VMX_SUCCESS ||
	    vmwrite_checked(CPU_BASED_VM_EXEC_CONTROL,
			adjust_controls(0,
				IA32_VMX_TRUE_PROCBASED_CTLS)) != VMX_SUCCESS ||
	    vmwrite_checked(VM_EXIT_CONTROLS,
			adjust_controls(VM_EXIT_HOST_ADDR_SPACE_SIZE |
				VM_EXIT_SAVE_IA32_EFER | VM_EXIT_LOAD_IA32_EFER,
				IA32_VMX_TRUE_EXIT_CTLS)) != VMX_SUCCESS ||
	    vmwrite_checked(VM_ENTRY_CONTROLS,
			adjust_controls(VM_ENTRY_IA32E_MODE |
				VM_ENTRY_LOAD_IA32_EFER,
				IA32_VMX_TRUE_ENTRY_CTLS)) != VMX_SUCCESS ||
	    vmwrite_checked(GUEST_RFLAGS, 0) != VMX_SUCCESS ||
	    vmwrite_checked(HOST_RIP,
			(unsigned long)&&vmentry_failure_handler) != VMX_SUCCESS) {
		ret = -EIO;
		goto fail;
	}
	exit_phase = 7;
	asm goto("" : : : "memory" : vmentry_failure_handler);
	status = vmlaunch_checked(&flags);
	pr_err("vmx_transition: late-failure VMLAUNCH returned "
	       "status=%u flags=%#lx\n",
	       status, flags);
	ret = -EIO;
	goto fail;

vmentry_failure_handler:
	if (exit_phase != 7 || read_field(VM_EXIT_REASON, &reason) ||
	    read_field(EXIT_QUALIFICATION, &qualification) ||
	    read_field(VM_EXIT_INTR_INFO, &interruption_info) ||
	    read_field(VM_EXIT_INTR_ERROR_CODE, &interruption_error)) {
		ret = -EIO;
		goto fail;
	}
	if (reason != (VMX_EXIT_ENTRY_FAILURE |
			VMX_EXIT_ENTRY_INVALID_GUEST_STATE) || qualification != 0 ||
	    (interruption_info & 0xff) != X86_TRAP_PF ||
	    ((interruption_info >> 8) & 7) != 3 ||
	    !(interruption_info & (1U << 11)) ||
	    !(interruption_info & (1U << 31)) || interruption_error != 0 ||
	    __read_cr3() != host_cr3 ||
	    *(volatile u64 *)host_mapping != HOST_VALUE) {
		ret = -EINVAL;
		goto fail;
	}
	status = vmresume_checked(&flags);
	if (status != VMX_FAIL_VALID ||
	    !status_flags_are_architectural(flags, status) ||
	    read_field(VM_INSTRUCTION_ERROR, &instruction_error) ||
	    instruction_error != VMX_ERROR_RESUME_NON_LAUNCHED) {
		ret = -EINVAL;
		goto fail;
	}

	/* Clear the VMCS for the independent pre-commit control-validation test;
	 * VMCLEAR preserves implementation-specific fields. */
	if (vmclear_checked(vmcs_pa) != VMX_SUCCESS ||
	    vmptrld_checked(vmcs_pa) != VMX_SUCCESS ||
	    vmwrite_checked(GUEST_RFLAGS, X86_EFLAGS_FIXED) != VMX_SUCCESS) {
		ret = -EIO;
		goto fail;
	}

	/* Unsupported control bits fail before architectural state is loaded. */
	if (vmwrite_checked(PIN_BASED_VM_EXEC_CONTROL, 0) != VMX_SUCCESS ||
	    vmwrite_checked(CPU_BASED_VM_EXEC_CONTROL, 1U << 31) != VMX_SUCCESS ||
	    vmwrite_checked(VM_EXIT_CONTROLS, 0) != VMX_SUCCESS ||
	    vmwrite_checked(VM_ENTRY_CONTROLS, 0) != VMX_SUCCESS) {
		ret = -EIO;
		goto fail;
	}
	status = vmlaunch_checked(&flags);
	if (status != VMX_FAIL_VALID ||
	    !status_flags_are_architectural(flags, status) ||
	    read_field(VM_INSTRUCTION_ERROR, &instruction_error) ||
	    instruction_error != VMX_ERROR_INVALID_CONTROLS ||
	    __read_cr3() != host_cr3 || *(volatile u64 *)host_mapping != HOST_VALUE) {
		ret = -EINVAL;
		goto fail;
	}

	pr_info("vmx_transition: PASS: host/guest CR3 translations\n");
	pr_info("vmx_transition: PASS: non-exiting guest MOV-to-CR3\n");
	pr_info("vmx_transition: PASS: CR0 mask/read shadow and CLTS exit\n");
	pr_info("vmx_transition: PASS: CR3-load exit reason and qualification\n");
	pr_info("vmx_transition: PASS: direct #PF qualification and "
		"CR2 preservation\n");
	pr_info("vmx_transition: PASS: VMCALL/VMRESUME/RSP/host restoration\n");
	pr_info("vmx_transition: PASS: VMCLEAR launch lifecycle and VMfail flags\n");
	pr_info("vmx_transition: PASS: late VM-entry failure preserves "
		"exit fields and clear launch state\n");
	pr_info("vmx_transition: PASS: early VM-entry failure is atomic\n");
	pr_info("vmx_transition: PASS: COMPLETE\n");
	test_error = 0;
	release_test_state();
	return 0;

fail:
	test_error = ret;
	pr_err("vmx_transition: FAIL: phase=%d error=%d\n", exit_phase, ret);
	release_test_state();
	return ret;
}

static void __exit
vmx_transition_exit(void)
{
	release_test_state();
	if (!test_error)
		pr_info("vmx_transition: unloaded cleanly\n");
}

module_init(vmx_transition_init);
module_exit(vmx_transition_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("gem5-vmx");
MODULE_DESCRIPTION("VMX entry/exit and CR3 architectural transition test");
