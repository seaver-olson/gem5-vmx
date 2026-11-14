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

#if defined(VMX_MIGRATE_TEST) || defined(VMX_MSR_TEST)
uint8_t ap_stack[8192] __attribute__((aligned(16)));
extern uint8_t ap_boot[], ap_boot_end[], ap_gdtr[], ap_root[];

static void
start_ap(uint64_t cr3, uint64_t gdt)
{
    check(ap_boot_end - ap_boot < 4096, 613);
    volatile uint8_t *boot = (volatile uint8_t *)0x8000;
    for (unsigned i = 0; i < (unsigned)(ap_boot_end - ap_boot); ++i) {
        boot[i] = ap_boot[i];
    }
    *(volatile uint64_t *)(gdt + 40) = 0x00cf9b000000ffff;
    *(volatile uint32_t *)(boot + (ap_gdtr - ap_boot) + 2) = gdt;
    *(volatile uint32_t *)(boot + (ap_root - ap_boot)) = cr3;
    __asm__ volatile("mfence" ::: "memory");
    volatile uint32_t *apic = (volatile uint32_t *)0xfee00000;
    apic[0x310 / 4] = 1u << 24;
    apic[0x300 / 4] = 0xc500;
    while (apic[0x300 / 4] & (1u << 12)) {}
    apic[0x300 / 4] = 0x608;
}
#endif

#ifdef VMX_MIGRATE_TEST
static uint32_t ap_on[1024] __attribute__((aligned(4096)));
static uint32_t bsp_vmcs[1024] __attribute__((aligned(4096)));
static volatile unsigned migration_done;

static void
migration_entry(unsigned runs, uint64_t eptp)
{
    check(read_field(0x201a) == eptp, 600);
    check(read_field(0x681e) == (uint64_t)guest_entry, 601);
    check((op_vmresume(0, 0, 0) & 0x41) == 0x40, 602);
    check(read_field(0x4400) == 5, 603); // VMCLEAR reset launch state.
    check(enter_guest(0) == 0 && guest_runs == runs, 604);
    check(read_field(0x4402) == 18 &&
              read_field(0x681e) == (uint64_t)guest_exit,
          605);
    field(0x681e, (uint64_t)guest_entry);
}

void
migration_ap(void)
{
    struct __attribute__((packed))
    {
        uint16_t limit;
        uint64_t base;
    } idtr = {sizeof(idt) - 1, (uint64_t)idt};
    __asm__ volatile("lidt %0" ::"m"(idtr) : "memory");
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0; mov %%cr4, %1" : "=r"(cr0), "=r"(cr4));
    cr0 = (cr0 | msr(0x486)) & msr(0x487);
    cr4 = (cr4 | msr(0x488) | (1ull << 13)) & msr(0x489);
    __asm__ volatile("mov %0, %%cr0; mov %1, %%cr4" ::"r"(cr0), "r"(cr4)
                     : "memory");
    uint64_t pointer = (uint64_t)ap_on;
    check(!(op_vmxon((uint64_t)&pointer, 0, 0) & 0x41), 606);
    pointer = (uint64_t)vmcs_region;
    check(!(op_vmptrld((uint64_t)&pointer, 0, 0) & 0x41), 607);
    migration_entry(2, 0x1234501e);
    field(0x201a, 0x2345601e);
    check(!(op_vmclear((uint64_t)&pointer, 0, 0) & 0x41), 608);
    check(!(op_vmxoff(0, 0, 0) & 0x41), 609);
    __asm__ volatile("mfence" ::: "memory");
    migration_done = 1;
    // Leave a real System wakeup event pending across the BSP's checkpoint.
    __asm__ volatile(".byte 0x0f, 0x04; .word 0x02" ::"D"(50000ull)
                     : "memory");
    migration_done = 2;
}

static void
test_vmx_migrate(uint64_t cr3, uint64_t gdt)
{
    field(0x201a, 0x1234501e); // Opaque component; EPT stays disabled.
    check(enter_guest(0) == 0 && guest_runs == 1, 610);
    field(0x681e, (uint64_t)guest_entry);
    uint64_t pointer = (uint64_t)vmcs_region;
    check(!(op_vmclear((uint64_t)&pointer, 0, 0) & 0x41), 611);
    // CPU 1's VMXOFF must not deactivate CPU 0's separate current VMCS.
    bsp_vmcs[0] = ap_on[0] = on_region[0];
    pointer = (uint64_t)bsp_vmcs;
    check(!(op_vmptrld((uint64_t)&pointer, 0, 0) & 0x41), 612);
    field(0x681e, 0xfeedface);

    start_ap(cr3, gdt);
    for (unsigned i = 0; !migration_done && i < 1000000; ++i) {
        __asm__ volatile("pause" ::: "memory");
    }
    check(migration_done, 614);
    __asm__ volatile("mfence" ::: "memory");
    check(read_field(0x681e) == 0xfeedface, 615);
    // Checkpoint while root-mode: the shared cleared VMCS is not current
    // on either CPU, but its components must still survive restoration.
    __asm__ volatile(".byte 0x0f, 0x04; .word 0x21" ::"D"(0ull) : "memory");
    pointer = (uint64_t)vmcs_region;
    check(!(op_vmptrld((uint64_t)&pointer, 0, 0) & 0x41), 616);
    migration_entry(3, 0x2345601e);
    check(!(op_vmclear((uint64_t)&pointer, 0, 0) & 0x41), 617);
    for (unsigned i = 0; migration_done != 2 && i < 1000000; ++i) {
        __asm__ volatile("pause" ::: "memory");
    }
    check(migration_done == 2, 618);
}
#endif

#ifdef VMX_MSR_TEST
struct MsrItem
{
    uint32_t index, reserved;
    uint64_t value;
};
static volatile struct MsrItem entry_msrs[512] __attribute__((aligned(4096)));
static volatile struct MsrItem store_msrs[512] __attribute__((aligned(4096)));
static volatile struct MsrItem host_msrs[512] __attribute__((aligned(4096)));
static uint64_t msr_ept[4][512] __attribute__((aligned(4096)));
static volatile unsigned msr_abort_mode, msr_ap_ready, msr_host_returned;
static volatile unsigned msr_timed_wakeup, msr_quiesce_stage;
static volatile uint32_t msr_region_word;
extern void msr_guest_fault(void);

static void
msr_write(uint32_t index, uint64_t value)
{
    __asm__ volatile("wrmsr" ::"c"(index), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32))
                     : "memory");
}

static void
msr_lists(unsigned entries, unsigned stores, unsigned loads)
{
    field(0x200a, (uint64_t)entry_msrs);
    field(0x2006, (uint64_t)store_msrs);
    field(0x2008, (uint64_t)host_msrs);
    field(0x4014, entries);
    field(0x400e, stores);
    field(0x4010, loads);
}

static void
msr_enter(unsigned resume, uint64_t reason)
{
    field(0x681e, (uint64_t)guest_entry);
    check(enter_guest(resume) == 0, 700);
    check(read_field(0x4402) == reason, 701);
}

void
msr_abort_observer(void)
{
    msr_ap_ready = 1;
    volatile uint32_t *apic = (volatile uint32_t *)0xfee00000;
    if (msr_timed_wakeup) {
        while (!msr_quiesce_stage) {
            __asm__ volatile("pause" ::: "memory");
        }
        // Repeat until the BSP acknowledges its early wake. An IPI sent
        // before it actually suspends must not leave it waiting for the
        // original timer, which must remain pending across the checkpoint.
        while (msr_quiesce_stage != 2) {
            apic[0x310 / 4] = 0;
            apic[0x300 / 4] = 0x4040;
            while (apic[0x300 / 4] & (1u << 12)) {}
        }
    }
    volatile uint32_t *indicator = &vmcs_region[1];
    for (unsigned i = 0; !*indicator && i < 1000000; ++i) {
        __asm__ volatile("pause" ::: "memory");
    }
    const unsigned expected =
        msr_abort_mode == 1 || msr_abort_mode == 4 ? 1 : 4;
    check(*indicator == expected, 750);
    if (msr_abort_mode == 3 || msr_abort_mode == 5) {
        // A late entry failure must skip the exit-store area entirely.
        check(store_msrs[0].value == 0xfeedface, 751);
    } else {
        check(store_msrs[0].value == 0x22, 752);
    }
    check(store_msrs[2].value == 0xfeedface, 753);
    check(vmcs_region[0] == on_region[0] && vmcs_region[2] == msr_region_word,
          754);
    // None of these events may restart a VMX-aborted logical processor.
    const uint32_t events[] = {0x4400, 0xc500, 0x608, 0x4200, 0x4040};
    for (unsigned round = 0; round < 2; ++round) {
        for (unsigned e = 0; e < sizeof(events) / sizeof(events[0]); ++e) {
            apic[0x310 / 4] = 0;
            apic[0x300 / 4] = events[e];
            while (apic[0x300 / 4] & (1u << 12)) {}
            for (unsigned i = 0; i < 1000; ++i) {
                __asm__ volatile("pause" ::: "memory");
            }
            check(!msr_host_returned && *indicator == expected, 755);
        }
        check(guest_runs == (msr_abort_mode >= 3 ? 0 : 1), 756);
        // The harness may checkpoint here; repeat the wakeup attempts after
        // drain/resume or restore. CPU 0 must remain halted throughout.
        __asm__ volatile(".byte 0x0f, 0x04; .word 0x21" ::"D"(0ull)
                         : "memory");
    }
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void
test_vmx_msrs(uint64_t cr3, uint64_t gdt, uint64_t pointer)
{
    uint64_t input = 0;
    __asm__ volatile(".byte 0x0f, 0x04; .word 0x50" ::"D"((uint64_t)&input),
                     "S"(8ull), "d"(0ull)
                     : "rax", "memory");
    __asm__ volatile("xor %%eax, %%eax; cpuid" ::
                         : "rax", "rbx", "rcx", "rdx", "memory");
    msr_abort_mode = input & 0xff;
    msr_timed_wakeup = (input >> 8) & 1;
    entry_msrs[0] = (struct MsrItem){0xc0000103, 0, 0x22};
    store_msrs[0] = (struct MsrItem){0xc0000103, 0, 0xfeedface};
    store_msrs[1] = (struct MsrItem){0xdead, 0, 0xfeedface};
    store_msrs[2] = (struct MsrItem){0xc0000103, 0, 0xfeedface};
    host_msrs[0] = (struct MsrItem){0x174, 0, 0x55};
    host_msrs[1] = (struct MsrItem){0xdead, 0, 0};
    if (msr_abort_mode) {
        check(msr_abort_mode <= 5, 757);
        msr_region_word = vmcs_region[2];
        msr_lists(1, msr_abort_mode == 2 ? 1 : 3, 2);
        if (msr_abort_mode == 3) {
            entry_msrs[0].index = 0xdead;
        }
        if (msr_abort_mode == 5) {
            field(0x6820, 0); // Invalid guest state, before entry-list loads.
        }
        if (msr_abort_mode == 4) {
            field(0x681e, (uint64_t)msr_guest_fault);
            field(0x4004, 1u << 6);
        }
        start_ap(cr3, gdt);
        while (!msr_ap_ready) {
            __asm__ volatile("pause" ::: "memory");
        }
        if (msr_timed_wakeup) {
            // IF remains clear: a fixed IPI wakes quiescence without
            // dispatching a handler. Keep its original 10 ms timer alive.
            msr_quiesce_stage = 1;
            __asm__ volatile(".byte 0x0f, 0x04; .word 0x02" ::"D"(10000000ull)
                             : "memory");
            msr_quiesce_stage = 2;
        }
        enter_guest(0);
        msr_host_returned = 1;
        check(0, 758); // An architectural abort cannot return here.
    }

    // All three zero counts ignore even unusable addresses.
    msr_lists(0, 0, 0);
    field(0x200a, ~0ull);
    field(0x2006, ~0ull);
    field(0x2008, ~0ull);
    msr_enter(0, 18);

    uint32_t a = 0x80000008, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    const uint64_t beyond = 1ull << (a & 0xff);
    const unsigned counts[] = {0x4014, 0x400e, 0x4010};
    const unsigned addresses[] = {0x200a, 0x2006, 0x2008};
    const uint64_t invalid[] = {1, beyond, beyond - 16, ~15ull};
    for (unsigned area = 0; area < 3; ++area) {
        for (unsigned bad = 0; bad < 4; ++bad) {
            msr_lists(0, 0, 0);
            msr_write(0xc0000103, 0xabcdef);
            field(addresses[area], invalid[bad]);
            field(counts[area], bad >= 2 ? 2 : 1);
            check((enter_guest(1) & 0x41) == 0x40, 702);
            check(read_field(0x4400) == 7, 703);
            check(msr(0xc0000103) == 0xabcdef, 704);
        }
    }

    // Duplicate MSRs prove list order. Stores observe guest values before
    // host VMCS fields and host MSR-list loads can replace them.
    entry_msrs[0] = (struct MsrItem){0xc0000103, 0, 0x11};
    entry_msrs[1] = (struct MsrItem){0x174, 0, 0x22};
    entry_msrs[2] = (struct MsrItem){0xc0000103, 0, 0x33};
    store_msrs[0] = (struct MsrItem){0xc0000103, 0, 0xfeedface};
    store_msrs[1] = (struct MsrItem){0x174, 0, 0xfeedface};
    host_msrs[0] = (struct MsrItem){0xc0000103, 0, 0x44};
    host_msrs[1] = (struct MsrItem){0x174, 0, 0x55};
    host_msrs[2] = (struct MsrItem){0xc0000103, 0, 0x66};
    msr_lists(3, 2, 3);
    for (unsigned i = 0; i < 2; ++i) {
        msr_enter(1, 18);
        check(store_msrs[0].value == 0x33 && store_msrs[1].value == 0x22, 705);
        check(store_msrs[0].index == 0xc0000103 &&
                  store_msrs[1].index == 0x174 && !store_msrs[0].reserved &&
                  !store_msrs[1].reserved,
              706);
        check(msr(0xc0000103) == 0x66 && msr(0x174) == 0x55, 707);
    }

    // A fault-triggered exit uses the same store/load lists as VMCALL.
    field(0x4004, 1u << 6);
    field(0x681e, (uint64_t)msr_guest_fault);
    check(enter_guest(1) == 0 && read_field(0x4402) == 0, 708);
    check(store_msrs[0].value == 0x33 && msr(0xc0000103) == 0x66, 709);
    field(0x4004, 0);

    // FS/GS bases may be stored even though VMX forbids loading them here.
    field(0x680e, 0x12340000);
    field(0x6810, 0x22340000);
    store_msrs[0] = (struct MsrItem){0xc0000100, 0, 0};
    store_msrs[1] = (struct MsrItem){0xc0000101, 0, 0};
    store_msrs[2] = (struct MsrItem){0x480, 0, 0};
    msr_lists(0, 3, 0);
    msr_enter(1, 18);
    check(store_msrs[0].value == 0x12340000 &&
              store_msrs[1].value == 0x22340000 &&
              store_msrs[2].value == msr(0x480),
          724);
    store_msrs[0].index = 0xc0000103;
    msr_lists(3, 1, 3);

    // Item 2 fails after item 1, without running item 3 or guest injection.
    // A late entry failure recovers host MSRs, skips stores and guest saves,
    // and leaves a clear VMCS's launch state clear.
    const struct MsrItem bad_items[] = {
        {0xdead, 0, 0},
        {0x174, 1, 0},
        {0xc0000100, 0, 0},
        {0xc0000101, 0, 0},
        {0x800, 0, 0},
        {0x8ff, 0, 0},
        {0x480, 0, 0},
        {0x3a, 0, 0},
        {0x9b, 0, 0},
        {0xc0000080, 0, 1ull << 63},
        {0xc0000080, 0, msr(0xc0000080) ^ (1u << 8)},
        {0x277, 0, 2},
        {0xc0000103, 0, 1ull << 32},
        {0xc0000102, 0, 1ull << 48},
        {0x2ff, 0, 2},
        {0x200, 0, 2},
        {0x201, 0, 1},
        {0x250, 0, 2},
        {0x1d9, 0, 1ull << 63},
    };
    for (unsigned bad = 0; bad < sizeof(bad_items) / sizeof(bad_items[0]);
         ++bad) {
        check(!(op_vmclear((uint64_t)&pointer, 0, 0) & 0x41), 710);
        check(!(op_vmptrld((uint64_t)&pointer, 0, 0) & 0x41), 711);
        entry_msrs[0] = (struct MsrItem){0xc0000102, 0, 0x11110000};
        entry_msrs[1] = bad_items[bad];
        entry_msrs[2] = (struct MsrItem){0xc0000102, 0, 0x22220000};
        store_msrs[0].value = 0xfeedface;
        field(0x482a, 0x1234);
        field(0x4016, 0x80000306);
        msr_enter(0, 0x80000022);
        check(read_field(0x6400) == 2, 712);
        check(msr(0xc0000102) == 0x11110000, 713);
        check(msr(0xc0000103) == 0x66 && store_msrs[0].value == 0xfeedface,
              714);
        check(read_field(0x482a) == 0x1234 && read_field(0x4016) == 0x80000306,
              715);
        check((op_vmresume(0, 0, 0) & 0x41) == 0x40 && read_field(0x4400) == 5,
              716);
    }
    field(0x4016, 0);

    // Invalid guest state must perform the same host recovery.
    field(0x6820, 0);
    msr_write(0xc0000102, 0x33330000);
    msr_enter(0, 0x80000021);
    check(msr(0xc0000102) == 0x33330000 && msr(0xc0000103) == 0x66 &&
              store_msrs[0].value == 0xfeedface,
          717);
    field(0x6820, 2);

    // Capacity field zero recommends 512, not zero entries.
    check(((msr(0x485) >> 25) & 7) == 0, 718);
    for (unsigned i = 0; i < 512; ++i) {
        entry_msrs[i] = (struct MsrItem){0xc0000103, 0, 0x1000 + i};
        store_msrs[i] = (struct MsrItem){0xc0000103, 0, 0xfeedface};
        host_msrs[i] = (struct MsrItem){0xc0000103, 0, 0x2000 + i};
    }
    msr_lists(512, 512, 512);
    msr_enter(0, 18);
    for (unsigned i = 0; i < 512; ++i) {
        check(store_msrs[i].value == 0x11ff &&
                  store_msrs[i].index == 0xc0000103 && !store_msrs[i].reserved,
              719);
    }
    check(msr(0xc0000103) == 0x21ff, 720);

    // Failed VMRESUME must preserve the already-launched state too.
    entry_msrs[0].index = 0xdead;
    msr_lists(1, 0, 0);
    msr_enter(1, 0x80000022);
    check(read_field(0x6400) == 1, 725);
    entry_msrs[0].index = 0xc0000103;
    msr_enter(1, 18);

    // EFER.LMA writes are ignored. TSC uses ordinary MSR side effects.
    const uint64_t efer = msr(0xc0000080);
    entry_msrs[0] = (struct MsrItem){0xc0000080, 0, efer ^ (1u << 10)};
    entry_msrs[1] = (struct MsrItem){0x10, 0, 1000000};
    store_msrs[0] = (struct MsrItem){0xc0000080, 0, 0};
    store_msrs[1] = (struct MsrItem){0x10, 0, 0};
    host_msrs[0] = (struct MsrItem){0x10, 0, 2000000};
    msr_lists(2, 2, 1);
    msr_enter(1, 18);
    check(store_msrs[0].value == efer, 721);
    check(store_msrs[1].value >= 1000000 && store_msrs[1].value < 2000000 &&
              msr(0x10) >= 2000000,
          722);

    // List pointers are HPAs even while EPT denies access to those GPAs.
    msr_ept[0][0] = (uint64_t)msr_ept[1] | 7;
    msr_ept[1][0] = (uint64_t)msr_ept[2] | 7;
    msr_ept[2][0] = (uint64_t)msr_ept[3] | 7;
    for (unsigned i = 0; i < 512; ++i) {
        msr_ept[3][i] = (uint64_t)i * 4096 | 0x37;
    }
    msr_ept[3][(uint64_t)entry_msrs / 4096] = 0;
    msr_ept[3][(uint64_t)store_msrs / 4096] = 0;
    msr_ept[3][(uint64_t)host_msrs / 4096] = 0;
    field(0x201a, (uint64_t)msr_ept | 0x1e);
    field(0x401e, 2);
    field(0x4002, controls(0x48e, 1u << 31));
    msr_enter(1, 18);
    check(store_msrs[0].value == efer, 723);
    field(0x4002, controls(0x48e, 0));
    msr_lists(0, 0, 0);
}
#endif

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

    uint64_t saved_cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(saved_cr0));
    for (unsigned variant = 0; variant < 8; ++variant) {
        const unsigned ad = variant & 1;
        // CD/NW are not loaded from the VMCS. Exercise both cached and
        // uncached execution using the live CR0, clearing NW when CD=0.
        const uint64_t cr0 =
            (saved_cr0 & ~(3ull << 29)) | ((uint64_t)(variant >> 2) << 30);
        __asm__ volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");
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
        ept_data[2] = 0;
        ept_data[3] = 0;
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
        check(ept_data[0] == 0x1234567a && ept_data[1] == 0xfeedface &&
                  ept_data[2] == 0x87654321 && ept_data[3] == 0x1234567a,
              114);

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
    __asm__ volatile("mov %0, %%cr0" ::"r"(saved_cr0) : "memory");
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
    const uint64_t pat = msr(0x277);
    for (unsigned index = 0; index < 8; ++index) {
        for (unsigned reserved = 0; reserved < 3; ++reserved) {
            const uint64_t invalid = reserved == 0   ? 2
                                     : reserved == 1 ? 3
                                                     : 0x80;
            const unsigned shift = index * 8;
            fault_vector = fault_error = 0;
            probe_wrmsr(0x277,
                        (pat & ~(0xffull << shift)) | (invalid << shift));
            check(fault_vector == 13 && fault_error == 0, 650 + index);
            check(msr(0x277) == pat, 658 + index);
        }
    }
    const unsigned legal_pat[] = {0, 1, 4, 5, 6, 7};
    for (unsigned i = 0; i < sizeof(legal_pat) / sizeof(legal_pat[0]); ++i) {
        const uint64_t value = 0x0101010101010101ull * legal_pat[i];
        fault_vector = 0;
        probe_wrmsr(0x277, value);
        check(!fault_vector && msr(0x277) == value, 666 + i);
    }
    probe_wrmsr(0x277, pat);
    check(!fault_vector && msr(0x277) == pat, 672);
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
#elif defined(VMX_MIGRATE_TEST)
    test_vmx_migrate(cr3, gdtr.base);
#elif defined(VMX_MSR_TEST)
    test_vmx_msrs(cr3, gdtr.base, pointer);
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
