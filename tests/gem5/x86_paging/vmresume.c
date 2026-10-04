// Minimal no-EPT VMRESUME regression, using actual privileged instructions.
#include <stdint.h>

static uint32_t on_region[1024] __attribute__((aligned(4096)));
static uint32_t vmcs_region[1024] __attribute__((aligned(4096)));
static uint64_t idt[512] __attribute__((aligned(16)));
static uint8_t guest_stack[4096] __attribute__((aligned(16)));
volatile uint64_t resume_ip, fault_error, fault_vector, rip_pointer,
    guest_runs;
extern void ud_entry(void), gp_entry(void), fault_entry(void);
extern void guest_entry(void), guest_exit(void);
extern uint64_t enter_guest(uint64_t);
extern uint64_t read_field(uint64_t);
#define PROBE(name) extern uint64_t name(uint64_t, uint64_t, uint64_t)
PROBE(op_vmxon);
PROBE(op_vmclear);
PROBE(op_vmptrld);
PROBE(op_vmxoff);
PROBE(op_vmwrite_reg);
PROBE(op_vmresume);

static void
check(int ok, uint64_t code)
{
    if (!ok) {
        __asm__ volatile(".byte 0x0f, 0x04; .word 0x22" ::"D"(0ull), "S"(code)
                         : "memory");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
}

static uint64_t
msr(uint32_t reg)
{
    uint32_t a, d;
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(reg));
    return a | ((uint64_t)d << 32);
}

static void
field(uint64_t encoding, uint64_t value)
{ check(!(op_vmwrite_reg(0, encoding, value) & 0x41), encoding); }

static uint32_t
controls(uint32_t reg, uint32_t wanted)
{
    uint64_t allowed = msr(reg);
    return (wanted | (uint32_t)allowed) & (allowed >> 32);
}

void
paging_test(void)
{
    // Catch #UD before VMXON; unexpected faults must fail, not time out. (i
    // need to not forget to time this outcome as well)
    for (unsigned v = 0; v < 256; ++v) {
        uint64_t target = (uint64_t)(v == 6    ? ud_entry
                                     : v == 13 ? gp_entry
                                               : fault_entry);
        idt[v * 2] = (target & 0xffff) | (8ull << 16) | (0x8eull << 40) |
                     ((target & 0xffff0000) << 32);
        idt[v * 2 + 1] = target >> 32;
    }
    struct __attribute__((packed))
    {
        uint16_t limit;
        uint64_t base;
    } idtr = {sizeof(idt) - 1, (uint64_t)idt}, gdtr;
    __asm__ volatile("lidt %0" ::"m"(idtr) : "memory");
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    check((op_vmresume(0, 0, 0) & 0x8d5) == 0x8d5 && fault_vector == 6, 1);
    fault_vector = 0;

    uint64_t cr0, cr3, cr4;
    __asm__ volatile("mov %%cr0, %0; mov %%cr3, %1; mov %%cr4, %2"
                     : "=r"(cr0), "=r"(cr3), "=r"(cr4));
    cr0 = (cr0 | msr(0x486)) & msr(0x487);
    cr4 = (cr4 | msr(0x488) | (1ull << 13)) & msr(0x489);
    __asm__ volatile("mov %0, %%cr0; mov %1, %%cr4" ::"r"(cr0), "r"(cr4)
                     : "memory");
    on_region[0] = vmcs_region[0] = (uint32_t)msr(0x480) & 0x7fffffff;
    uint64_t pointer = (uint64_t)on_region;
    check(!(op_vmxon((uint64_t)&pointer, 0, 0) & 0x41), 2);
    check((op_vmresume(0, 0, 0) & 0x8d5) == 1 && !fault_vector, 3);
    pointer = (uint64_t)vmcs_region;
    check(!(op_vmclear((uint64_t)&pointer, 0, 0) & 0x41), 4);
    check(!(op_vmptrld((uint64_t)&pointer, 0, 0) & 0x41), 5);
    check((op_vmresume(0, 0, 0) & 0x8d5) == 0x40 && !fault_vector, 6);
    check(read_field(0x4400) == 5, 7); // VMRESUME with non-launched VMCS

    field(0x4000, controls(0x48d, 0));       // pin controls
    field(0x4002, controls(0x48e, 0));       // primary controls, no EPT
    field(0x400c, controls(0x48f, 1u << 9)); // 64-bit host
    field(0x4012, controls(0x490, 1u << 9)); // 64-bit guest
    field(0x2800, ~0ull);
    / VMCS link pointer field(0x6800, cr0);
    field(0x6802, cr3);
    field(0x6804, cr4);
    field(0x681c, (uint64_t)(guest_stack + sizeof(guest_stack)));
    field(0x681e, (uint64_t)guest_entry);
    field(0x6820, 2); /* RIP/RFLAGS */
    field(0x6c00, cr0);
    field(0x6c02, cr3);
    field(0x6c04, cr4);
    /* ES, CS, SS, DS, FS, GS, LDTR, TR. Match the disk-free boot selectors;
     * use valid accessed descriptors and an unusable null LDTR. */
    for (unsigned i = 0; i < 8; ++i) {
        field(0x800 + 2 * i, i == 1 ? 8 : i == 6 ? 0 : i == 7 ? 24 : 16);
        field(0x6806 + 2 * i, 0);
        field(0x4800 + 2 * i, i == 7 ? 0x67 : i == 6 ? 0 : 0xffffffff);
        field(0x4814 + 2 * i, i == 1   ? 0xa09b
                              : i == 6 ? 0x10000
                              : i == 7 ? 0x8b
                                       : 0xc093);
    }
    for (unsigned i = 0; i < 7; ++i) {
        field(0xc00 + 2 * i, i == 1 ? 8 : i == 6 ? 24 : 16);
    }
    field(0x6816, gdtr.base);
    field(0x4810, gdtr.limit);
    field(0x6818, idtr.base);
    field(0x4812, idtr.limit);
    field(0x6c0c, gdtr.base);
    field(0x6c0e, idtr.base);

    /* Launch once, then resume twice. Check saved RIP, exit reason and a
     * memory side effect so duplicated or skipped guest execution fails. */
    for (unsigned i = 0; i < 3; ++i) {
        field(0x681e, (uint64_t)guest_entry); // reset guest RIP so each entry
                                              // exec same VMCALL path
        check(enter_guest(i != 0) == 0, 10 + i);
        check(guest_runs == i + 1, 20 + i);
        check(read_field(0x4402) == 18, 30 + i); /* VMCALL exit */
        check(read_field(0x681e) == (uint64_t)guest_exit, 40 + i);
        check(read_field(0x440c) == 3, 50 + i); /* instruction length */
    }
    check(!(op_vmxoff(0, 0, 0) & 0x41), 60);
    __asm__ volatile(".byte 0x0f, 0x04; .word 0x22" ::"D"(0ull), "S"(0ull)
                     : "memory");
}
