// SPDX-License-Identifier: GPL-2.0
/*
 * misc.c
 *
 * This is a collection of several routines used to extract the kernel
 * which includes KASLR relocation, decompression, ELF parsing, and
 * relocation processing. Additionally included are the screen and serial
 * output functions and related debugging support functions.
 *
 * malloc by Hannu Savolainen 1993 and Matthias Urlichs 1994
 * puts by Nick Holloway 1993, better puts by Martin Mares 1995
 * High loaded stuff by Hans Lermen & Werner Almesberger, Feb. 1996
 */

#include "misc.h"
#include "error.h"
#include "../string.h"
#include "../voffset.h"
#include <asm/bootparam_utils.h>

/*
 * WARNING!!
 * This code is compiled with -fPIC and it is relocated dynamically at
 * run time, but no relocation processing is performed. This means that
 * it is not safe to place pointers in static structures.
 */

/* Macros used by the included decompressor code below. */
#define STATIC		static
/* Define an externally visible malloc()/free(). */
#define MALLOC_VISIBLE
#include <linux/decompress/mm.h>

/*
 * Provide definitions of memzero and memmove as some of the decompressors will
 * try to define their own functions if these are not defined as macros.
 */
#define memzero(s, n)	memset((s), 0, (n))
#ifndef memmove
#define memmove		memmove
/* Functions used by the included decompressor code below. */
void *memmove(void *dest, const void *src, size_t n);
#endif

/*
 * This is set up by the setup-routine at boot-time
 */
struct boot_params *boot_params_ptr;

struct port_io_ops pio_ops;

memptr free_mem_ptr;
memptr free_mem_end_ptr;
int spurious_nmi_count;

static char *vidmem;
static int vidport;

/* These might be accessed before .bss is cleared, so use .data instead. */
static int lines __section(".data");
static int cols __section(".data");

#ifdef CONFIG_KERNEL_GZIP
#include "../../../../lib/decompress_inflate.c"
#endif

#ifdef CONFIG_KERNEL_BZIP2
#include "../../../../lib/decompress_bunzip2.c"
#endif

#ifdef CONFIG_KERNEL_LZMA
#include "../../../../lib/decompress_unlzma.c"
#endif

#ifdef CONFIG_KERNEL_XZ
#include "../../../../lib/decompress_unxz.c"
#endif

#ifdef CONFIG_KERNEL_LZO
#include "../../../../lib/decompress_unlzo.c"
#endif

#ifdef CONFIG_KERNEL_LZ4
#include "../../../../lib/decompress_unlz4.c"
#endif

#ifdef CONFIG_KERNEL_ZSTD
#include "../../../../lib/decompress_unzstd.c"
#endif
/*
 * NOTE: When adding a new decompressor, please update the analysis in
 * ../header.S.
 */

static void scroll(void)
{
	int i;

	memmove(vidmem, vidmem + cols * 2, (size_t)(lines - 1) * cols * 2);
	for (i = (lines - 1) * cols * 2; i < lines * cols * 2; i += 2)
		vidmem[i] = ' ';
}

#define XMTRDY          0x20

#define TXR             0       /*  Transmit register (WRITE) */
#define LSR             5       /*  Line Status               */
static void serial_putchar(int ch)
{
	unsigned timeout = 0xffff;

	while ((inb(early_serial_base + LSR) & XMTRDY) == 0 && --timeout)
		cpu_relax();

	outb(ch, early_serial_base + TXR);
}

void __putstr(const char *s)
{
	int x, y, pos;
	char c;

	if (early_serial_base) {
		const char *str = s;
		while (*str) {
			if (*str == '\n')
				serial_putchar('\r');
			serial_putchar(*str++);
		}
	}

	if (lines == 0 || cols == 0)
		return;

	x = boot_params_ptr->screen_info.orig_x;
	y = boot_params_ptr->screen_info.orig_y;

	while ((c = *s++) != '\0') {
		if (c == '\n') {
			x = 0;
			if (++y >= lines) {
				scroll();
				y--;
			}
		} else {
			vidmem[(x + cols * y) * 2] = c;
			if (++x >= cols) {
				x = 0;
				if (++y >= lines) {
					scroll();
					y--;
				}
			}
		}
	}

	boot_params_ptr->screen_info.orig_x = x;
	boot_params_ptr->screen_info.orig_y = y;

	pos = (x + cols * y) * 2;	/* Update cursor position */
	outb(14, vidport);
	outb(0xff & (pos >> 9), vidport+1);
	outb(15, vidport);
	outb(0xff & (pos >> 1), vidport+1);
}

static noinline void __putnum(unsigned long value, unsigned int base,
			      int mindig)
{
	char buf[8*sizeof(value)+1];
	char *p;

	p = buf + sizeof(buf);
	*--p = '\0';

	while (mindig-- > 0 || value) {
		unsigned char digit = value % base;
		digit += (digit >= 10) ? ('a'-10) : '0';
		*--p = digit;

		value /= base;
	}

	__putstr(p);
}

void __puthex(unsigned long value)
{
	__putnum(value, 16, sizeof(value)*2);
}

void __putdec(unsigned long value)
{
	__putnum(value, 10, 1);
}

#ifdef CONFIG_X86_NEED_RELOCS
static void handle_relocations(void *output, unsigned long output_len,
			       unsigned long virt_addr)
{
	int *reloc;
	unsigned long delta, map, ptr;
	unsigned long min_addr = (unsigned long)output;
	unsigned long max_addr = min_addr + (VO___bss_start - VO__text);

	/*
	 * Calculate the delta between where vmlinux was linked to load
	 * and where it was actually loaded.
	 */
	delta = min_addr - LOAD_PHYSICAL_ADDR;

	/*
	 * The kernel contains a table of relocation addresses. Those
	 * addresses have the final load address of the kernel in virtual
	 * memory. We are currently working in the self map. So we need to
	 * create an adjustment for kernel memory addresses to the self map.
	 * This will involve subtracting out the base address of the kernel.
	 */
	map = delta - __START_KERNEL_map;

	/*
	 * 32-bit always performs relocations. 64-bit relocations are only
	 * needed if KASLR has chosen a different starting address offset
	 * from __START_KERNEL_map.
	 */
	if (IS_ENABLED(CONFIG_X86_64))
		delta = virt_addr - LOAD_PHYSICAL_ADDR;

	if (!delta) {
		debug_putstr("No relocation needed... ");
		return;
	}
	debug_putstr("Performing relocations... ");

	/*
	 * Process relocations: 32 bit relocations first then 64 bit after.
	 * Two sets of binary relocations are added to the end of the kernel
	 * before compression. Each relocation table entry is the kernel
	 * address of the location which needs to be updated stored as a
	 * 32-bit value which is sign extended to 64 bits.
	 *
	 * Format is:
	 *
	 * kernel bits...
	 * 0 - zero terminator for 64 bit relocations
	 * 64 bit relocation repeated
	 * 0 - zero terminator for 32 bit relocations
	 * 32 bit relocation repeated
	 *
	 * So we work backwards from the end of the decompressed image.
	 */
	for (reloc = output + output_len - sizeof(*reloc); *reloc; reloc--) {
		long extended = *reloc;
		extended += map;

		ptr = (unsigned long)extended;
		if (ptr < min_addr || ptr > max_addr)
			error("32-bit relocation outside of kernel!\n");

		*(uint32_t *)ptr += delta;
	}
#ifdef CONFIG_X86_64
	for (reloc--; *reloc; reloc--) {
		long extended = *reloc;
		extended += map;

		ptr = (unsigned long)extended;
		if (ptr < min_addr || ptr > max_addr)
			error("64-bit relocation outside of kernel!\n");

		*(uint64_t *)ptr += delta;
	}
#endif
}
#else
static inline void handle_relocations(void *output, unsigned long output_len,
				      unsigned long virt_addr)
{ }
#endif

static size_t parse_elf(void *output)
{
#ifdef CONFIG_X86_64
	Elf64_Ehdr ehdr;
	Elf64_Phdr *phdrs, *phdr;
#else
	Elf32_Ehdr ehdr;
	Elf32_Phdr *phdrs, *phdr;
#endif
	void *dest;
	int i;

	memcpy(&ehdr, output, sizeof(ehdr));
	if (ehdr.e_ident[EI_MAG0] != ELFMAG0 ||
	   ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
	   ehdr.e_ident[EI_MAG2] != ELFMAG2 ||
	   ehdr.e_ident[EI_MAG3] != ELFMAG3)
		error("Kernel is not a valid ELF file");

	debug_putstr("Parsing ELF... ");

	phdrs = malloc(sizeof(*phdrs) * ehdr.e_phnum);
	if (!phdrs)
		error("Failed to allocate space for phdrs");

	memcpy(phdrs, output + ehdr.e_phoff, sizeof(*phdrs) * ehdr.e_phnum);

	for (i = 0; i < ehdr.e_phnum; i++) {
		phdr = &phdrs[i];

		switch (phdr->p_type) {
		case PT_LOAD:
#ifdef CONFIG_X86_64
			if ((phdr->p_align % 0x200000) != 0)
				error("Alignment of LOAD segment isn't multiple of 2MB");
#endif
#ifdef CONFIG_RELOCATABLE
			dest = output;
			dest += (phdr->p_paddr - LOAD_PHYSICAL_ADDR);
#else
			dest = (void *)(phdr->p_paddr);
#endif
			memmove(dest, output + phdr->p_offset, phdr->p_filesz);
			break;
		default: /* Ignore other PT_* */ break;
		}
	}

	free(phdrs);

	return ehdr.e_entry - LOAD_PHYSICAL_ADDR;
}

const unsigned long kernel_text_size = VO___start_rodata - VO__text;
const unsigned long kernel_total_size = VO__end - VO__text;

static u8 boot_heap[BOOT_HEAP_SIZE] __aligned(4);

extern unsigned char input_data[];
extern unsigned int input_len, output_len;

unsigned long decompress_kernel(unsigned char *outbuf, unsigned long virt_addr,
				void (*error)(char *x))
{
	unsigned long entry;

	if (!free_mem_ptr) {
		free_mem_ptr     = (unsigned long)boot_heap;
		free_mem_end_ptr = (unsigned long)boot_heap + sizeof(boot_heap);
	}

	if (__decompress(input_data, input_len, NULL, NULL, outbuf, output_len,
			 NULL, error) < 0)
		return ULONG_MAX;

	entry = parse_elf(outbuf);
	handle_relocations(outbuf, output_len, virt_addr);

	return entry;
}

/*
 * Set the memory encryption xloadflag based on the mem_encrypt= command line
 * parameter, if provided.
 */
static void parse_mem_encrypt(struct setup_header *hdr)
{
	int on = cmdline_find_option_bool("mem_encrypt=on");
	int off = cmdline_find_option_bool("mem_encrypt=off");

	if (on > off)
		hdr->xloadflags |= XLF_MEM_ENCRYPTION;
}

static void early_sev_detect(void)
{
	/*
	 * Accessing video memory causes guest termination because
	 * the boot stage2 #VC handler of SEV-ES/SNP guests does not
	 * support MMIO handling and kexec -c adds screen_info to the
	 * boot parameters passed to the kexec kernel, which causes
	 * console output to be dumped to both video and serial.
	 */
	if (sev_status & MSR_AMD64_SEV_ES_ENABLED)
		lines = cols = 0;
}

/*
 * The compressed kernel image (ZO), has been moved so that its position
 * is against the end of the buffer used to hold the uncompressed kernel
 * image (VO) and the execution environment (.bss, .brk), which makes sure
 * there is room to do the in-place decompression. (See header.S for the
 * calculations.)
 *
 *                             |-----compressed kernel image------|
 *                             V                                  V
 * 0                       extract_offset                      +INIT_SIZE
 * |-----------|---------------|-------------------------|--------|
 *             |               |                         |        |
 *           VO__text      startup_32 of ZO          VO__end    ZO__end
 *             ^                                         ^
 *             |-------uncompressed kernel image---------|
 *
 */
asmlinkage __visible void *extract_kernel(void *rmode, unsigned char *output)
{
	unsigned long virt_addr = LOAD_PHYSICAL_ADDR;
	memptr heap = (memptr)boot_heap;
	unsigned long needed_size;
	size_t entry_offset;

	/* Retain x86 boot parameters pointer passed from startup_32/64. */
	boot_params_ptr = rmode;

	/* Clear flags intended for solely in-kernel use. */
	boot_params_ptr->hdr.loadflags &= ~KASLR_FLAG;

	parse_mem_encrypt(&boot_params_ptr->hdr);

	sanitize_boot_params(boot_params_ptr);

	if (boot_params_ptr->screen_info.orig_video_mode == 7) {
		vidmem = (char *) 0xb0000;
		vidport = 0x3b4;
	} else {
		vidmem = (char *) 0xb8000;
		vidport = 0x3d4;
	}

	lines = boot_params_ptr->screen_info.orig_video_lines;
	cols = boot_params_ptr->screen_info.orig_video_cols;

	init_default_io_ops();

	/*
	 * Detect TDX guest environment.
	 *
	 * It has to be done before console_init() in order to use
	 * paravirtualized port I/O operations if needed.
	 */
	early_tdx_detect();

	early_sev_detect();

	console_init();

	/*
	 * Save RSDP address for later use. Have this after console_init()
	 * so that early debugging output from the RSDP parsing code can be
	 * collected.
	 */
	boot_params_ptr->acpi_rsdp_addr = get_rsdp_addr();

	debug_putstr("early console in extract_kernel\n");

	free_mem_ptr     = heap;	/* Heap */
	free_mem_end_ptr = heap + BOOT_HEAP_SIZE;

	/*
	 * The memory hole needed for the kernel is the larger of either
	 * the entire decompressed kernel plus relocation table, or the
	 * entire decompressed kernel plus .bss and .brk sections.
	 *
	 * On X86_64, the memory is mapped with PMD pages. Round the
	 * size up so that the full extent of PMD pages mapped is
	 * included in the check against the valid memory table
	 * entries. This ensures the full mapped area is usable RAM
	 * and doesn't include any reserved areas.
	 */
	needed_size = max_t(unsigned long, output_len, kernel_total_size);
#ifdef CONFIG_X86_64
	needed_size = ALIGN(needed_size, MIN_KERNEL_ALIGN);
#endif

	/* Report initial kernel position details. */
	debug_putaddr(input_data);
	debug_putaddr(input_len);
	debug_putaddr(output);
	debug_putaddr(output_len);
	debug_putaddr(kernel_total_size);
	debug_putaddr(needed_size);

#ifdef CONFIG_X86_64
	/* Report address of 32-bit trampoline */
	debug_putaddr(trampoline_32bit);
#endif

	choose_random_location((unsigned long)input_data, input_len,
				(unsigned long *)&output,
				needed_size,
				&virt_addr);

	/* Validate memory location choices. */
	if ((unsigned long)output & (MIN_KERNEL_ALIGN - 1))
		error("Destination physical address inappropriately aligned");
	if (virt_addr & (MIN_KERNEL_ALIGN - 1))
		error("Destination virtual address inappropriately aligned");
#ifdef CONFIG_X86_64
	if (heap > 0x3fffffffffffUL)
		error("Destination address too large");
	if (virt_addr + needed_size > KERNEL_IMAGE_SIZE)
		error("Destination virtual address is beyond the kernel mapping area");
#else
	if (heap > ((-__PAGE_OFFSET-(128<<20)-1) & 0x7fffffff))
		error("Destination address too large");
#endif
#ifndef CONFIG_RELOCATABLE
	if (virt_addr != LOAD_PHYSICAL_ADDR)
		error("Destination virtual address changed when not relocatable");
#endif

	debug_putstr("\nDecompressing Linux... ");

	if (init_unaccepted_memory()) {
		debug_putstr("Accepting memory... ");
		accept_memory(__pa(output), needed_size);
	}

	entry_offset = decompress_kernel(output, virt_addr, error);

	debug_putstr("done.\nBooting the kernel (entry_offset: 0x");
	debug_puthex(entry_offset);
	debug_putstr(").\n");

	/* Disable exception handling before booting the kernel */
	cleanup_exception_handling();

	if (spurious_nmi_count) {
		error_putstr("Spurious early NMIs ignored: ");
		error_putdec(spurious_nmi_count);
		error_putstr("\n");
	}

	return output + entry_offset;
}
