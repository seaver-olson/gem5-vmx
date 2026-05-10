#!/bin/sh
set -eu

MODULE=${MODULE:-/root/vmx_shadow_vmcs_demo.ko}

echo "vmx-demo: loading ${MODULE}"
insmod "${MODULE}" || true

echo "vmx-demo: recent kernel log"
dmesg | tail -n 40

echo "vmx-demo: unloading ${MODULE}"
rmmod vmx_shadow_vmcs_demo || true

echo "vmx-demo: exiting gem5"
/sbin/m5 exit
