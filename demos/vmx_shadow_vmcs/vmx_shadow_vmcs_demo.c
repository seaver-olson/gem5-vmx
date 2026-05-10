/*
 * gem5 VMX shadow VMCS demo.
 *
 * This module demonstrates the currently implemented VMX root-operation subset:
 * VMXON, VMCLEAR, VMPTRLD, VMWRITE, and VMREAD. It intentionally stops before
 * VMLAUNCH because non-root context switching is not implemented on this branch.
 */

#include <asm/msr.h>
#include <asm/processor.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/types.h>

#define IA32_FEATURE_CONTROL 0x3a
#define IA32_VMX_BASIC 0x480

#define CR4_VMXE (1UL << 13)

#define VMCS_GUEST_RIP 0x681eULL
#define DEMO_GUEST_RIP 0x123456789abcdef0ULL

/*
 * The branch currently interprets VMREAD/VMWRITE operands opposite of Intel's
 * architectural operand order. Keep the workaround visible so the demo can run
 * on this branch without claiming VMLAUNCH-level semantics.
 */
#ifndef GEM5_VMX_BRANCH_OPERAND_ORDER
#define GEM5_VMX_BRANCH_OPERAND_ORDER 1
#endif

static void *vmxon_region;
static void *vmcs_region;
static unsigned long original_cr4;
static bool vmxon_active;

static inline unsigned char
vmx_status_from_flags(unsigned long flags)
{
	if (flags & X86_EFLAGS_CF)
		return 1;
	if (flags & X86_EFLAGS_ZF)
		return 2;
	return 0;
}

static inline unsigned char
vmxon_inst(u64 phys)
{
	unsigned long flags;

	asm volatile("vmxon %[pa]; pushfq; popq %[flags]"
		     : [flags] "=r" (flags)
		     : [pa] "m" (phys)
		     : "cc", "memory");

	return vmx_status_from_flags(flags);
}

static inline unsigned char
vmclear_inst(u64 phys)
{
	unsigned long flags;

	asm volatile("vmclear %[pa]; pushfq; popq %[flags]"
		     : [flags] "=r" (flags)
		     : [pa] "m" (phys)
		     : "cc", "memory");

	return vmx_status_from_flags(flags);
}

static inline unsigned char
vmptrld_inst(u64 phys)
{
	unsigned long flags;

	asm volatile("vmptrld %[pa]; pushfq; popq %[flags]"
		     : [flags] "=r" (flags)
		     : [pa] "m" (phys)
		     : "cc", "memory");

	return vmx_status_from_flags(flags);
}

static inline unsigned char
vmwrite_inst(u64 field, u64 value)
{
	unsigned long flags;

#if GEM5_VMX_BRANCH_OPERAND_ORDER
	asm volatile("vmwrite %[field], %[value]; pushfq; popq %[flags]"
		     : [flags] "=r" (flags)
		     : [field] "r" (field), [value] "r" (value)
		     : "cc", "memory");
#else
	asm volatile("vmwrite %[value], %[field]; pushfq; popq %[flags]"
		     : [flags] "=r" (flags)
		     : [value] "r" (value), [field] "r" (field)
		     : "cc", "memory");
#endif

	return vmx_status_from_flags(flags);
}

static inline unsigned char
vmread_inst(u64 field, u64 *value)
{
	unsigned long flags;
	u64 out = 0;

#if GEM5_VMX_BRANCH_OPERAND_ORDER
	asm volatile("vmread %[value], %[field]; pushfq; popq %[flags]"
		     : [flags] "=r" (flags), [value] "=r" (out)
		     : [field] "r" (field)
		     : "cc", "memory");
#else
	asm volatile("vmread %[field], %[value]; pushfq; popq %[flags]"
		     : [flags] "=r" (flags), [value] "=r" (out)
		     : [field] "r" (field)
		     : "cc", "memory");
#endif

	*value = out;
	return vmx_status_from_flags(flags);
}

static int
check_vmx_controls(void)
{
	u64 feature_control = native_read_msr(IA32_FEATURE_CONTROL);

	if (!(feature_control & 0x1)) {
		pr_err("vmx_shadow_vmcs_demo: IA32_FEATURE_CONTROL is unlocked\n");
		return -EOPNOTSUPP;
	}

	if (!(feature_control & (1ULL << 2))) {
		pr_err("vmx_shadow_vmcs_demo: VMXON outside SMX is disabled\n");
		return -EOPNOTSUPP;
	}

	return 0;
}

static int __init
vmx_shadow_vmcs_demo_init(void)
{
	u64 vmx_basic;
	u32 revision_id;
	u64 vmxon_pa;
	u64 vmcs_pa;
	u64 readback = 0;
	unsigned char status;
	int ret;

	pr_info("vmx_shadow_vmcs_demo: starting root-mode shadow VMCS demo\n");

	ret = check_vmx_controls();
	if (ret)
		return ret;

	vmx_basic = native_read_msr(IA32_VMX_BASIC);
	revision_id = (u32)(vmx_basic & 0x7fffffffU);

	vmxon_region = (void *)get_zeroed_page(GFP_KERNEL);
	vmcs_region = (void *)get_zeroed_page(GFP_KERNEL);
	if (!vmxon_region || !vmcs_region) {
		ret = -ENOMEM;
		goto fail;
	}

	*(u32 *)vmxon_region = revision_id;
	*(u32 *)vmcs_region = revision_id;

	vmxon_pa = virt_to_phys(vmxon_region);
	vmcs_pa = virt_to_phys(vmcs_region);

	pr_info("vmx_shadow_vmcs_demo: revision=0x%x vmxon_pa=0x%llx vmcs_pa=0x%llx\n",
		revision_id, vmxon_pa, vmcs_pa);

	original_cr4 = __read_cr4();
	__write_cr4(original_cr4 | CR4_VMXE);

	status = vmxon_inst(vmxon_pa);
	if (status) {
		pr_err("vmx_shadow_vmcs_demo: VMXON failed status=%u\n", status);
		ret = -EIO;
		goto fail_restore_cr4;
	}
	vmxon_active = true;
	pr_info("vmx_shadow_vmcs_demo: VMXON succeeded\n");

	status = vmclear_inst(vmcs_pa);
	if (status) {
		pr_err("vmx_shadow_vmcs_demo: VMCLEAR failed status=%u\n", status);
		ret = -EIO;
		goto fail_restore_cr4;
	}

	status = vmptrld_inst(vmcs_pa);
	if (status) {
		pr_err("vmx_shadow_vmcs_demo: VMPTRLD failed status=%u\n", status);
		ret = -EIO;
		goto fail_restore_cr4;
	}
	pr_info("vmx_shadow_vmcs_demo: selected shadow VMCS\n");

	status = vmwrite_inst(VMCS_GUEST_RIP, DEMO_GUEST_RIP);
	if (status) {
		pr_err("vmx_shadow_vmcs_demo: VMWRITE GUEST_RIP failed status=%u\n",
		       status);
		ret = -EIO;
		goto fail_restore_cr4;
	}

	status = vmread_inst(VMCS_GUEST_RIP, &readback);
	if (status) {
		pr_err("vmx_shadow_vmcs_demo: VMREAD GUEST_RIP failed status=%u\n",
		       status);
		ret = -EIO;
		goto fail_restore_cr4;
	}

	if (readback != DEMO_GUEST_RIP) {
		pr_err("vmx_shadow_vmcs_demo: FAIL: GUEST_RIP readback=0x%llx expected=0x%llx\n",
		       readback, DEMO_GUEST_RIP);
		ret = -EIO;
		goto fail_restore_cr4;
	}

	pr_info("vmx_shadow_vmcs_demo: PASS: GUEST_RIP readback matched 0x%llx\n",
		readback);
	pr_info("vmx_shadow_vmcs_demo: stopping before VMLAUNCH; L2 context switching is not implemented\n");

	return 0;

fail_restore_cr4:
	__write_cr4(original_cr4);
fail:
	if (vmcs_region) {
		free_page((unsigned long)vmcs_region);
		vmcs_region = NULL;
	}
	if (vmxon_region) {
		free_page((unsigned long)vmxon_region);
		vmxon_region = NULL;
	}
	return ret;
}

static void __exit
vmx_shadow_vmcs_demo_exit(void)
{
	if (vmxon_active)
		pr_info("vmx_shadow_vmcs_demo: leaving VMX active because branch VMXOFF is not wired yet\n");

	if (original_cr4)
		__write_cr4(original_cr4);

	if (vmcs_region)
		free_page((unsigned long)vmcs_region);
	if (vmxon_region)
		free_page((unsigned long)vmxon_region);

	pr_info("vmx_shadow_vmcs_demo: unloaded\n");
}

module_init(vmx_shadow_vmcs_demo_init);
module_exit(vmx_shadow_vmcs_demo_exit);

MODULE_LICENSE("Proprietary");
MODULE_AUTHOR("gem5-vmx");
MODULE_DESCRIPTION("gem5 Intel VMX shadow VMCS demo payload");
