CC ?= gcc
CFLAGS = -O2 -g -ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mgeneral-regs-only -Wall -Wextra -Werror

.PHONY: all
all: vmx_operands.elf vmresume.elf paging.elf legacy.elf pae.elf gdb.elf se-context-1 se-context-2

se-context-%: se_context.S
	$(CC) -nostdlib -static -no-pie -DIDENTITY=$* -o $@ $<

paging.elf: entry.S paging.c link.ld
	$(CC) $(CFLAGS) -nostdlib -static -no-pie -Wl,-T,link.ld -o $@ entry.S paging.c

legacy.elf: legacy.S link.ld
	$(CC) $(CFLAGS) -nostdlib -static -no-pie -Wl,-T,link.ld -o $@ legacy.S

pae.elf: pae.S link.ld
	$(CC) $(CFLAGS) -nostdlib -static -no-pie -Wl,-T,link.ld -o $@ pae.S

gdb.elf: gdb.S link.ld
	$(CC) $(CFLAGS) -nostdlib -static -no-pie -Wl,-T,link.ld -o $@ gdb.S

host_faults: host_faults.c
	$(CC) -O2 -Wall -Wextra -Werror -o $@ $<

.PHONY: clean
clean:
	$(RM) vmx_operands.elf vmresume.elf paging.elf legacy.elf pae.elf gdb.elf host_faults se-context-1 se-context-2

vmx_operands.elf: vmx_operands.S vmx_operands.c entry.S link.ld
	$(CC) $(CFLAGS) -nostdlib -static -no-pie -Wl,-T,link.ld -o $@ vmx_operands.S vmx_operands.c

vmresume.elf: vmresume.S vmresume.c vmx_operands.S entry.S link.ld
	$(CC) $(CFLAGS) -nostdlib -static -no-pie -Wl,-T,link.ld -o $@ vmresume.S vmresume.c
