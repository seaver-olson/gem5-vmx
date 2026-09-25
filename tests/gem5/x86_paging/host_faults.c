/* SPDX-License-Identifier: BSD-3-Clause */
/* Optional Linux/Intel differential check of page-fault access classification. */
#define _GNU_SOURCE
#include <cpuid.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

static sigjmp_buf recovery;
static volatile unsigned error;
static volatile uintptr_t fault_address;

static void handler(int signal, siginfo_t *info, void *context)
{
    (void)signal;
    ucontext_t *state = context;
    error = state->uc_mcontext.gregs[REG_ERR];
    fault_address = (uintptr_t)info->si_addr;
    siglongjmp(recovery, 1);
}

static int probe(void *page, int operation, unsigned access_bits)
{
    error = 0;
    fault_address = 0;
    if (!sigsetjmp(recovery, 1)) {
        if (operation == 0)
            __asm__ volatile("mov (%0), %%rax" :: "r"(page) : "rax", "memory");
        else if (operation == 1)
            __asm__ volatile("addq $1, (%0)" :: "r"(page) : "memory", "cc");
        else if (operation == 2)
            __asm__ volatile("lock addq $1, (%0)" :: "r"(page) : "memory", "cc");
        else
            ((void (*)(void))page)();
        return 1;
    }
    printf("operation=%d error=%#x\n", operation, error);
    /* P is deliberately excluded: Linux may use nonpresent PTEs to implement
     * PROT_NONE. Only the architectural U/S, W/R, and I/D classifications
     * are compared here. */
    return fault_address != (uintptr_t)page || (error & 0x16) != access_bits;
}

int main(void)
{
    unsigned a, b, c, d;
    char vendor[13] = {};
    __cpuid(0, a, b, c, d);
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    if (strcmp(vendor, "GenuineIntel")) {
        puts("SKIP: this differential test requires Intel hardware");
        return 77;
    }
    const size_t size = sysconf(_SC_PAGESIZE);
    void *page = mmap(NULL, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED)
        return 1;
    memset(page, 0xc3, size);
    struct sigaction action = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, NULL) || mprotect(page, size, PROT_NONE))
        return 1;
    int failed = probe(page, 0, 4);
    failed |= probe(page, 1, 6);
    failed |= probe(page, 2, 6);
    if (mprotect(page, size, PROT_READ))
        return 1;
    failed |= probe(page, 1, 6);
    failed |= probe(page, 2, 6);
    failed |= probe(page, 3, 20);
    munmap(page, size);
    puts(failed ? "FAIL" : "PASS");
    return failed;
}
