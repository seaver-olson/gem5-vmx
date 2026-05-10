# VMX Shadow VMCS Demo Payload

This demo is scoped to the VMX root-operation subset currently implemented in
this gem5 branch. It does not execute `VMLAUNCH` or run an L2 guest. The module
demonstrates that a CPL0 payload can:

- enable `CR4.VMXE`
- execute `VMXON`
- execute `VMCLEAR`
- execute `VMPTRLD`
- write `GUEST_RIP` in the shadow VMCS
- read `GUEST_RIP` back from the shadow VMCS

The expected kernel log line is:

```text
vmx_shadow_vmcs_demo: PASS: GUEST_RIP readback matched 0x123456789abcdef0
```

## Build Inside The Guest

Copy this directory into the booted x86 Linux guest and build it against the
guest's running kernel headers:

```sh
make
sudo insmod vmx_shadow_vmcs_demo.ko
dmesg | tail -n 30
sudo rmmod vmx_shadow_vmcs_demo
```

Do not rely on this path for the conference demo unless you have verified the
guest image has `make`, `gcc`, and `/lib/modules/$(uname -r)/build`.

## Build On The Host

The reliable path is to build the module on the host against the exact Linux
`5.4.49` source/config used by the gem5 resource:

```sh
demos/vmx_shadow_vmcs/build_prebuilt_module.sh
```

That creates `demos/vmx_shadow_vmcs/vmx_shadow_vmcs_demo.ko`. When this file is
present, `configs/run_vmx_demo_fs.py` embeds it into the gem5 readfile and the
guest only needs `insmod`, `dmesg`, `rmmod`, and `m5`.

## gem5 Readfile Hook

If the module is already present in the guest filesystem, use
`guest_run_vmx_shadow_vmcs_demo.sh` as the gem5 readfile script. It loads the
module, prints the recent kernel log, and exits gem5 through `/sbin/m5 exit`.

## Kernel And Disk Resources

The demo full-system config uses gem5's tested Linux `5.4.49` kernel and Ubuntu
18.04 disk image resources. Download them into this repository with:

```sh
build/X86/gem5.opt configs/download_vmx_demo_resources.py
```

Then boot the pair with:

```sh
build/X86/gem5.opt configs/run_vmx_demo_fs.py
```

Resources are stored under `resources/`, which is intentionally ignored by git
because the disk image is large.

## Branch-Specific Operand Note

This branch currently interprets generated `VMREAD`/`VMWRITE` operands opposite
of Intel's architectural form. The inline assembly wrappers in
`vmx_shadow_vmcs_demo.c` intentionally target the branch's current behavior so
the conference demo works before non-root context switching is implemented.

Once `src/arch/x86/isa/formats/vmx.isa` is fixed to Intel operand order, set
`GEM5_VMX_BRANCH_OPERAND_ORDER` to `0`.
