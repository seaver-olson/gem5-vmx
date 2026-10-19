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
static volatile uint64_t guest_tables[4][512] __attribute__((aligned(4096)));
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

    for (unsigned variant = 0; variant < 4; ++variant) {
        const unsigned ad = variant & 1;
        const uint64_t software = 1ull << 52;
        // GVA 0x400000/0x401000 -> GPA 0x2000000/0x2001000 -> ELF RAM.
        // Three guest paging pages also have distinct GPAs and HPAs.
        guest_tables[0][0] = 0x1801203;
        guest_tables[1][0] = 0x1802203;
        guest_tables[2][2] =
            0x2000283; // Guest leaf, software bit 9, A/D clear.
        ept_tables[ad][0] = (uint64_t)ept_tables[2] | 7 | software;
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
        if (variant & 2) {
            // Establish clean resident lines without functional memory writes.
            for (unsigned i = 0; i < 3; ++i) {
                volatile uint64_t *entry = &guest_tables[i][i == 2 ? 2 : 0];
                __asm__ volatile("clflush (%0); mfence" ::"r"(entry)
                                 : "memory");
                (void)*entry;
            }
            volatile uint64_t *entries[] = {
                &ept_tables[ad][0], &ept_tables[2][0], &ept_tables[3][12],
                &ept_tables[3][16], &ept_tables[4][0], &ept_tables[5][0],
            };
            for (unsigned i = 0; i < 6; ++i) {
                __asm__ volatile("clflush (%0); mfence" ::"r"(entries[i])
                                 : "memory");
                (void)*entries[i];
            }
        }
        field(0x201a, (uint64_t)ept_tables[ad] | 0x1e | (ad << 6));
        field(0x681e, GUEST_IP(ept_guest_read));
        check(enter_guest(variant != 0) == 0, 107);
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
        check(guest_tables[0][0] == 0x1801223 &&
                  guest_tables[1][0] == 0x1802223 &&
                  guest_tables[2][2] == 0x20002e3,
              123);
        check(ept_tables[ad][0] ==
                  ((uint64_t)ept_tables[2] | 7 | software | (ad ? 0x100 : 0)),
              124);
        check(ept_tables[5][1] ==
                  ((uint64_t)ept_data | 0x37 | (ad ? 0x300 : 0)),
              125);
        ept_tables[ad][0] = 0; // The next entry must capture its new root.
    }
    uint32_t a = 0x80000008, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if ((a & 0xff) == 52) {
        // Legal guest PTEs can exceed four-level EPT's GPA range. The
        // instruction remains low while its data page is remapped high.
        ept_tables[0][0] = (uint64_t)ept_tables[2] | 7;
        ept_tables[4][3] = (uint64_t)guest_tables[3] | 0x37;
        guest_tables[2][2] = 0x1803003;
        field(0x201a, (uint64_t)ept_tables[0] | 0x1e);
        for (unsigned bit = 48; bit < 52; ++bit) {
            for (unsigned access = 0; access < 3; ++access) {
                const uint64_t high = 1ull << bit;
                guest_tables[3][0] = 0x2000003 | (access == 2 ? high : 0);
                guest_tables[3][1] = 0x2001003 | high;
                uint64_t rip =
                    GUEST_IP(access == 1 ? ept_guest_write : ept_guest_read);
                field(0x681e, rip);
                check(enter_guest(1) == 0, 126);
                check_ept_exit(48, 0x180 | (1u << access),
                               high | (access == 2 ? 0x2000000 : 0x2001000),
                               access == 2 ? 0x400000 : 0x401000, rip);
            }
        }
    }
    // Disable EPT on resume; neither host nor next guest may retain it.
    field(0x4002, controls(0x48e, 0));
    field(0x6802, host_cr3);
    field(0x681e, (uint64_t)guest_entry);
    check(enter_guest(1) == 0 && guest_runs == 1, 121);
    check(read_field(0x4402) == 18, 122);
}
#endif

#ifdef VMX_TRANSITION_TEST
// Port the Linux modules' unique checks without duplicating VMCS setup.
static volatile uint64_t roots[2][4][512] __attribute__((aligned(4096)));
static volatile uint64_t data[2][512] __attribute__((aligned(4096)));
volatile uint64_t transition_seen[6], transition_root_b;
extern void transition_guest(void), transition_exit1(void);
extern void transition_exit2(void), transition_clts(void);
extern void transition_clts_exit(void), transition_cr3(void);
extern void transition_cr3_instruction(void), transition_pf(void);
extern void transition_getsec(void), transition_xsetbv(void);
extern void transition_xsetbv_instruction(void);
extern uint64_t probe_wrmsr(uint64_t, uint64_t);
PROBE(op_vmptrst);

static void
test_vmx_capabilities(void)
{
    uint32_t a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    check((c & (1u << 5)) && (msr(0x3a) & 5) == 5, 200);
    // This is the simulator's advertised contract, not a hardware profile.
    const uint64_t expected[] = {
        0, ((1ull << 15) | (1ull << 31)) << 32,
        ((1ull << 9) | (1ull << 20) | (1ull << 21)) << 32 | (1u << 9),
        ((1ull << 9) | (1ull << 15)) << 32 | (1u << 9)};
    for (unsigned i = 0; i < 4; ++i) {
        check(msr(0x481 + i) == expected[i], 201 + i);
        check(msr(0x48d + i) == expected[i], 205 + i);
    }
    check(!(msr(0x489) & (1u << 17)) && msr(0x48a) == 0x2a, 209);
    check(msr(0x48b) == (2ull << 32), 210); // Secondary controls: EPT only.
    check(msr(0x48c) == ((1u << 6) | (1u << 14) | (1u << 21)), 211);
    // Unlike the retired no-EPT assumptions, these MSRs are now readable.
    const uint32_t readonly[] = {0x480, 0x48b, 0x48c, 0x3a};
    for (unsigned i = 0; i < sizeof(readonly) / sizeof(readonly[0]); ++i) {
        uint64_t before = msr(readonly[i]);
        fault_vector = fault_error = 0;
        probe_wrmsr(readonly[i], before);
        check(fault_vector == 13 && fault_error == 0, 212 + i);
        check(msr(readonly[i]) == before, 216 + i);
    }
    fault_vector = 0;
}

static void
transition_exit(unsigned resume, void (*rip)(void), uint64_t reason,
                uint64_t host_cr0, uint64_t host_cr3, uint64_t host_cr4)
{
    field(0x681e, (uint64_t)rip);
    check(enter_guest(resume) == 0, 220);
    check(read_field(0x4402) == reason, 221);
    uint64_t cr0, cr3, cr4;
    __asm__ volatile("mov %%cr0, %0; mov %%cr3, %1; mov %%cr4, %2"
                     : "=r"(cr0), "=r"(cr3), "=r"(cr4));
    check(cr0 == host_cr0 && cr3 == host_cr3 && cr4 == host_cr4, 222);
    check(*(volatile uint64_t *)0x400000 == 0x484f5354, 223);
}

static void
test_vmx_transition(uint64_t cr0, uint64_t cr3, uint64_t cr4, uint64_t pointer)
{
    test_vmx_capabilities();
    uint32_t revision = vmcs_region[0];
    vmcs_region[0] ^= 1;
    check(!(op_vmclear((uint64_t)&pointer, 0, 0) & 0x41), 224);
    vmcs_region[0] = revision;
    check(!(op_vmptrld((uint64_t)&pointer, 0, 0) & 0x41), 225);
    vmcs_region[0] ^= 1;
    check((op_vmptrld((uint64_t)&pointer, 0, 0) & 0x8d5) == 0x40, 226);
    check(read_field(0x4400) == 11, 227);
    vmcs_region[0] = revision;
    uint64_t current = 0;
    check(!(op_vmptrst((uint64_t)&current, 0, 0) & 0x41), 228);
    check(current == pointer, 229); // Bad revision must not lose current VMCS.

    // Deterministic VMX-root checkpoint boundary, without active EPT state.
    field(0x681e, 0x123456789abcdef0ull);
    __asm__ volatile(".byte 0x0f, 0x04; .word 0x21" ::"D"(0ull) : "memory");
    check(!(op_vmptrst((uint64_t)&current, 0, 0) & 0x41), 230);
    check(current == pointer && read_field(0x681e) == 0x123456789abcdef0ull,
          231);

    // Identity-map the code/stack below 2 MiB, but map one data GVA to two
    // different guest physical pages. The host keeps its boot mapping.
    *(volatile uint64_t *)0x400000 = 0x484f5354;
    for (unsigned i = 0; i < 2; ++i) {
        roots[i][0][0] = (uint64_t)roots[i][1] | 3;
        roots[i][1][0] = (uint64_t)roots[i][2] | 3;
        roots[i][2][0] = 0x83;
        roots[i][2][2] = (uint64_t)roots[i][3] | 3;
        roots[i][3][0] = (uint64_t)data[i] | 3;
        data[i][0] = 0x100 + i;
    }
    transition_root_b = (uint64_t)roots[1];
    field(0x6802, (uint64_t)roots[0]);
    transition_exit(0, transition_guest, 18, cr0, cr3, cr4);
    check(transition_seen[0] == 0x100 && transition_seen[1] == 0x101, 232);
    check(transition_seen[2] == transition_root_b &&
              read_field(0x6802) == transition_root_b,
          233);
    check(transition_seen[3] == 0x535441434b, 234); // Guest stack round trip.
    check(read_field(0x681e) == (uint64_t)transition_exit1 &&
              read_field(0x440c) == 3,
          235);
    transition_exit(1, (void (*)(void))((uint64_t)transition_exit1 + 3), 18,
                    cr0, cr3, cr4);
    check(transition_seen[4] == 1 &&
              read_field(0x681e) == (uint64_t)transition_exit2,
          236);

    // Masked TS with shadow zero: CLTS must not clear the real guest TS.
    field(0x6802, (uint64_t)roots[0]);
    field(0x6800, cr0 | 8);
    field(0x6000, 8);           // CR0 guest/host mask
    field(0x6004, cr0 & ~8ull); // CR0 read shadow
    transition_exit(1, transition_clts, 18, cr0, cr3, cr4);
    check((read_field(0x6800) & 8) && !(transition_seen[5] & 8), 237);
    check(read_field(0x681e) == (uint64_t)transition_clts_exit, 238);
    field(0x6004, cr0 | 8);
    transition_exit(1, transition_clts, 28, cr0, cr3, cr4);
    check(read_field(0x6400) == 0x20 && (read_field(0x6800) & 8), 239);
    check(read_field(0x681e) == (uint64_t)transition_clts, 240);

    field(0x6000, 0);
    field(0x6004, cr0);
    field(0x6800, cr0);
    field(0x4002, controls(0x48e, 1u << 15)); // CR3-load exiting
    transition_exit(1, transition_cr3, 28, cr0, cr3, cr4);
    check(read_field(0x6400) == 3 && read_field(0x6802) == (uint64_t)roots[0],
          241);
    check(read_field(0x681e) == (uint64_t)transition_cr3_instruction, 242);

    field(0x4002, controls(0x48e, 0));
    field(0x4004, 1u << 14); // Intercept #PF, including its error code.
    uint64_t saved_cr2, after_cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(saved_cr2));
    __asm__ volatile("mov %0, %%cr2" ::"r"(0x12345000ull) : "memory");
    transition_exit(1, transition_pf, 0, cr0, cr3, cr4);
    __asm__ volatile("mov %%cr2, %0" : "=r"(after_cr2));
    check(after_cr2 == 0x12345000 && read_field(0x6400) == 0x600000, 243);
    check(read_field(0x4404) == 0x80000b0e && read_field(0x4406) == 0, 244);
    check(read_field(0x681e) == (uint64_t)transition_pf, 245);
    __asm__ volatile("mov %0, %%cr2" ::"r"(saved_cr2) : "memory");

    // Disabled SMXE/OSXSAVE: #UD wins over GETSEC/XSETBV's VM exits.
    field(0x4004, 1u << 6);
    field(0x6804, cr4 & ~((1ull << 14) | (1ull << 18)));
    transition_exit(1, transition_getsec, 0, cr0, cr3, cr4);
    check(read_field(0x4404) == 0x80000306 &&
              read_field(0x681e) == (uint64_t)transition_getsec,
          246);
    transition_exit(1, transition_xsetbv, 0, cr0, cr3, cr4);
    check(read_field(0x4404) == 0x80000306 &&
              read_field(0x681e) == (uint64_t)transition_xsetbv_instruction,
          247);

    // Late failure loads host state, preserves earlier exception-exit fields,
    // and must not mark a previously clear VMCS as launched.
    uint64_t info = read_field(0x4404), error = read_field(0x4406);
    check(!(op_vmclear((uint64_t)&pointer, 0, 0) & 0x41), 248);
    check((op_vmresume(0, 0, 0) & 0x8d5) == 1, 249);
    check(!(op_vmptrld((uint64_t)&pointer, 0, 0) & 0x41), 250);
    field(0x6826, 1ull << 48); // Noncanonical guest SYSENTER_EIP.
    transition_exit(0, guest_entry, (1ull << 31) | 33, cr0, cr3, cr4);
    check(read_field(0x6400) == 0 && read_field(0x4404) == info &&
              read_field(0x4406) == error && guest_runs == 0,
          251);
    check((op_vmresume(0, 0, 0) & 0x8d5) == 0x40 && read_field(0x4400) == 5,
          252);

    // Only the primary-control bit is invalid; other controls/state are valid.
    // Bit 31 is now supported, so do not reuse the old no-EPT negative test.
    field(0x6826, 0);
    field(0x4002, controls(0x48e, 0) | 1);
    check((enter_guest(0) & 0x8d5) == 0x40 && read_field(0x4400) == 7, 253);
    uint64_t after_cr0, after_cr3;
    __asm__ volatile("mov %%cr0, %0; mov %%cr3, %1"
                     : "=r"(after_cr0), "=r"(after_cr3));
    check(after_cr0 == cr0 && after_cr3 == cr3 && guest_runs == 0 &&
              *(volatile uint64_t *)0x400000 == 0x484f5354,
          254);
}
#endif

#ifdef VMX_ADDRESS_TEST
extern void address_guest(void), address_exit(void), address_exit2(void);

static void
address_enter(unsigned resume, void (*rip)(void), uint64_t code)
{
    field(0x681e, (uint64_t)rip);
    check(enter_guest(resume) == 0, code);
    check(read_field(0x4402) == 18, code + 1);
}

static void
test_vmx_address(uint64_t host_cr3)
{
    uint32_t a = 0x80000008, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    unsigned width = a & 0xff;
    uint64_t bad = 1ull << width;
    uint64_t pointer = bad;
    check((op_vmptrld((uint64_t)&pointer, 0, 0) & 0x41) == 0x40, 500);
    check(read_field(0x4400) == 9, 501);
    check((op_vmclear((uint64_t)&pointer, 0, 0) & 0x41) == 0x40, 502);
    check(read_field(0x4400) == 2, 503);
    check(!(op_vmxoff(0, 0, 0) & 0x41), 504);
    check((op_vmxon((uint64_t)&pointer, 0, 0) & 0x41) == 1, 505);
    pointer = (uint64_t)on_region;
    check(!(op_vmxon((uint64_t)&pointer, 0, 0) & 0x41), 506);
    pointer = (uint64_t)vmcs_region;
    check(!(op_vmptrld((uint64_t)&pointer, 0, 0) & 0x41), 507);

    const uint64_t root = host_cr3 & ~0xfffull;
    unsigned launched = 0;
    for (unsigned bit = 0; bit < 13; ++bit) {
        if (bit == 3 || bit == 4) {
            continue;
        }
        uint64_t low = bit == 12 ? 0xfe7 : 1ull << bit;
        field(0x6c02, root | low);
        field(0x6802, root | low);
        address_enter(launched, guest_entry, 510);
        launched = 1;
        uint64_t live_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(live_cr3));
        check(live_cr3 == (root | low) && read_field(0x6802) == (root | low),
              512);
    }
    field(0x6c02, host_cr3);
    field(0x6802, host_cr3);
    uint64_t runs = guest_runs;
    address_enter(1, address_guest, 520);
    check(guest_runs == runs + 1 && read_field(0x6802) == (root | 1), 522);
    check(read_field(0x681e) == (uint64_t)address_exit, 523);
    address_enter(1, (void (*)(void))((uint64_t)address_exit + 3), 524);
    check(guest_runs == runs + 2 && read_field(0x6802) == (root | 1), 526);
    check(read_field(0x681e) == (uint64_t)address_exit2, 527);

    field(0x6c02, root | bad);
    check((enter_guest(1) & 0x41) == 0x40, 530);
    check(read_field(0x4400) == 8, 531); // Early invalid host state.
    field(0x6c02, host_cr3);
    field(0x6802, root | bad);
    check(enter_guest(1) == 0, 532);
    check(read_field(0x4402) == ((1ull << 31) | 33),
          533); // Late guest failure.
    check(guest_runs == runs + 2, 534);
    field(0x6802, host_cr3);

    if (width == 52) {
        const uint64_t high = 1ull << 48, alias = 0x800000;
        // Map actual high RAM into a low GVA using a legal host PDE.
        // Every region/header/root write below is a CPU store, not a proxy.
        uint64_t *pml4 = (uint64_t *)root;
        uint64_t *pdpt = (uint64_t *)(pml4[0] & ~0xfffull);
        volatile uint64_t *pd = (uint64_t *)(pdpt[0] & ~0xfffull);
        uint64_t saved = pd[4];
        pd[4] = high | 0x83;
        __asm__ volatile("invlpg (%0)" ::"r"(alias) : "memory");
        volatile uint32_t *regions = (uint32_t *)alias;
        regions[0] = regions[1024] = (uint32_t)msr(0x480) & 0x7fffffff;
        for (unsigned i = 0; i < 512; ++i) {
            ((volatile uint64_t *)(alias + 0x2000))[i] = pml4[i];
            ((volatile uint64_t *)(alias + 0x3000))[i] = pml4[i];
        }
        __asm__ volatile("mfence" ::: "memory");
        pointer = high + 0x1000;
        check(!(op_vmclear((uint64_t)&pointer, 0, 0) & 0x41), 540);
        check(!(op_vmptrld((uint64_t)&pointer, 0, 0) & 0x41), 541);
        field(0x201a, 0x1234501e);
        check(read_field(0x201a) == 0x1234501e, 542);
        check(!(op_vmclear((uint64_t)&pointer, 0, 0) & 0x41), 543);
        check(!(op_vmxoff(0, 0, 0) & 0x41), 544);
        pointer = high;
        check(!(op_vmxon((uint64_t)&pointer, 0, 0) & 0x41), 545);
        pointer = (uint64_t)vmcs_region;
        check(!(op_vmptrld((uint64_t)&pointer, 0, 0) & 0x41), 546);
        field(0x6c02, high + 0x3000);
        field(0x6802, high + 0x2000);
        address_enter(1, guest_entry, 550);
        uint64_t live_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(live_cr3));
        check(live_cr3 == high + 0x3000 && read_field(0x6802) == high + 0x2000,
              552);
        // Root-mode checkpoint with real high VMXON/VMCS/root addresses.
        __asm__ volatile(".byte 0x0f, 0x04; .word 0x21" ::"D"(0ull)
                         : "memory");
        field(0x6c02, host_cr3);
        field(0x6802, host_cr3);
        __asm__ volatile("mov %0, %%cr3" ::"r"(host_cr3) : "memory");
        pd[4] = saved;
        __asm__ volatile("invlpg (%0)" ::"r"(alias) : "memory");
    }
}
#endif

#ifdef VMX_NMI_TEST
volatile uint64_t nmi_events, nmi_depth, nmi_fixed;
static volatile uint64_t nmi_ept[6][512] __attribute__((aligned(4096)));
static uint64_t nmi_paging[4][512] __attribute__((aligned(4096)));
static uint64_t nmi_stack[512] __attribute__((aligned(4096)));
// Segment-load accessed-bit emulation is outside this regression. Supply
// accessed descriptors so a handler exit can resume without rewriting CS.
static const uint64_t nmi_gdt[] = {0, 0x00af9b000000ffff, 0x00cf93000000ffff};
extern void nmi_guest(void), nmi_handler(void), nmi_handler_exit(void);
extern void nmi_done(void), nmi_iret(void), nmi_load(void);
extern void nmi_fault_handler(void), nmi_wrong_path(void);
extern void nmi_priority(void), nmi_fixed_handler(void);

static void
nmi_gate(unsigned vector, void (*handler)(void))
{
    uint64_t target = (uint64_t)handler;
    idt[vector * 2] = (target & 0xffff) | (8ull << 16) | (0x8eull << 40) |
                      ((target & 0xffff0000) << 32);
    idt[vector * 2 + 1] = target >> 32;
}

static void
nmi_entry(void (*entry)(void), uint64_t rsp, unsigned blocked)
{
    field(0x4816, 0xa09b);
    field(0x4818, 0xc093);
    field(0x804, 16);
    field(0x681c, rsp);
    field(0x681e, (uint64_t)entry);
    field(0x6820, 2);
    field(0x4824, blocked);
    field(0x4016, 0);
}

static void
nmi_exit(unsigned resume, unsigned reason, void (*rip)(void), unsigned blocked,
         uint64_t code)
{
    check(enter_guest(resume) == 0, code);
    check(read_field(0x4402) == reason, code + 1);
    check(read_field(0x681e) == (uint64_t)rip, code + 2);
    check((read_field(0x4824) & 8) == blocked, code + 3);
    check(read_field(0x4408) == 0, code + 4);
}

static void
test_vmx_nmi(uint64_t host_cr3)
{
    nmi_gate(2, nmi_handler);
    nmi_gate(14, nmi_fault_handler);
    nmi_gate(13, nmi_fault_handler);
    nmi_gate(64, nmi_fixed_handler);
    const uint64_t top = (uint64_t)(guest_stack + sizeof(guest_stack));
    const uint64_t frame = (uint64_t)nmi_stack;
    unsigned launched = 0;
    field(0x6816, (uint64_t)nmi_gdt);
    field(0x4810, sizeof(nmi_gdt) - 1);
    // Independent guest paging leaves 1 GiB unmapped for a real #PF.
    // EPT uses 4 KiB leaves for the ELF's first 2 MiB, including IRET's stack.
    nmi_paging[0][0] = (uint64_t)nmi_paging[1] | 3;
    nmi_paging[1][0] = (uint64_t)nmi_paging[2] | 3;
    nmi_paging[1][3] = (uint64_t)nmi_paging[3] | 3;
    nmi_paging[3][503] = 0xfee00083;
    nmi_ept[0][0] = (uint64_t)nmi_ept[1] | 7;
    nmi_ept[1][0] = (uint64_t)nmi_ept[2] | 7;
    nmi_ept[1][3] = (uint64_t)nmi_ept[3] | 7;
    nmi_ept[3][503] = (uint64_t)nmi_ept[5] | 7;
    nmi_ept[5][0] = 0xfee00007; // UC APIC mapping, not WB RAM.
    for (unsigned i = 0; i < 512; ++i) {
        nmi_paging[2][i] = ((uint64_t)i << 21) | 0x83;
        nmi_ept[4][i] = ((uint64_t)i << 12) | 0x37;
    }
    nmi_ept[2][0] = (uint64_t)nmi_ept[4] | 7;
    field(0x6802, (uint64_t)nmi_paging[0]);

    // No EPT, EPT without A/D, and EPT with real conditional A/D updates.
    for (unsigned mode = 0; mode < 3; ++mode) {
        field(0x4002, controls(0x48e, mode ? 1u << 31 : 0));
        field(0x401e, mode ? 2 : 0);
        field(0x201a, (uint64_t)nmi_ept[0] | 0x1e | (mode == 2 ? 64 : 0));
        for (unsigned source = 0; source < 3; ++source) {
            nmi_events = nmi_depth = nmi_fixed = 0;
            nmi_entry(source == 2   ? nmi_priority
                      : source == 1 ? nmi_done
                                    : nmi_guest,
                      top, source == 2 ? 8 : 0);
            if (source == 1) {
                field(0x4016, 0x80000202);
            }
            if (source == 2) {
                field(0x6820, 0x202);
            }
            nmi_exit(launched, 18, nmi_handler_exit, 8, 310);
            launched = 1;
            check(nmi_events == 1 && nmi_depth == 1, 315);
            field(0x4016, 0);
            field(0x681e, (uint64_t)nmi_handler_exit + 3);
            nmi_exit(1, 18, nmi_done, 0, 330);
            check(nmi_events == 2 && nmi_depth == 0, 335);
            check(nmi_fixed == (source == 2), 336);
        }

        nmi_stack[0] = (uint64_t)nmi_done;
        nmi_stack[1] = 8;
        nmi_stack[2] = 2;
        nmi_stack[3] = top;
        nmi_stack[4] = 16;
        nmi_entry(nmi_iret, frame, 8);
        nmi_exit(1, 18, nmi_done, 0, 340);

        // NMI exiting is not advertised today. Do not mask an unsupported
        // requested control down to zero and call that a passing test.
        field(0x4000, controls(0x48d, 0) | (1u << 3));
        nmi_entry(nmi_iret, frame, 8);
        if ((msr(0x48d) >> 32) & (1u << 3)) {
            nmi_exit(1, 18, nmi_done, 8, 350);
        } else {
            check((enter_guest(1) & 0x41) == 0x40, 350);
            check(read_field(0x4400) == 7 && read_field(0x4824) == 8, 351);
        }
        field(0x4000, controls(0x48d, 0));

        field(0x4004, 1u << 14);
        nmi_entry(nmi_iret, 0x40000000, 8);
        nmi_exit(1, 0, nmi_iret, 0, 360);
        check(read_field(0x4404) == 0x80001b0e, 365);
        check(read_field(0x6400) == 0x40000000, 366);
        field(0x4004, 0);
        nmi_entry(nmi_iret, frame, 8);
        nmi_stack[0] = 1ull << 48; // Non-memory canonical-RIP check: #GP.
        field(0x4004, 1u << 13);
        nmi_exit(1, 0, nmi_iret, 0, 370);
        check(read_field(0x4404) == 0x80000b0d, 375);
        // Accept that same #GP and reach its handler, ending IRET lifetime.
        field(0x4004, 0);
        nmi_entry(nmi_iret, frame, 8);
        nmi_exit(1, 18, nmi_fault_handler, 0, 430);
        nmi_stack[0] = (uint64_t)nmi_done;
        field(0x4004, 0);

        nmi_entry(nmi_wrong_path, frame, 8);
        nmi_exit(1, 18, nmi_done, 8, 380);
        if (!mode) {
            continue;
        }

        volatile uint64_t *stack_leaf = &nmi_ept[4][frame >> 12];
        uint64_t stack_mapping = frame | 0x37;
        for (unsigned misconfig = 0; misconfig < 2; ++misconfig) {
            *stack_leaf = frame | (misconfig ? 0x32 : 0x30);
            nmi_entry(nmi_iret, frame, 8);
            nmi_exit(1, misconfig ? 49 : 48, nmi_iret, 0, 390);
            check(read_field(0x6400) == (misconfig ? 0 : 0x1181), 395);
            check(read_field(0x2400) == frame, 396);
            if (!misconfig) {
                // Re-enter with the saved, now-unblocked state. A second
                // fault at the same RIP must not reuse the old attribution.
                nmi_exit(1, 48, nmi_iret, 0, 440);
                check(read_field(0x6400) == 0x181, 445);
            }
            *stack_leaf = stack_mapping;
            nmi_exit(1, 18, nmi_done, 0, 400);
            check(read_field(0x6400) == 0, 405);
        }
        *stack_leaf = frame | 0x30;
        nmi_entry(nmi_load, frame, 8);
        nmi_exit(1, 48, nmi_load, 8, 410);
        check(read_field(0x6400) == 0x181, 415);
        *stack_leaf = stack_mapping;
        // A later descriptor read is still part of IRET, unlike its fetch.
        const uint64_t gdt = (uint64_t)nmi_gdt;
        unsigned gdt_page = gdt >> 12;
        uint64_t gdt_mapping = nmi_ept[4][gdt_page];
        nmi_ept[4][gdt_page] = (gdt_mapping & ~7ull);
        nmi_entry(nmi_iret, frame, 8);
        nmi_exit(1, 48, nmi_iret, 0, 450);
        check(read_field(0x6400) == 0x1181 && read_field(0x2400) == gdt + 8,
              455);
        nmi_ept[4][gdt_page] = gdt_mapping;
        unsigned code_page = (uint64_t)nmi_iret >> 12;
        uint64_t code_mapping = nmi_ept[4][code_page];
        nmi_ept[4][code_page] = code_mapping & ~4ull;
        nmi_entry(nmi_iret, frame, 8);
        nmi_exit(1, 48, nmi_iret, 8, 420);
        check(!(read_field(0x6400) & 0x1000), 425);
        nmi_ept[4][code_page] = code_mapping;
        // IRET completes, then its target fetch faults: no IRET attribution.
        code_page = (uint64_t)nmi_done >> 12;
        code_mapping = nmi_ept[4][code_page];
        nmi_ept[4][code_page] = code_mapping & ~4ull;
        nmi_entry(nmi_iret, frame, 8);
        nmi_exit(1, 48, nmi_done, 0, 460);
        check(!(read_field(0x6400) & 0x1000), 465);
        nmi_ept[4][code_page] = code_mapping;
    }
    field(0x4002, controls(0x48e, 0));
    field(0x6802, host_cr3);
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
#elif defined(VMX_TRANSITION_TEST)
    test_vmx_transition(cr0, cr3, cr4, pointer);
#elif defined(VMX_NMI_TEST)
    test_vmx_nmi(cr3);
#elif defined(VMX_ADDRESS_TEST)
    test_vmx_address(cr3);
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
