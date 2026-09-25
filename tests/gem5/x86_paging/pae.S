/* SPDX-License-Identifier: BSD-3-Clause */
.section .text.boot
.global _start
.code64
_start:
    cli
    lea stack_top(%rip), %rsp
    lgdt gdt_pointer(%rip)
    pushq $16
    lea compatibility(%rip), %rax
    push %rax
    lretq
.code32
compatibility:
    mov $24, %eax
    mov %eax, %ds
    mov %eax, %es
    mov %eax, %ss
    mov %cr0, %eax
    and $0x1fffffff, %eax
    mov %eax, %cr0
    mov $0xc0000080, %ecx
    rdmsr
    and $0xfffffeff, %eax
    or $0x800, %eax
    wrmsr
    mov $32, %eax
    mov %eax, %cr4
    xor %ecx, %ecx
1:  mov %ecx, %eax
    shl $21, %eax
    or $0x87, %eax
    mov %eax, identity(,%ecx,8)
    inc %ecx
    cmp $32, %ecx
    jne 1b
    movl $identity+1, pdpt
    movl $first+1, pdpt+8
    movl $second+1, pdpt+16
    movl $first+1, pdpt+24
    movl $0x400087, first
    movl $0x600087, second
    movl $0x12345678, 0x400000
    movl $0x76543210, 0x600000
    mov $pdpt, %eax
    mov %eax, %cr3
    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0
    mov $1, %esi
    cmpl $0x12345678, 0x40000000
    jne fail
    inc %esi
    cmpl $0x76543210, 0x80000000
    jne fail
    inc %esi
    cmpl $0x12345678, 0xc0000000
    jne fail

    /* Invalidate every cached translation, but retain the loaded PDPTE. */
    movl $second+1, pdpt+8
    /* A checkpoint/switch here must preserve registers whose backing memory
     * already differs. The Python harness resumes after this marker. */
    mfence
    xor %edi, %edi
    xor %esi, %esi
    .byte 0x0f, 0x04
    .word 0x21
    mov $3, %esi
    invlpg (0x40000000)
    inc %esi
    cmpl $0x12345678, 0x40000000
    jne fail
    /* WP does not reload PDPTEs. */
    mov %cr0, %eax
    xor $0x10000, %eax
    mov %eax, %cr0
    invlpg (0x40000000)
    inc %esi
    cmpl $0x12345678, 0x40000000
    jne fail
    /* Same-value CR3 writes must reload all four entries. */
    mov $pdpt, %eax
    mov %eax, %cr3
    inc %esi
    cmpl $0x76543210, 0x40000000
    jne fail
    /* PGE and PSE toggles reload, even though PAE stays active. */
    movl $first+1, pdpt+8
    mov %cr4, %eax
    xor $128, %eax
    mov %eax, %cr4
    inc %esi
    cmpl $0x12345678, 0x40000000
    jne fail
    movl $second+1, pdpt+8
    mov %cr4, %eax
    xor $16, %eax
    mov %eax, %cr4
    inc %esi
    cmpl $0x76543210, 0x40000000
    jne fail
    /* CD and NW transitions also reload. */
    movl $first+1, pdpt+8
    mov %cr0, %eax
    or $0x40000000, %eax
    mov %eax, %cr0
    inc %esi
    cmpl $0x12345678, 0x40000000
    jne fail
    movl $second+1, pdpt+8
    mov %cr0, %eax
    or $0x20000000, %eax
    mov %eax, %cr0
    inc %esi
    cmpl $0x76543210, 0x40000000
    jne fail
    mov %cr0, %eax
    and $0x9fffffff, %eax
    mov %eax, %cr0

    /* Present PDPTE PWT/PCD and ignored 11:9 are legal. A nonpresent
     * entry may contain reserved encodings, including NX with NXE set. */
    movl $0xfffffffe, pdpt+24
    movl $0xffffffff, pdpt+28
    movl $first+0xe19, pdpt+8
    mov $pdpt, %eax
    mov %eax, %cr3
    inc %esi
    cmpl $0x12345678, 0x40000000
    jne fail

    /* Enter PAE through CR4 while paging remains enabled. The unused
     * upper half of the identity PD also acts as a legacy 4 KiB PT. */
    mov %cr0, %eax
    and $0x7ffeffff, %eax
    mov %eax, %cr0
    xor %eax, %eax
    mov %eax, %cr4
    mov $256, %ecx
1:  mov %ecx, %eax
    shl $12, %eax
    or $1, %eax
    mov %eax, identity(,%ecx,4)
    inc %ecx
    cmp $512, %ecx
    jne 1b
    movl $first+1, pdpt+8
    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0
    movl $second+1, pdpt+8
    /* Legacy paging set PDE.A in the shared root word. It is reserved
     * when that same word becomes a PAE PDPTE. The code/data TLBs are warm. */
    andl $~32, pdpt
    mov $32, %eax
    mov %eax, %cr4
    inc %esi
    cmpl $0x76543210, 0x40000000
    jne fail
    xor %esi, %esi
fail:
    xor %edi, %edi
    .byte 0x0f, 0x04
    .word 0x22
    ud2
.section .rodata
.balign 8
gdt:
    .quad 0, 0x00af9a000000ffff, 0x00cf9a000000ffff, 0x00cf92000000ffff
gdt_pointer:
    .word 31
    .quad gdt
.section .bss
.balign 32
pdpt: .skip 32
.balign 4096
identity: .skip 4096
first: .skip 4096
second: .skip 4096
.balign 16
stack: .skip 16384
stack_top:
.section .note.GNU-stack,"",@progbits
