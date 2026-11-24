/* Copyright (c) 2026 Seaver Olson, Loyola University Chicago
 * SPDX-License-Identifier: BSD-3-Clause
 */
// Use the same VMCS bootstrap and fault handlers as the VM-entry suite.
#define TRANSLATION_STUDY
#include "vmresume.c"

#define PAGE 4096
#define ALIGNED __attribute__((aligned(PAGE)))
typedef uint64_t Table[512];
static Table guest_root ALIGNED, guest_pdpt ALIGNED, guest_pd ALIGNED;
static Table guest_pt[2] ALIGNED, other_guest_pd ALIGNED;
Table study_alias_pt ALIGNED;
static Table ept_root[2] ALIGNED, ept_pdpt[2] ALIGNED, ept_pd[2] ALIGNED;
static Table ept_low[2][2] ALIGNED, ept_data_pt[2] ALIGNED;
static uint8_t data[4][PAGE] ALIGNED, nodes[128][PAGE] ALIGNED;
uint8_t study_pressure[8][PAGE] ALIGNED;
uint64_t study_first;
volatile uint64_t study_value, study_before;
static uint64_t options[2], fingerprint, checksum;
extern void study_split(void), study_word(void), study_alias(void);
extern void study_loop(void), study_loop_guest(void), study_root_read(void);

static void
write_results(uint64_t *values, unsigned count)
{
    uint64_t written;
    __asm__ volatile(".byte 0x0f, 0x04; .word 0x4f"
                     : "=a"(written)
                     : "D"(values), "S"(count * 8ull), "d"(0ull),
                       "c"("study.bin")
                     : "memory");
    check(written == count * 8, 900);
}

static void
build_tables(void)
{
    guest_root[0] = (uint64_t)guest_pdpt | 0x27;
    guest_pdpt[0] = (uint64_t)guest_pd | 0x27;
    for (unsigned j = 0; j < 2; ++j) {
        guest_pd[j] = (uint64_t)guest_pt[j] | 0x27;
        for (unsigned i = 0; i < 512; ++i) {
            guest_pt[j][i] = (512ull * j + i) * PAGE | 0x67;
        }
    }
    guest_pd[4] = 0x8000e7; // One 2 MiB guest leaf.
    guest_pd[6] = (uint64_t)study_alias_pt | 0x27;
    study_alias_pt[0] = 0x800067;
    for (unsigned i = 0; i < 512; ++i) {
        other_guest_pd[i] = guest_pd[i];
    }
    other_guest_pd[4] = 0xa000e7;
    for (unsigned root = 0; root < 2; ++root) {
        ept_root[root][0] = (uint64_t)ept_pdpt[root] | 0x107;
        ept_pdpt[root][0] = (uint64_t)ept_pd[root] | 0x107;
        for (unsigned j = 0; j < 2; ++j) {
            ept_pd[root][j] = (uint64_t)ept_low[root][j] | 0x107;
            for (unsigned i = 0; i < 512; ++i) {
                ept_low[root][j][i] = (512ull * j + i) * PAGE | 0x337;
            }
        }
        // Both EPT roots have identical final data mappings.
        ept_pd[root][4] = (uint64_t)ept_data_pt[0] | 0x107;
        ept_pd[root][5] = (uint64_t)ept_data_pt[1] | 0x107;
    }
    // Only the backing of the guest page directory changes on re-entry.
    unsigned pd_page = (uint64_t)guest_pd / PAGE;
    check(pd_page < 1024, 901);
    ept_low[1][pd_page / 512][pd_page % 512] =
        (uint64_t)other_guest_pd | 0x337;
    ept_data_pt[0][0] = (uint64_t)data[0] | 0x337;
    ept_data_pt[0][1] = (uint64_t)data[2] | 0x337;
    ept_data_pt[1][0] = (uint64_t)data[3] | 0x337;
    data[0][0] = 0x19;
    data[0][PAGE - 1] = 0xa5;
    data[1][0] = 0xee; // Poison a contiguous host read.
    data[2][0] = 0x3c;
    data[3][PAGE - 1] = 0x76;
}

static uint64_t
node(unsigned page)
{
    return (uint64_t)&nodes[page][64 * ((page / 2) % 64)];
}

static void
build_cycle(unsigned pages)
{
    unsigned order[128];
    uint32_t seed = 1;
    for (unsigned i = 0; i < pages; ++i) {
        order[i] = i;
    }
    for (unsigned i = pages - 1; i; --i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        unsigned j = seed % (i + 1);
        unsigned tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
    fingerprint = 14695981039346656037ull;
    for (unsigned i = 0; i < pages; ++i) {
        *(uint64_t *)node(order[i]) = node(order[(i + 1) % pages]);
        fingerprint = (fingerprint ^ order[i]) * 1099511628211ull;
    }
    study_first = node(order[0]);
    // Verify the actual links during the common warm-up, outside the ROI.
    uint64_t current = study_first;
    for (unsigned i = 0; i < 4096; ++i) {
        current = *(volatile uint64_t *)current;
        check(current == node(order[(i + 1) % pages]), 902);
        checksum = ((checksum << 7) | (checksum >> 57)) + current;
    }
}

static void
bench_result(void)
{
    check(study_value == checksum, 903);
    uint64_t values[] = {options[1], 4096, fingerprint, study_value};
    write_results(values, 4);
}

static void
study_prepare(void)
{
    uint64_t length, cr0;
    __asm__ volatile(".byte 0x0f, 0x04; .word 0x50"
                     : "=a"(length)
                     : "D"(options), "S"(sizeof(options)), "d"(0ull)
                     : "memory");
    check(length == sizeof(options), 904);
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(3ull << 29);
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");
    build_tables();
    if (options[0]) {
        check(options[1] == 32 || options[1] == 128, 905);
        build_cycle(options[1]);
    }
    if (options[0] == 1) {
        __asm__ volatile("mov %0, %%cr3" ::"r"(guest_root) : "memory");
        study_loop();
        bench_result();
        __asm__ volatile(".byte 0x0f, 0x04; .word 0x22" ::"D"(0ull), "S"(0ull)
                         : "memory");
    }
}

static void
run_payload(void (*ip)(void), unsigned root, int resume, unsigned reason)
{
    field(0x201a, (uint64_t)ept_root[root] | 0x5e);
    field(0x681e, (uint64_t)ip);
    check(!enter_guest(resume) && read_field(0x4402) == reason, 906);
}

static void
study_guest(void)
{
    field(0x4002, controls(0x48e, 1u << 31));
    field(0x401e, 2);
    field(0x6802, (uint64_t)guest_root);
    if (options[0] == 2) {
        run_payload(study_loop_guest, 0, 0, 18);
        bench_result();
        return;
    }
    check(options[0] == 0, 907);
    run_payload(study_split, 0, 0, 18);
    uint64_t values[5] = {study_value};
    check(values[0] == 0x3ca5, 908);
    ept_data_pt[0][1] &= ~7ull;
    run_payload(study_split, 0, 1, 48);
    values[1] = read_field(0x2400);
    values[2] = read_field(0x6400);
    check(values[1] == 0x801000 && values[2] == 0x181 &&
              read_field(0x681e) == (uint64_t)study_word &&
              read_field(0x640a) == 0x801000,
          909);
    ept_data_pt[0][1] |= 7;
    run_payload(study_alias, 0, 1, 18);
    values[3] = study_value;
    check(study_before == 0x19 && values[3] == 0x3c, 910);
    run_payload(study_root_read, 0, 1, 18);
    check(study_value == 0xa5, 911);
    run_payload(study_root_read, 1, 1, 18);
    values[4] = study_value;
    check(values[4] == 0x76, 912);
    write_results(values, 5);
}
