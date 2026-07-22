// SPDX-License-Identifier: GPL-2.0
/*
 * Small end-to-end smoke test for gem5's VMX root-operation model.
 *
 * The module intentionally leaves VMX operation active between module load
 * and unload.  This lets the full-system harness take a gem5 checkpoint while
 * the VMXON region, current VMCS pointer, and shadow VMCS fields are live.
 */

#include <asm/io.h>
#include <asm/msr.h>
#include <asm/processor-flags.h>
#include <asm/special_insns.h>
#include <asm/tlbflush.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>

#define IA32_FEATURE_CONTROL 0x3a
#define IA32_VMX_BASIC 0x480
#define IA32_VMX_PINBASED_CTLS 0x481
#define IA32_VMX_PROCBASED_CTLS 0x482
#define IA32_VMX_EXIT_CTLS 0x483
#define IA32_VMX_ENTRY_CTLS 0x484
#define IA32_VMX_CR4_FIXED1 0x489
#define IA32_VMX_VMCS_ENUM 0x48a
#define IA32_VMX_PROCBASED_CTLS2 0x48b
#define IA32_VMX_EPT_VPID_CAP 0x48c

#define SUPPORTED_PROC_CTLS (1U << 15)
#define SUPPORTED_EXIT_CTLS ((1U << 9) | (1U << 20) | (1U << 21))
#define SUPPORTED_ENTRY_CTLS ((1U << 9) | (1U << 15))

#define CPUID_1_ECX_VMX (1U << 5)

#define VMCS_GUEST_RIP 0x681eUL
#define VMCS_VM_INSTRUCTION_ERROR 0x4400UL
#define TEST_GUEST_RIP 0x123456789abcdef0UL
#define VMX_ERROR_INCORRECT_VMCS_REVISION 11UL

enum vmx_status {
	VMX_SUCCESS = 0,
	VMX_FAIL_INVALID = 1,
	VMX_FAIL_VALID = 2,
};

static void *vmxon_region;
static void *vmcs_region;
static u64 vmxon_pa;
static u64 vmcs_pa;
static unsigned long saved_cr4;
static bool cr4_saved;
static bool vmx_active;
static bool smoke_ready;

static enum vmx_status
status_from_rflags(unsigned long flags)
{
	if (flags & X86_EFLAGS_CF)
		return VMX_FAIL_INVALID;
	if (flags & X86_EFLAGS_ZF)
		return VMX_FAIL_VALID;
	return VMX_SUCCESS;
}

static enum vmx_status
vmxon_checked(u64 pa)
{
	unsigned long flags;

	asm volatile("vmxon %[operand]; pushfq; popq %[flags]"
		     : [flags] "=r" (flags)
		     : [operand] "m" (pa)
		     : "cc", "memory");
	return status_from_rflags(flags);
}

static enum vmx_status
vmclear_checked(u64 pa)
{
	unsigned long flags;

	asm volatile("vmclear %[operand]; pushfq; popq %[flags]"
		     : [flags] "=r" (flags)
		     : [operand] "m" (pa)
		     : "cc", "memory");
	return status_from_rflags(flags);
}

static enum vmx_status
vmptrld_checked(u64 pa)
{
	unsigned long flags;

	asm volatile("vmptrld %[operand]; pushfq; popq %[flags]"
		     : [flags] "=r" (flags)
		     : [operand] "m" (pa)
		     : "cc", "memory");
	return status_from_rflags(flags);
}

static void
vmptrst_value(u64 *pa)
{
	asm volatile("vmptrst %0" : "=m" (*pa) : : "memory");
}

/* AT&T syntax: VMWRITE value, field and VMREAD field, destination. */
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

	asm volatile("vmread %[field], %[value]; pushfq; popq %[flags]"
		     : [flags] "=r" (flags), [value] "=r" (result)
		     : [field] "r" (field)
		     : "cc", "memory");
	*value = result;
	return status_from_rflags(flags);
}

static enum vmx_status
vmxoff_checked(void)
{
	unsigned long flags;

	asm volatile("vmxoff; pushfq; popq %[flags]"
		     : [flags] "=r" (flags)
		     :
		     : "cc", "memory");
	return status_from_rflags(flags);
}

static bool
cpu_reports_vmx(void)
{
	u32 eax = 1;
	u32 ebx;
	u32 ecx;
	u32 edx;

	asm volatile("cpuid"
		     : "+a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx));
	return ecx & CPUID_1_ECX_VMX;
}

static int
read_msr_safe64(u32 msr, u64 *value)
{
	u32 low;
	u32 high;
	int error = rdmsr_safe(msr, &low, &high);

	*value = low | ((u64)high << 32);
	return error;
}

static bool
capability_contract_is_honest(void)
{
	u64 pin;
	u64 proc;
	u64 exit;
	u64 entry;
	u64 cr4_fixed1;
	u64 vmcs_enum;
	u64 ignored;

	if (read_msr_safe64(IA32_VMX_PINBASED_CTLS, &pin) ||
	    read_msr_safe64(IA32_VMX_PROCBASED_CTLS, &proc) ||
	    read_msr_safe64(IA32_VMX_EXIT_CTLS, &exit) ||
	    read_msr_safe64(IA32_VMX_ENTRY_CTLS, &entry) ||
	    read_msr_safe64(IA32_VMX_CR4_FIXED1, &cr4_fixed1) ||
	    read_msr_safe64(IA32_VMX_VMCS_ENUM, &vmcs_enum))
		return false;

	/* Every allowed-one bit is an implementation promise. */
	if ((u32)pin != 0 || (u32)(pin >> 32) != 0 ||
	    (u32)proc != 0 || (u32)(proc >> 32) != SUPPORTED_PROC_CTLS ||
	    (u32)exit != (1U << 9) ||
	    (u32)(exit >> 32) != SUPPORTED_EXIT_CTLS ||
	    (u32)entry != (1U << 9) ||
	    (u32)(entry >> 32) != SUPPORTED_ENTRY_CTLS ||
	    (cr4_fixed1 & (1ULL << 17)) || vmcs_enum != 0x2a)
		return false;

	/* Secondary controls and EPT/VPID are not enumerated and therefore
	 * their capability MSRs are unavailable rather than readable as zero. */
	if (!read_msr_safe64(IA32_VMX_PROCBASED_CTLS2, &ignored) ||
	    !read_msr_safe64(IA32_VMX_EPT_VPID_CAP, &ignored))
		return false;

	/* VMX capability MSRs and locked IA32_FEATURE_CONTROL are read-only. */
	if (!wrmsrl_safe(IA32_VMX_BASIC, native_read_msr(IA32_VMX_BASIC)) ||
	    !wrmsrl_safe(IA32_FEATURE_CONTROL,
		    native_read_msr(IA32_FEATURE_CONTROL)))
		return false;

	return true;
}

static enum vmx_status
release_test_state(void)
{
	enum vmx_status vmxoff_status = VMX_SUCCESS;

	if (vmx_active) {
		vmxoff_status = vmxoff_checked();
		vmx_active = false;
	}
	if (cr4_saved) {
		if (!(saved_cr4 & X86_CR4_VMXE))
			cr4_clear_bits(X86_CR4_VMXE);
		cr4_saved = false;
	}
	if (vmcs_region) {
		free_page((unsigned long)vmcs_region);
		vmcs_region = NULL;
	}
	if (vmxon_region) {
		free_page((unsigned long)vmxon_region);
		vmxon_region = NULL;
	}
	return vmxoff_status;
}

static int __init
vmx_smoke_init(void)
{
	u64 feature_control;
	u64 vmx_basic;
	u32 revision;
	u64 current_vmcs = ~0ULL;
	unsigned long readback = 0;
	enum vmx_status status;
	int ret = -EIO;

	pr_info("vmx_smoke: START\n");

	if (!cpu_reports_vmx()) {
		pr_err("vmx_smoke: FAIL: CPUID does not report VMX\n");
		return -EOPNOTSUPP;
	}

	feature_control = native_read_msr(IA32_FEATURE_CONTROL);
	if ((feature_control & (FEATURE_CONTROL_LOCKED |
				FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX)) !=
			(FEATURE_CONTROL_LOCKED |
			 FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX)) {
		pr_err("vmx_smoke: FAIL: IA32_FEATURE_CONTROL=%#llx\n",
		       feature_control);
		return -EOPNOTSUPP;
	}

	vmx_basic = native_read_msr(IA32_VMX_BASIC);
	if (!capability_contract_is_honest()) {
		pr_err("vmx_smoke: FAIL: VMX capability contract mismatch\n");
		return -EOPNOTSUPP;
	}
	pr_info("vmx_smoke: PASS: honest capability and read-only MSR contract\n");
	revision = (u32)(vmx_basic & 0x7fffffffU);
	vmxon_region = (void *)get_zeroed_page(GFP_KERNEL);
	vmcs_region = (void *)get_zeroed_page(GFP_KERNEL);
	if (!vmxon_region || !vmcs_region) {
		ret = -ENOMEM;
		goto fail;
	}

	*(u32 *)vmxon_region = revision;
	*(u32 *)vmcs_region = revision;
	vmxon_pa = virt_to_phys(vmxon_region);
	vmcs_pa = virt_to_phys(vmcs_region);

	saved_cr4 = __read_cr4();
	cr4_saved = true;
	cr4_set_bits(X86_CR4_VMXE);

	status = vmxon_checked(vmxon_pa);
	if (status != VMX_SUCCESS) {
		pr_err("vmx_smoke: FAIL: VMXON status=%u\n", status);
		goto fail;
	}
	vmx_active = true;
	pr_info("vmx_smoke: PASS: VMXON\n");

	/* VMCLEAR does not inspect the revision identifier (SDM VMCLEAR
	 * operation); revision checking belongs to VMPTRLD. */
	*(u32 *)vmcs_region = revision ^ 1U;
	status = vmclear_checked(vmcs_pa);
	if (status != VMX_SUCCESS) {
		pr_err("vmx_smoke: FAIL: VMCLEAR status=%u\n", status);
		goto fail;
	}
	pr_info("vmx_smoke: PASS: VMCLEAR ignores revision identifier\n");
	*(u32 *)vmcs_region = revision;
	status = vmptrld_checked(vmcs_pa);
	if (status != VMX_SUCCESS) {
		pr_err("vmx_smoke: FAIL: VMPTRLD status=%u\n", status);
		goto fail;
	}
	pr_info("vmx_smoke: PASS: VMPTRLD\n");

	*(u32 *)vmcs_region = revision ^ 1U;
	status = vmptrld_checked(vmcs_pa);
	if (status != VMX_FAIL_VALID ||
	    vmread_checked(VMCS_VM_INSTRUCTION_ERROR, &readback) != VMX_SUCCESS ||
	    readback != VMX_ERROR_INCORRECT_VMCS_REVISION) {
		pr_err("vmx_smoke: FAIL: bad-revision VMPTRLD status=%u error=%#lx\n",
		       status, readback);
		goto fail;
	}
	*(u32 *)vmcs_region = revision;
	pr_info("vmx_smoke: PASS: VMPTRLD rejects bad revision with error 11\n");

	vmptrst_value(&current_vmcs);
	if (current_vmcs != vmcs_pa) {
		pr_err("vmx_smoke: FAIL: VMPTRST=%#llx expected=%#llx\n",
		       current_vmcs, vmcs_pa);
		goto fail;
	}
	pr_info("vmx_smoke: PASS: VMPTRST (current VMCS=%#llx)\n",
		current_vmcs);

	status = vmwrite_checked(VMCS_GUEST_RIP, TEST_GUEST_RIP);
	if (status != VMX_SUCCESS) {
		pr_err("vmx_smoke: FAIL: VMWRITE status=%u\n", status);
		goto fail;
	}
	pr_info("vmx_smoke: PASS: VMWRITE GUEST_RIP=%#lx\n",
		TEST_GUEST_RIP);
	status = vmread_checked(VMCS_GUEST_RIP, &readback);
	if (status != VMX_SUCCESS || readback != TEST_GUEST_RIP) {
		pr_err("vmx_smoke: FAIL: VMREAD status=%u value=%#lx expected=%#lx\n",
		       status, readback, TEST_GUEST_RIP);
		goto fail;
	}
	pr_info("vmx_smoke: PASS: VMREAD GUEST_RIP=%#lx\n", readback);

	smoke_ready = true;
	pr_info("vmx_smoke: READY: VMX active, current VMCS=%#llx, GUEST_RIP=%#lx\n",
		vmcs_pa, readback);
	return 0;

fail:
	release_test_state();
	return ret;
}

static void __exit
vmx_smoke_exit(void)
{
	u64 current_vmcs = ~0ULL;
	unsigned long readback = 0;
	enum vmx_status status = VMX_FAIL_INVALID;
	enum vmx_status vmxoff_status;
	bool pass = smoke_ready;

	if (vmx_active) {
		vmptrst_value(&current_vmcs);
		status = vmread_checked(VMCS_GUEST_RIP, &readback);
		pass = pass && current_vmcs == vmcs_pa &&
			status == VMX_SUCCESS && readback == TEST_GUEST_RIP;
		if (current_vmcs == vmcs_pa)
			pr_info("vmx_smoke: PASS: VMPTRST state verification\n");
		if (status == VMX_SUCCESS && readback == TEST_GUEST_RIP)
			pr_info("vmx_smoke: PASS: VMREAD state verification\n");
	}

	vmxoff_status = release_test_state();
	pass = pass && vmxoff_status == VMX_SUCCESS;
	if (vmxoff_status == VMX_SUCCESS)
		pr_info("vmx_smoke: PASS: VMXOFF\n");
	if (pass)
		pr_info("vmx_smoke: PASS: COMPLETE (all state checks passed)\n");
	else
		pr_err("vmx_smoke: FAIL: unload state VMCS=%#llx status=%u "
		       "GUEST_RIP=%#lx VMXOFF=%u\n",
		       current_vmcs, status, readback, vmxoff_status);
}

module_init(vmx_smoke_init);
module_exit(vmx_smoke_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("gem5-vmx");
MODULE_DESCRIPTION("Smoke and checkpoint test for gem5 VMX state");
