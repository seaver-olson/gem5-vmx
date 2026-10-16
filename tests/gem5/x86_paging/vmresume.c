// Launch/resume regressions, using actual privileged instructions.
#include <stdint.h>

static uint32_t on_region[1024] __attribute__((aligned(4096)));
static uint32_t vmcs_region[1024] __attribute__((aligned(4096)));
static uint64_t idt[512] __attribute__((aligned(16)));
static uint8_t guest_stack[4096] __attribute__((aligned(16)));
volatile uint64_t resume_ip, fault_error, fault_vector, rip_pointer,
    guest_runs;
volatile uint64_t injected_events, injected_rip, injected_flags, injected_error;
extern void ud_entry(void), gp_entry(void), fault_entry(void);
extern void guest_entry(void), guest_exit(void);
extern void injected_ud(void), injected_gp(void), injected_exit(void);
extern uint64_t enter_guest(uint64_t);
extern uint64_t read_field(uint64_t);
#define PROBE(name) extern uint64_t name(uint64_t, uint64_t, uint64_t)
PROBE(op_vmxon);
PROBE(op_vmclear);
PROBE(op_vmptrld);
PROBE(op_vmxoff);
PROBE(op_vmwrite_reg);
PROBE(op_vmresume);
PROBE(op_vmlaunch);
PROBE(op_vmcall);
PROBE(op_ud2);

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

#ifdef EPT_ENTRY_TEST
static volatile uint64_t guest_tables[3][512] __attribute__((aligned(4096)));
static volatile uint64_t ept_tables[6][512] __attribute__((aligned(4096)));
static volatile uint32_t ept_data[1024] __attribute__((aligned(4096)));
extern void ept_guest_read(void), ept_guest_write(void), ept_guest_exit(void);
#define GUEST_IP(label)                                                       \
    (0x400000ull + (uint64_t)(label) - (uint64_t)ept_guest_read)

static void
check_ept_exit(uint64_t reason, uint64_t qualification, uint64_t gpa,
               uint64_t gla, uint64_t rip)
{
    check(read_field(0x4402) == reason, 100);
    check(read_field(0x2400) == gpa, 101);
    check(read_field(0x681e) == rip, 102);
    if (reason == 48) {
        check(read_field(0x6400) == qualification, 103);
        check(read_field(0x640a) == gla, 104);
    }
}

static void
test_ept_entry(uint64_t host_cr3)
{
    field(0x4002, controls(0x48e, 1u << 31));
    field(0x401e, 2); // Only EPT; use the real advertised capabilities.
    field(0x6802, 0x1800000); // Guest CR3 is a GPA, not a host address.
    field(0x681e, GUEST_IP(ept_guest_read));
    field(0x201a, (uint64_t)ept_tables[0] | 0x9e); // Reserved EPTP bit 7.
    check((enter_guest(0) & 0x41) == 0x40, 105);
    check(read_field(0x4400) == 7 && !fault_vector, 106);

    for (unsigned ad = 0; ad < 2; ++ad) {
        // GVA 0x400000/0x401000 -> GPA 0x2000000/0x2001000 -> ELF RAM.
        // Three guest paging pages also have distinct GPAs and HPAs.
        guest_tables[0][0] = 0x1801003;
        guest_tables[1][0] = 0x1802003;
        guest_tables[2][2] = 0x2000083; // Guest 2 MiB leaf, A/D clear.
        ept_tables[ad][0] = (uint64_t)ept_tables[2] | 7;
        ept_tables[2][0] = (uint64_t)ept_tables[3] | 7;
        ept_tables[3][12] = (uint64_t)ept_tables[4] | 7;
        ept_tables[3][16] = (uint64_t)ept_tables[5] | 7;
        for (unsigned i = 0; i < 3; ++i) {
            ept_tables[4][i] = (uint64_t)guest_tables[i] | 0x37;
        }
        ept_tables[5][0] = (uint64_t)ept_guest_read | 0x33; // No execute.
        ept_tables[5][1] = (uint64_t)ept_data | 0x30;       // No access.
        ept_data[0] = 0;
        ept_data[1] = 0xfeedface;
        field(0x201a, (uint64_t)ept_tables[ad] | 0x1e | (ad << 6));
        field(0x681e, GUEST_IP(ept_guest_read));
        check(enter_guest(ad != 0) == 0, 107);
        check_ept_exit(48, 0x19c, 0x2000000, 0x400000, 0x400000);

        ept_tables[5][0] = (uint64_t)ept_guest_read | 0x37;
        check(enter_guest(1) == 0, 108); // Retry the same guest instruction.
        check_ept_exit(48, 0x181, 0x2001000, 0x401000, 0x400000);
        ept_tables[5][1] = (uint64_t)ept_data | 0x31; // Read but not write.
        check(enter_guest(1) == 0, 109);
        check_ept_exit(48, 0x18a, 0x2001000, 0x401000,
                       GUEST_IP(ept_guest_write));
        check(ept_data[0] == 0, 110); // A denied write must not reach memory.
        ept_tables[5][1] = (uint64_t)ept_data | 0x32; // Illegal write-only.
        check(enter_guest(1) == 0, 111);
        check_ept_exit(49, 0, 0x2001000, 0, GUEST_IP(ept_guest_write));
        ept_tables[5][1] = (uint64_t)ept_data | 0x37;
        check(enter_guest(1) == 0, 112);
        check(read_field(0x4402) == 18 &&
                  read_field(0x681e) == GUEST_IP(ept_guest_exit),
              113);
        check(ept_data[0] == 0x12345678 && ept_data[1] == 0xfeedface, 114);

        // Distinguish paging-structure faults (qualification bit 8 clear)
        // from the final instruction/data accesses above (bit 8 set).
        field(0x681e, GUEST_IP(ept_guest_read));
        ept_tables[4][0] = (uint64_t)guest_tables[0] | 0x30;
        check(enter_guest(1) == 0, 115);
        check_ept_exit(48, 0x80 | (ad ? 3 : 1), 0x1800000, 0x400000, 0x400000);
        guest_tables[0][0] &= ~0x20ull;
        ept_tables[4][0] = (uint64_t)guest_tables[0] | 0x31;
        check(enter_guest(1) == 0, 116);
        check_ept_exit(48, 0x88 | (ad ? 3 : 2), 0x1800000, 0x400000, 0x400000);
        check(!(guest_tables[0][0] & 0x20), 117);
        ept_tables[4][0] = (uint64_t)guest_tables[0] | 0x37;
        check(enter_guest(1) == 0 && read_field(0x4402) == 18, 118);
        check((guest_tables[0][0] & 0x20) && (guest_tables[1][0] & 0x20) &&
                  (guest_tables[2][2] & 0x60) == 0x60,
              119);
        check((ept_tables[4][0] & 0x300) == (ad ? 0x300 : 0) &&
                  (ept_tables[5][1] & 0x300) == (ad ? 0x300 : 0),
              120);
        ept_tables[ad][0] = 0; // The next entry must capture its new root.
    }
    // Disable EPT on resume; neither host nor next guest may retain it.
    field(0x4002, controls(0x48e, 0));
    field(0x6802, host_cr3);
    field(0x681e, (uint64_t)guest_entry);
    check(enter_guest(1) == 0 && guest_runs == 1, 121);
    check(read_field(0x4402) == 18, 122);
}
#endif

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
    check((op_vmlaunch(0, 0, 0) & 0x8d5) == 0x8d5 && fault_vector == 6, 61);
    fault_vector = 0;
    // Exercise single instructions without dedicated timing classes too,
    // then a macro-op fault to keep both ROM dispatch paths covered.
    check((op_vmcall(0, 0, 0) & 0x8d5) == 0x8d5 && fault_vector == 6, 66);
    fault_vector = 0;
    check((op_vmxoff(0, 0, 0) & 0x8d5) == 0x8d5 && fault_vector == 6, 67);
    fault_vector = 0;
    check((op_ud2(0, 0, 0) & 0x8d5) == 0x8d5 && fault_vector == 6, 68);
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
    check((op_vmlaunch(0, 0, 0) & 0x8d5) == 1 && !fault_vector, 62);
    pointer = (uint64_t)vmcs_region;
    check(!(op_vmclear((uint64_t)&pointer, 0, 0) & 0x41), 4);
    check(!(op_vmptrld((uint64_t)&pointer, 0, 0) & 0x41), 5);
    check((op_vmresume(0, 0, 0) & 0x8d5) == 0x40 && !fault_vector, 6);
    check(read_field(0x4400) == 5, 7); // VMRESUME with non-launched VMCS

    field(0x4000, controls(0x48d, 0));       // pin controls
    field(0x4002, controls(0x48e, 0));       // primary controls, no EPT
    field(0x400c, controls(0x48f, 1u << 9)); // 64-bit host
    field(0x4012, controls(0x490, 1u << 9)); // 64-bit guest
    field(0x2800, ~0ull); // VMCS link pointer
    field(0x6800, cr0);
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

#ifdef EPT_ENTRY_TEST
    test_ept_entry(cr3);
#else
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
        if (i == 0) {
            // A failed second launch must not re-enter or clear launch state;
            // the following two successful resumes verify it remains usable.
            check((op_vmlaunch(0, 0, 0) & 0x8d5) == 0x40 && !fault_vector, 63);
            check(read_field(0x4400) == 4, 64); // VMLAUNCH with launched VMCS
            check(guest_runs == 1, 65);
        }
    }

    /* A second launch and a resume must deliver their injected exceptions
     * before executing guest_entry. Exit directly from the handlers so this
     * checks entry dispatch independently of IRET emulation. */
    check(!(op_vmclear((uint64_t)&pointer, 0, 0) & 0x41), 70);
    check(!(op_vmptrld((uint64_t)&pointer, 0, 0) & 0x41), 71);
    for (unsigned i = 0; i < 2; ++i) {
        unsigned vector = i ? 13 : 6;
        uint64_t target = (uint64_t)(i ? injected_gp : injected_ud);
        idt[vector * 2] = (target & 0xffff) | (8ull << 16) | (0x8eull << 40) |
                          ((target & 0xffff0000) << 32);
        idt[vector * 2 + 1] = target >> 32;
        // Reset state changed by the preceding guest or exception delivery.
        field(0x4816, 0xa09b); // Guest CS access rights
        field(0x4818, 0xc093); // Guest SS access rights
        field(0x804, 16);      // Guest SS selector
        field(0x681c, (uint64_t)(guest_stack + sizeof(guest_stack)));
        field(0x681e, (uint64_t)guest_entry);
        field(0x6820, 0x8d7); // Status flags must survive in the saved frame.
        field(0x4016, (1u << 31) | (3u << 8) | (i ? 1u << 11 : 0) | vector);
        field(0x4018, i ? 0x28 : 0); // VM-entry exception error code
        check(enter_guest(i != 0) == 0, 72 + i);
        check(injected_events == i + 1 && guest_runs == 3, 74 + i);
        check(injected_rip == (uint64_t)guest_entry, 76 + i);
        check((injected_flags & 0x8d5) == 0x8d5, 78 + i);
        check(injected_error == (i ? 0x28 : 0), 80 + i);
        check(read_field(0x4402) == 18, 82 + i);
        check(read_field(0x681e) == (uint64_t)injected_exit, 84 + i);
        check(read_field(0x440c) == 3, 86 + i);
    }
    // Clearing injection must restore the ordinary redirect on this VMCS.
    field(0x4016, 0);
    field(0x4816, 0xa09b);
    field(0x4818, 0xc093);
    field(0x804, 16);
    field(0x681c, (uint64_t)(guest_stack + sizeof(guest_stack)));
    field(0x681e, (uint64_t)guest_entry);
    check(enter_guest(1) == 0, 88);
    check(guest_runs == 4 && injected_events == 2, 89);
    check(read_field(0x4402) == 18 &&
          read_field(0x681e) == (uint64_t)guest_exit, 90);
#endif
    check(!(op_vmxoff(0, 0, 0) & 0x41), 60);
    __asm__ volatile(".byte 0x0f, 0x04; .word 0x22" ::"D"(0ull), "S"(0ull)
                     : "memory");
}
