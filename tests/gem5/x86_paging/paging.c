#include <stdint.h>
#include <stddef.h>

#define P 1ull
#define W 2ull
#define U 4ull
#define A 32ull
#define D 64ull
#define PS 128ull
#define NX (1ull << 63)
#define VA (1ull << 39)
#define NONE (~0ull)

static uint64_t root[512] __attribute__((aligned(4096)));
static uint64_t pdpt[512] __attribute__((aligned(4096)));
static uint64_t pd[512] __attribute__((aligned(4096)));
static uint64_t pt[512] __attribute__((aligned(4096)));
static uint64_t data[512] __attribute__((aligned(4096)));
static uint64_t second[512] __attribute__((aligned(4096)));
static unsigned char large[1 << 21] __attribute__((aligned(1 << 21)));
static unsigned char idt[4096] __attribute__((aligned(16)));
volatile uint64_t resume_ip, fault_error, fault_vector;
extern void fault_entry(void), gp_entry(void), ss_entry(void);
extern void probe_read(uint64_t);
extern void probe_exec(uint64_t);
extern void probe_write(uint64_t);
extern void probe_efer_exec(uint64_t, uint64_t);
extern void probe_cr0(uint64_t), probe_cr3(uint64_t), probe_cr4(uint64_t);

static __attribute__((noreturn)) void finish(uint64_t code)
{
    if (code) {
        static const char filename[] = "failure.bin";
        uint64_t details[] = {code, fault_error, fault_vector,
                              root[1], pdpt[0], pd[0], pt[0]};
        uint64_t ignored;
        __asm__ volatile(".byte 0x0f, 0x04; .word 0x4f"
            : "=a"(ignored) : "D"((uint64_t)details), "S"(sizeof(details)),
              "d"(0ull), "c"((uint64_t)filename) : "memory");
    }
    __asm__ volatile(".byte 0x0f, 0x04; .word 0x22" :: "D"(0ull), "S"(code));
    for (;;) __asm__ volatile("hlt");
}

static void check(int ok, unsigned code) { if (!ok) finish(code); }
static void flush(void)
{
    __asm__ volatile("mov %0, %%cr3" :: "r"((uint64_t)root) : "memory");
}
static void mapping(int big)
{
    root[1] = (uint64_t)pdpt | P | W | U;
    pdpt[0] = (uint64_t)pd | P | W | U;
    pd[0] = big ? (uint64_t)large | P | W | U | PS :
                 (uint64_t)pt | P | W | U;
    pt[0] = (uint64_t)data | P | W | U;
    data[0] = 0xc3; /* ret */
    large[0] = 0xc3;
    flush();
}
static void access(void (*probe)(uint64_t), uint64_t address, uint64_t error,
                   unsigned code)
{
    fault_error = NONE;
    fault_vector = 0;
    probe(address);
    check(fault_error == error, code);
    if (error != NONE) {
        check(fault_vector == (code == 126 ? 13u : 14u), code);
        if (fault_vector == 14) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            check(cr2 == address, code);
        }
    }
}
void paging_test(void)
{
    uint64_t old_root;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_root));
    root[0] = ((uint64_t *)old_root)[0]; /* retain bootstrap identity mapping */
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    for (unsigned vector = 12; vector <= 14; vector++) {
        uint64_t target = (uint64_t)(vector == 14 ? fault_entry :
                                     vector == 13 ? gp_entry : ss_entry);
        uint64_t *gate = (uint64_t *)&idt[vector * 16];
        gate[0] = (target & 0xffff) | ((uint64_t)cs << 16) |
                  (0x8eull << 40) | ((target & 0xffff0000) << 32);
        gate[1] = target >> 32;
    }
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; }
        idtr = { sizeof(idt) - 1, (uint64_t)idt };
    __asm__ volatile("lidt %0" :: "m"(idtr));
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xc0000080));
    low |= 1u << 11; /* EFER.NXE */
    __asm__ volatile("wrmsr" :: "a"(low), "d"(high), "c"(0xc0000080));
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 1ull << 16; /* CR0.WP */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

    for (int big = 0; big < 2; big++) {
        uint64_t *levels[] = {&root[1], &pdpt[0], &pd[0], &pt[0]};
        int count = big ? 3 : 4;
        for (int level = 0; level < count; level++) {
            mapping(big);
            *levels[level] |= NX;
            flush();
            access(probe_exec, VA, 17, 10 + big * 10 + level);
            flush();
            access(probe_read, VA, NONE, 30 + big * 10 + level);
            access(probe_exec, VA, 17, 50 + big * 10 + level);
        }
        mapping(big);
        access(probe_read, VA, NONE, 70 + big);
        for (int i = 0; i < count; i++)
            check(*levels[i] & A, 72 + big * 4 + i);
        check(!(*levels[count - 1] & D), 80 + big);
        access(probe_write, VA, NONE, 82 + big);
        check(*levels[count - 1] & D, 84 + big);
        mapping(big);
        access(probe_write, VA, NONE, 86 + big);
        check(*levels[count - 1] & D, 88 + big);
        for (int level = 0; level < count; level++) {
            mapping(big);
            *levels[level] |= 1ull << 48;
            flush();
            access(probe_read, VA, 9, 100 + big * 10 + level);
        }
    }
    /* XD is an access right of a complete translation (SDM 5.6/5.7). A
     * deeper nonpresent or reserved entry means there is no translation:
     * the error code reports P=0 or RSVD=1, not an XD violation. */
    mapping(0);
    root[1] |= NX;
    pt[0] &= ~P;
    flush();
    access(probe_exec, VA, 16, 64);
    mapping(0);
    root[1] |= NX;
    pt[0] |= 1ull << 48;
    flush();
    access(probe_exec, VA, 25, 65);
    /* Changing the page size without invalidation may use either
     * translation (SDM 5.10.2.3), but must never stop the simulator. */
    mapping(0);
    access(probe_read, VA, NONE, 66);
    pd[0] = (uint64_t)large | P | W | U | PS;
    fault_error = NONE;
    fault_vector = 0;
    probe_read(VA + 0x5000);
    check(fault_error == NONE || (fault_vector == 14 && fault_error == 0), 67);
    flush();
    mapping(1);
    pd[0] |= 1ull << 13; /* reserved large-page alignment */
    flush();
    access(probe_read, VA, 9, 120);
    mapping(0);
    pd[0] &= ~W;
    flush();
    access(probe_write, VA, 3, 121);
    check(!(pt[0] & D), 122);
    /* NX is reserved when EFER.NXE is clear, including data accesses.
     * Nonpresent entries do not diagnose reserved encodings. */
    mapping(0);
    pt[0] |= NX;
    low &= ~(1u << 11);
    __asm__ volatile("wrmsr" :: "a"(low), "d"(high), "c"(0xc0000080));
    flush();
    access(probe_read, VA + 8, 9, 123);
    access(probe_exec, VA + 8, 9, 124);
    pt[0] &= ~P;
    flush();
    access(probe_read, VA + 8, 0, 125);
    low |= 1u << 11;
    __asm__ volatile("wrmsr" :: "a"(low), "d"(high), "c"(0xc0000080));
    /* The handler accepts #GP(0); canonicality must precede any walk. */
    access(probe_read, 0x0000800000000000ull, 0, 126);

    /* Architectural invalidation must order descriptor replacement and
     * prevent a pre-invalidation fetch from retiring on O3. */
    mapping(0);
    access(probe_read, VA, NONE, 140);
    pt[0] |= NX;
    __asm__ volatile("invlpg (%0)" :: "r"(VA) : "memory");
    access(probe_exec, VA, 17, 141);
    mapping(0);
    access(probe_read, VA, NONE, 142);
    second[0] = 0x13579bdf;
    pt[0] = (uint64_t)second | P | W | U;
    __asm__ volatile("invlpg (%0)" :: "r"(VA) : "memory");
    check(*(volatile uint64_t *)VA == 0x13579bdf, 143);

    /* EFER changes serialize instruction fetch without an extra CR3 write. */
    mapping(0);
    pt[0] |= NX;
    flush();
    for (unsigned i = 0; i < 20; ++i) {
        low ^= 1u << 11;
        fault_error = NONE;
        fault_vector = 0;
        probe_efer_exec(((uint64_t)high << 32) | low, VA);
        check(fault_vector == 14 &&
              fault_error == ((low & (1u << 11)) ? 17u : 9u), 150 + (i & 1));
    }

    /* Faulting control writes must leave the old root and permissions
     * intact. The optional test profile advertises PCID explicitly. */
    uint64_t cr4, observed;
    fault_error = NONE;
    fault_vector = 0;
    probe_cr0(cr0 & ~(1ull << 31)); /* PG cannot be cleared in 64-bit code. */
    __asm__ volatile("mov %%cr0, %0" : "=r"(observed));
    check(fault_vector == 13 && fault_error == 0 && observed == cr0, 182);
    uint32_t features, cpuid_eax;
    __asm__ volatile("cpuid" : "=a"(cpuid_eax), "=c"(features)
                     : "a"(1), "c"(0)
                     : "rbx", "rdx");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (!(features & (1u << 17))) {
        fault_error = NONE;
        fault_vector = 0;
        probe_cr4(cr4 | (1ull << 17));
        __asm__ volatile("mov %%cr4, %0" : "=r"(observed));
        check(fault_vector == 13 && fault_error == 0 && observed == cr4, 160);
    }
    for (unsigned bit = 48; bit < 64; ++bit) {
        fault_error = NONE;
        fault_vector = 0;
        probe_cr3((uint64_t)root | (1ull << bit));
        __asm__ volatile("mov %%cr3, %0" : "=r"(observed));
        check(fault_vector == 13 && fault_error == 0 &&
              observed == (uint64_t)root, 161 + bit - 48);
    }
    /* Ignored low bits must not be mistaken for reserved address bits. */
    fault_error = NONE;
    probe_cr3((uint64_t)root | 0xfe7);
    check(fault_error == NONE, 177);
    flush();
    if (features & (1u << 17)) {
        probe_cr3((uint64_t)root | 8); /* PWT prevents enabling PCIDE. */
        fault_error = NONE;
        fault_vector = 0;
        probe_cr4(cr4 | (1ull << 17));
        __asm__ volatile("mov %%cr4, %0" : "=r"(observed));
        check(fault_vector == 13 && fault_error == 0 && observed == cr4, 178);
        flush();
        fault_error = NONE;
        probe_cr4(cr4 | (1ull << 17));
        check(fault_error == NONE, 179);
        probe_cr3((uint64_t)root | (1ull << 63) | 17);
        __asm__ volatile("mov %%cr3, %0" : "=r"(observed));
        check(fault_error == NONE && observed == ((uint64_t)root | 17), 180);
        fault_error = NONE;
        fault_vector = 0;
        probe_cr3((uint64_t)root | (1ull << 62) | 17);
        __asm__ volatile("mov %%cr3, %0" : "=r"(observed));
        check(fault_vector == 13 && fault_error == 0 &&
              observed == ((uint64_t)root | 17), 181);
        probe_cr4(cr4);
        flush();
    }

    /* Leaf G is effective only with PGE. Test required invalidations,
     * without assuming that hardware must retain a stale global entry. */
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        probe_cr4(scenario ? cr4 | (1ull << 7) : cr4 & ~(1ull << 7));
        mapping(0);
        pt[0] |= 1ull << 8;
        flush();
        access(probe_read, VA, NONE, 190 + scenario * 2);
        second[0] = 0x76543210;
        pt[0] = (uint64_t)second | P | W | U | (1ull << 8);
        if (scenario == 0)
            flush();
        else if (scenario == 1)
            __asm__ volatile("invlpg (%0)" :: "r"(VA) : "memory");
        else
            probe_cr4(cr4 & ~(1ull << 7));
        check(*(volatile uint64_t *)VA == 0x76543210, 191 + scenario * 2);
    }
    probe_cr4(cr4 & ~(1ull << 7));
    flush();
    probe_cr4(cr4);

    /* WP changes must affect warm translations without a CR3 reload. */
    for (unsigned big = 0; big < 2; ++big) {
        mapping(big);
        pd[0] &= ~W;
        flush();
        access(probe_read, VA, NONE, 200 + big);
        probe_cr0(cr0 & ~(1ull << 16));
        access(probe_write, VA, NONE, 202 + big);
        check((big ? pd[0] : pt[0]) & D, 204 + big);
        probe_cr0(cr0);
        access(probe_write, VA, 3, 206 + big);
    }

    /* Functional requests cross two nonidentity mappings without filling
     * either translation or setting A/D, even during timing execution. */
    mapping(0);
    pt[1] = (uint64_t)second | P | W | U;
    data[511] = 0x0123456789abcdefull;
    second[0] = 0xfedcba9876543210ull;
    __asm__ volatile("mfence" ::: "memory");
    static const char filename[] = "functional.bin";
    uint64_t written;
    __asm__ volatile(".byte 0x0f, 0x04; .word 0x4f"
        : "=a"(written) : "D"(VA + 4088), "S"(16ull), "d"(0ull),
          "c"((uint64_t)filename) : "memory");
    check(written == 16, 130);
    check(!(root[1] & A) && !(pdpt[0] & A) && !(pd[0] & A) &&
          !(pt[0] & (A | D)) && !(pt[1] & (A | D)), 131);
    uint64_t received;
    __asm__ volatile(".byte 0x0f, 0x04; .word 0x50"
        : "=a"(received) : "D"(VA), "S"(8ull), "d"(0ull) : "memory");
    /* Do not let O3 observe the destination before the host-side functional
     * write executes. The pseudo-op is not an architectural memory store. */
    __asm__ volatile("xor %%eax, %%eax; cpuid" :::
                     "rax", "rbx", "rcx", "rdx", "memory");
    check(received == 8, 132);
    check(data[0] == 0x7766554433221100ull, 133);
    check(!(root[1] & A) && !(pdpt[0] & A) && !(pd[0] & A) &&
          !(pt[0] & (A | D)), 134);

    finish(0);
}
