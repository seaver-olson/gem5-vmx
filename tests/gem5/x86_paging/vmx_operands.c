#include <stdint.h>
#include <stddef.h>

#define VA (1ull << 39)
#define NONE (~0ull)
#define FLAGS 0x8d5ull
#define RIP 0x681eull
static uint64_t root[512] __attribute__((aligned(4096)));
static uint64_t pdpt[512] __attribute__((aligned(4096)));
static uint64_t pd[512] __attribute__((aligned(4096)));
static uint64_t pt[512] __attribute__((aligned(4096)));
static uint64_t first[512] __attribute__((aligned(4096)));
static uint64_t second[512] __attribute__((aligned(4096)));
static uint32_t on_region[1024] __attribute__((aligned(4096)));
static uint32_t vmcs_region[1024] __attribute__((aligned(4096)));
static uint64_t idt[512] __attribute__((aligned(16)));
volatile uint64_t resume_ip, fault_error, fault_vector, rip_pointer;
extern void fault_entry(void), gp_entry(void), ss_entry(void), ud_entry(void);
typedef uint64_t (*probe_t)(uint64_t, uint64_t, uint64_t);
#define PROBE(name) extern uint64_t name(uint64_t, uint64_t, uint64_t)
PROBE(op_vmxon); PROBE(op_vmclear); PROBE(op_vmptrld); PROBE(op_vmptrst);
PROBE(op_vmwrite_mem); PROBE(op_vmwrite_reg); PROBE(op_vmread_mem);
PROBE(op_vmxoff); PROBE(op_vmxon_sib); PROBE(op_vmptrst_rip);
PROBE(op_vmread_rip); PROBE(op_vmwrite_rip);
extern uint64_t read_field(uint64_t);

static __attribute__((noreturn)) void finish(uint64_t code)
{
    if (code) {
        static const char name[] = "failure.bin";
        uint64_t detail[] = {code, fault_vector, fault_error};
        uint64_t ignored;
        __asm__ volatile(".byte 0x0f, 0x04; .word 0x4f" : "=a"(ignored)
            : "D"((uint64_t)detail), "S"(sizeof(detail)), "d"(0ull),
              "c"((uint64_t)name) : "memory");
    }
    __asm__ volatile(".byte 0x0f, 0x04; .word 0x22" :: "D"(0ull), "S"(code));
    for (;;) __asm__ volatile("hlt");
}
static void check(int ok, unsigned code) { if (!ok) finish(code); }
static uint64_t msr(uint32_t reg)
{
    uint32_t a, d;
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(reg));
    return a | ((uint64_t)d << 32);
}
static void flush(void)
{ __asm__ volatile("mov %0, %%cr3" :: "r"((uint64_t)root) : "memory"); }
static void probe(probe_t op, uint64_t address, uint64_t field, uint64_t value,
                  unsigned vector, uint64_t error, uint64_t flags, unsigned code)
{
    fault_vector = 0; fault_error = NONE;
    uint64_t result = op(address, field, value);
    check(fault_vector == vector && fault_error == error &&
          (result & FLAGS) == flags, code);
}
#define SUCCESS(op,addr,field,value,code) probe(op,addr,field,value,0,NONE,0,code)
#define STATUS(op,addr,field,value,status,code) probe(op,addr,field,value,0,NONE,status,code)
#define FAULT(op,addr,field,vec,err,code) probe(op,addr,field,0,vec,err,FLAGS,code)

void paging_test(void)
{
    uint64_t old_root, cr0, cr4;
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_root));
    root[0] = ((uint64_t *)old_root)[0];
    root[1] = (uint64_t)pdpt | 7;
    pdpt[0] = (uint64_t)pd | 7;
    pd[0] = (uint64_t)pt | 7;
    pt[0] = (uint64_t)first | 7;
    pt[1] = (uint64_t)second | 7;
    flush();
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    for (unsigned v = 0; v < 256; ++v) {
        uint64_t target = (uint64_t)(v == 6 ? ud_entry :
            v == 12 ? ss_entry : v == 13 ? gp_entry : fault_entry);
        idt[v*2] = (target & 0xffff) | ((uint64_t)cs << 16) |
            (0x8eull << 40) | ((target & 0xffff0000) << 32);
        idt[v*2+1] = target >> 32;
    }
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; }
        idtr = {sizeof(idt)-1, (uint64_t)idt};
    __asm__ volatile("lidt %0" :: "m"(idtr));
    const uint64_t hole = VA + 8192;
    probe_t operations[] = {op_vmxon, op_vmclear, op_vmptrld, op_vmptrst,
                           op_vmread_mem, op_vmwrite_mem};
    for (unsigned i = 0; i < 6; ++i)
        FAULT(operations[i], hole, RIP, 6, 0, 10+i);
    __asm__ volatile("mov %%cr0, %0; mov %%cr4, %1" : "=r"(cr0), "=r"(cr4));
    cr0 = (cr0 | msr(0x486) | (1ull << 16)) & msr(0x487);
    cr4 = (cr4 | msr(0x488)) & msr(0x489);
    __asm__ volatile("mov %0, %%cr0; mov %1, %%cr4" :: "r"(cr0), "r"(cr4) : "memory");
    on_region[0] = vmcs_region[0] = (uint32_t)msr(0x480);
    volatile uint64_t *split = (volatile uint64_t *)(VA + 4092);
    *split = (uint64_t)on_region;
    FAULT(op_vmxon, hole, 0, 14, 0, 20);
    SUCCESS(op_vmxon_sib, (uint64_t)split, 0, 0, 21);
    SUCCESS(op_vmptrst_rip, 0, 0, 0, 22);
    check(rip_pointer == NONE, 23);
    STATUS(op_vmread_mem, hole, RIP, 0, 1, 24);
    STATUS(op_vmwrite_mem, hole, RIP, 0, 1, 25);
    *split = (uint64_t)vmcs_region;
    SUCCESS(op_vmclear, (uint64_t)split, 0, 0, 26);
    SUCCESS(op_vmptrld, (uint64_t)split, 0, 0, 27);
    STATUS(op_vmxon, hole, 0, 0, 0x40, 28); /* root precheck before operand */
    check(read_field(0x4400) == 15, 29);
    const uint64_t value = 0x1122334455667788ull;
    SUCCESS(op_vmwrite_reg, 0, RIP, value, 30);
    check(read_field(RIP) == value, 31);
    SUCCESS(op_vmread_mem, (uint64_t)split, RIP, 0, 32);
    check(*split == value, 33);
    SUCCESS(op_vmwrite_mem, (uint64_t)split, RIP, 0, 34);
    check(read_field(RIP) == value, 35);
    SUCCESS(op_vmread_rip, 0, RIP, 0, 36);
    check(rip_pointer == value, 37);
    rip_pointer = value + 19;
    SUCCESS(op_vmwrite_rip, 0, RIP, 0, 38);
    check(read_field(RIP) == value + 19, 39);
    STATUS(op_vmread_mem, hole, 0xffff8000, 0, 0x40, 40);
    check(read_field(0x4400) == 12, 41);
    FAULT(op_vmwrite_mem, hole, 0xffff8000, 14, 0, 42);
    STATUS(op_vmwrite_reg, 0, 0x4400, 7, 0x40, 43);
    check(read_field(0x4400) == 13, 44);
    FAULT(op_vmptrst, hole, 0, 14, 2, 45);
    FAULT(op_vmread_mem, hole, RIP, 14, 2, 46);
    FAULT(op_vmread_mem, 1ull << 47, RIP, 13, 0, 47);
    *split = (uint64_t)vmcs_region;
    pt[1] = 0; flush();
    FAULT(op_vmwrite_mem, (uint64_t)split, RIP, 14, 0, 50);
    check(read_field(RIP) == value + 19, 51);
    FAULT(op_vmptrld, (uint64_t)split, 0, 14, 0, 52);
    uint32_t before = *(volatile uint32_t *)split;
    FAULT(op_vmread_mem, (uint64_t)split, RIP, 14, 2, 53);
    check(*(volatile uint32_t *)split == before, 54);
    SUCCESS(op_vmptrst_rip, 0, 0, 0, 55);
    check(rip_pointer == (uint64_t)vmcs_region, 56);
    pt[1] = (uint64_t)second | 7; flush();
    *split = (uint64_t)vmcs_region;
    SUCCESS(op_vmclear, (uint64_t)split, 0, 0, 57);
    SUCCESS(op_vmptrst_rip, 0, 0, 0, 58);
    check(rip_pointer == NONE, 59);
    SUCCESS(op_vmxoff, 0, 0, 0, 60);
    finish(0);
}
