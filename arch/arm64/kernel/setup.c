// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/kernel/setup.c
 *
 * Copyright (C) 1995-2001 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/acpi.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/initrd.h>
#include <linux/console.h>
#include <linux/cache.h>
#include <linux/screen_info.h>
#include <linux/init.h>
#include <linux/kexec.h>
#include <linux/root_dev.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/smp.h>
#include <linux/fs.h>
#include <linux/panic_notifier.h>
#include <linux/proc_fs.h>
#include <linux/memblock.h>
#include <linux/of_fdt.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/workqueue.h>
#include <linux/io.h>
#include <linux/kmsg_dump.h>
#include <linux/kthread.h>
#include <linux/efi.h>
#include <linux/psci.h>
#include <linux/sched/task.h>
#include <linux/scs.h>
#include <linux/mm.h>

#include <asm/acpi.h>
#include <asm/fixmap.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/daifflags.h>
#include <asm/elf.h>
#include <asm/cpufeature.h>
#include <asm/cpu_ops.h>
#include <asm/kasan.h>
#include <asm/numa.h>
#include <asm/scs.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/smp_plat.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/traps.h>
#include <asm/efi.h>
#include <asm/xen/hypervisor.h>
#include <asm/mmu_context.h>

static int num_standard_resources;
static struct resource *standard_resources;

phys_addr_t __fdt_pointer __initdata;
u64 mmu_enabled_at_boot __initdata;

/*
 * Standard memory resources
 */
static struct resource mem_res[] = {
	{
		.name = "Kernel code",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_SYSTEM_RAM
	},
	{
		.name = "Kernel data",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_SYSTEM_RAM
	}
};

#define kernel_code mem_res[0]
#define kernel_data mem_res[1]

/*
 * The recorded values of x0 .. x3 upon kernel entry.
 */
u64 __cacheline_aligned boot_args[4];

void __init smp_setup_processor_id(void)
{
	u64 mpidr = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;
	set_cpu_logical_map(0, mpidr);

	pr_info("Booting Linux on physical CPU 0x%010lx [0x%08x]\n",
		(unsigned long)mpidr, read_cpuid_id());
}

bool arch_match_cpu_phys_id(int cpu, u64 phys_id)
{
	return phys_id == cpu_logical_map(cpu);
}

struct mpidr_hash mpidr_hash;
/**
 * smp_build_mpidr_hash - Pre-compute shifts required at each affinity
 *			  level in order to build a linear index from an
 *			  MPIDR value. Resulting algorithm is a collision
 *			  free hash carried out through shifting and ORing
 */
static void __init smp_build_mpidr_hash(void)
{
	u32 i, affinity, fs[4], bits[4], ls;
	u64 mask = 0;
	/*
	 * Pre-scan the list of MPIDRS and filter out bits that do
	 * not contribute to affinity levels, ie they never toggle.
	 */
	for_each_possible_cpu(i)
		mask |= (cpu_logical_map(i) ^ cpu_logical_map(0));
	pr_debug("mask of set bits %#llx\n", mask);
	/*
	 * Find and stash the last and first bit set at all affinity levels to
	 * check how many bits are required to represent them.
	 */
	for (i = 0; i < 4; i++) {
		affinity = MPIDR_AFFINITY_LEVEL(mask, i);
		/*
		 * Find the MSB bit and LSB bits position
		 * to determine how many bits are required
		 * to express the affinity level.
		 */
		ls = fls(affinity);
		fs[i] = affinity ? ffs(affinity) - 1 : 0;
		bits[i] = ls - fs[i];
	}
	/*
	 * An index can be created from the MPIDR_EL1 by isolating the
	 * significant bits at each affinity level and by shifting
	 * them in order to compress the 32 bits values space to a
	 * compressed set of values. This is equivalent to hashing
	 * the MPIDR_EL1 through shifting and ORing. It is a collision free
	 * hash though not minimal since some levels might contain a number
	 * of CPUs that is not an exact power of 2 and their bit
	 * representation might contain holes, eg MPIDR_EL1[7:0] = {0x2, 0x80}.
	 */
	mpidr_hash.shift_aff[0] = MPIDR_LEVEL_SHIFT(0) + fs[0];
	mpidr_hash.shift_aff[1] = MPIDR_LEVEL_SHIFT(1) + fs[1] - bits[0];
	mpidr_hash.shift_aff[2] = MPIDR_LEVEL_SHIFT(2) + fs[2] -
						(bits[1] + bits[0]);
	mpidr_hash.shift_aff[3] = MPIDR_LEVEL_SHIFT(3) +
				  fs[3] - (bits[2] + bits[1] + bits[0]);
	mpidr_hash.mask = mask;
	mpidr_hash.bits = bits[3] + bits[2] + bits[1] + bits[0];
	pr_debug("MPIDR hash: aff0[%u] aff1[%u] aff2[%u] aff3[%u] mask[%#llx] bits[%u]\n",
		mpidr_hash.shift_aff[0],
		mpidr_hash.shift_aff[1],
		mpidr_hash.shift_aff[2],
		mpidr_hash.shift_aff[3],
		mpidr_hash.mask,
		mpidr_hash.bits);
	/*
	 * 4x is an arbitrary value used to warn on a hash table much bigger
	 * than expected on most systems.
	 */
	if (mpidr_hash_size() > 4 * num_possible_cpus())
		pr_warn("Large number of MPIDR hash buckets detected\n");
}

static void __init setup_machine_fdt(phys_addr_t dt_phys)
{
	int size;
	void *dt_virt = fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL);
	const char *name;

	if (dt_virt)
		memblock_reserve(dt_phys, size);

	/*
	 * dt_virt is a fixmap address, hence __pa(dt_virt) can't be used.
	 * Pass dt_phys directly.
	 */
	if (!early_init_dt_scan(dt_virt, dt_phys)) {
		pr_crit("\n"
			"Error: invalid device tree blob at physical address %pa (virtual address 0x%px)\n"
			"The dtb must be 8-byte aligned and must not exceed 2 MB in size\n"
			"\nPlease check your bootloader.",
			&dt_phys, dt_virt);

		/*
		 * Note that in this _really_ early stage we cannot even BUG()
		 * or oops, so the least terrible thing to do is cpu_relax(),
		 * or else we could end-up printing non-initialized data, etc.
		 */
		while (true)
			cpu_relax();
	}

	/* Early fixups are done, map the FDT as read-only now */
	fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL_RO);

	name = of_flat_dt_get_machine_name();
	if (!name)
		return;

	pr_info("Machine model: %s\n", name);
	dump_stack_set_arch_desc("%s (DT)", name);
}

static void __init request_standard_resources(void)
{
	struct memblock_region *region;
	struct resource *res;
	unsigned long i = 0;
	size_t res_size;

	kernel_code.start   = __pa_symbol(_stext);
	kernel_code.end     = __pa_symbol(__init_begin - 1);
	kernel_data.start   = __pa_symbol(_sdata);
	kernel_data.end     = __pa_symbol(_end - 1);
	insert_resource(&iomem_resource, &kernel_code);
	insert_resource(&iomem_resource, &kernel_data);

	num_standard_resources = memblock.memory.cnt;
	res_size = num_standard_resources * sizeof(*standard_resources);
	standard_resources = memblock_alloc(res_size, SMP_CACHE_BYTES);
	if (!standard_resources)
		panic("%s: Failed to allocate %zu bytes\n", __func__, res_size);

	for_each_mem_region(region) {
		res = &standard_resources[i++];
		if (memblock_is_nomap(region)) {
			res->name  = "reserved";
			res->flags = IORESOURCE_MEM;
			res->start = __pfn_to_phys(memblock_region_reserved_base_pfn(region));
			res->end = __pfn_to_phys(memblock_region_reserved_end_pfn(region)) - 1;
		} else {
			res->name  = "System RAM";
			res->flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;
			res->start = __pfn_to_phys(memblock_region_memory_base_pfn(region));
			res->end = __pfn_to_phys(memblock_region_memory_end_pfn(region)) - 1;
		}

		insert_resource(&iomem_resource, res);
	}
}

static int __init reserve_memblock_reserved_regions(void)
{
	u64 i, j;

	for (i = 0; i < num_standard_resources; ++i) {
		struct resource *mem = &standard_resources[i];
		phys_addr_t r_start, r_end, mem_size = resource_size(mem);

		if (!memblock_is_region_reserved(mem->start, mem_size))
			continue;

		for_each_reserved_mem_range(j, &r_start, &r_end) {
			resource_size_t start, end;

			start = max(PFN_PHYS(PFN_DOWN(r_start)), mem->start);
			end = min(PFN_PHYS(PFN_UP(r_end)) - 1, mem->end);

			if (start > mem->end || end < mem->start)
				continue;

			reserve_region_with_split(mem, start, end, "reserved");
		}
	}

	return 0;
}
arch_initcall(reserve_memblock_reserved_regions);

u64 __cpu_logical_map[NR_CPUS] = { [0 ... NR_CPUS-1] = INVALID_HWID };

u64 cpu_logical_map(unsigned int cpu)
{
	return __cpu_logical_map[cpu];
}

#include <linux/font.h>

void __init __no_sanitize_address setup_arch(char **cmdline_p)
{
	setup_initial_init_mm(_stext, _etext, _edata, _end);

	*cmdline_p = boot_command_line;

	kaslr_init();

	early_fixmap_init();
	early_ioremap_init();

	{
		void __iomem *wdt = early_ioremap(0x10007000, 0x100);
		if (wdt) {
			/* PMOSWDT: Explicitly disarm hardware RGU watchdog so postmarketOS
			 * runs continuously without tripping reset.
			 */
			iowrite32(0x1971, wdt + 0x08);              /* RESTART key */
			iowrite32(0x22000000, wdt + 0x00);          /* MODE_KEY (EN bit 0 = DISABLED) */
			iowrite32(0x1971, wdt + 0x08);              /* RESTART key */

			early_iounmap(wdt, 0x100);
			pr_info("MediaTek topRGU MASTER WATCHDOG DISARMED for continuous operation\n");
		}
	}

	setup_machine_fdt(__fdt_pointer);

	/* Reserve BOE LCD Framebuffer (0xfe2b0000) so the kernel does not
	 * reuse it; the on-screen log is provided by fbtext exclusively.
	 * screen_info is deliberately NOT populated: it made sysfb create
	 * a simple-framebuffer that fbcon (console=tty0) latched onto,
	 * painting unfiltered kmsg over our console. */
	memblock_reserve(0xfe2b0000, 2000 * 4864);

	/* MMSYS Display Clock Keepalive (from u-boot-mtk) */
	{
		void __iomem *mmsys = early_ioremap(0x14000000, 0x1000);
		if (mmsys) {
			u32 sta0 = readl(mmsys + 0x100);
			u32 sta1 = readl(mmsys + 0x110);
			writel(sta0, mmsys + 0x104);
			writel(sta1, mmsys + 0x114);
			early_iounmap(mmsys, 0x1000);
			pr_info("=== MMSYS keepalive: MM0_STA=0x%08x MM1_STA=0x%08x ===\n", sta0, sta1);
		}
	}

	/* Repoint DISP_RDMA0 at our framebuffer exclusively: this drops the
	 * bootloader's composited logo/orange-state overlay layer, leaving
	 * only our own scanout for fbtext/fbcon. */
	{
		void __iomem *rdma = early_ioremap(0x14007000, 0x1000);
		void __iomem *mutex = early_ioremap(0x14001000, 0x1000);
		if (rdma) {
			u32 cur_addr = readl(rdma + 0xf00);
			u32 cur_pitch = readl(rdma + 0x02c);
			pr_info("=== DISP_RDMA0 status: START_ADDR=0x%08x PITCH=%u ===\n", cur_addr, cur_pitch);
			writel(0xfe2b0000, rdma + 0xf00);
			writel(4864, rdma + 0x02c);
			writel(0x20, rdma + 0x024);
			writel(1200, rdma + 0x014);
			writel(2000, rdma + 0x018);
			writel(0x3, rdma + 0x010);
			dmb(sy);
			early_iounmap(rdma, 0x1000);
		}
		if (mutex) {
			writel(1, mutex + 0x20);
			writel(1, mutex + 0x24);
			dmb(sy);
			early_iounmap(mutex, 0x1000);
			pr_info("=== DISP_MUTEX0 Trigger executed at 0x14001000 ===\n");
		}
	}

	/* Read-only census of the bootloader's leftover overlay layers.
	 * Layer enables live at 0x840+0x20*n, their fetch addresses at
	 * 0xf40+4*n; anything still enabled besides our framebuffer is the
	 * source of the ghost orange-state frame composited over fbtext. */
	{
		void __iomem *ovl = early_ioremap(0x14002000, 0x1000);
		if (ovl) {
			pr_info("=== OVL0 EN=0x%08x RST=0x%08x DP=0x%08x/%08x CON=0x%08x ===\n",
				readl(ovl + 0x800), readl(ovl + 0x814),
				readl(ovl + 0x024), readl(ovl + 0x030),
				readl(ovl + 0x040));
			pr_info("=== OVL0 L0: con=%08x addr=%08x | L1: con=%08x addr=%08x ===\n",
				readl(ovl + 0x840), readl(ovl + 0xf40),
				readl(ovl + 0x860), readl(ovl + 0xf44));
			pr_info("=== OVL0 L2: con=%08x addr=%08x | L3: con=%08x addr=%08x ===\n",
				readl(ovl + 0x880), readl(ovl + 0xf48),
				readl(ovl + 0x8a0), readl(ovl + 0xf4c));
			early_iounmap(ovl, 0x1000);
		}
	}

	/*
	 * Initialise the static keys early as they may be enabled by the
	 * cpufeature code and early parameters.
	 */
	jump_label_init();
	parse_early_param();

	dynamic_scs_init();

	/*
	 * The primary CPU enters the kernel with all DAIF exceptions masked.
	 *
	 * We must unmask Debug and SError before preemption or scheduling is
	 * possible to ensure that these are consistently unmasked across
	 * threads, and we want to unmask SError as soon as possible after
	 * initializing earlycon so that we can report any SErrors immediately.
	 *
	 * IRQ and FIQ will be unmasked after the root irqchip has been
	 * detected and initialized.
	 */
	local_daif_restore(DAIF_PROCCTX_NOIRQ);

	/*
	 * TTBR0 is only used for the identity mapping at this stage. Make it
	 * point to zero page to avoid speculatively fetching new entries.
	 */
	cpu_uninstall_idmap();

	xen_early_init();
	efi_init();

	if (!efi_enabled(EFI_BOOT)) {
		if ((u64)_text % MIN_KIMG_ALIGN)
			pr_warn(FW_BUG "Kernel image misaligned at boot, please fix your bootloader!");
		WARN_TAINT(mmu_enabled_at_boot, TAINT_FIRMWARE_WORKAROUND,
			   FW_BUG "Booted with MMU enabled!");
	}

	arm64_memblock_init();

	paging_init();

	acpi_table_upgrade();

	/* Parse the ACPI tables for possible boot-time configuration */
	acpi_boot_table_init();

	if (acpi_disabled)
		unflatten_device_tree();

	bootmem_init();

	kasan_init();

	request_standard_resources();

	early_ioremap_reset();

	if (acpi_disabled)
		psci_dt_init();
	else
		psci_acpi_init();

	init_bootcpu_ops();
	smp_init_cpus();
	smp_build_mpidr_hash();

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
	/*
	 * Make sure init_thread_info.ttbr0 always generates translation
	 * faults in case uaccess_enable() is inadvertently called by the init
	 * thread.
	 */
	init_task.thread_info.ttbr0 = phys_to_ttbr(__pa_symbol(reserved_pg_dir));
#endif

	if (boot_args[1] || boot_args[2] || boot_args[3]) {
		pr_err("WARNING: x1-x3 nonzero in violation of boot protocol:\n"
			"\tx1: %016llx\n\tx2: %016llx\n\tx3: %016llx\n"
			"This indicates a broken bootloader or old kernel\n",
			boot_args[1], boot_args[2], boot_args[3]);
	}
}

static inline bool cpu_can_disable(unsigned int cpu)
{
#ifdef CONFIG_HOTPLUG_CPU
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	if (ops && ops->cpu_can_disable)
		return ops->cpu_can_disable(cpu);
#endif
	return false;
}

bool arch_cpu_is_hotpluggable(int num)
{
	return cpu_can_disable(num);
}

static void dump_kernel_offset(void)
{
	const unsigned long offset = kaslr_offset();

	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE) && offset > 0) {
		pr_emerg("Kernel Offset: 0x%lx from 0x%lx\n",
			 offset, KIMAGE_VADDR);
		pr_emerg("PHYS_OFFSET: 0x%llx\n", PHYS_OFFSET);
	} else {
		pr_emerg("Kernel Offset: disabled\n");
	}
}

void pmos_hardware_reset(void);

static int arm64_panic_block_dump(struct notifier_block *self,
				  unsigned long v, void *p)
{
	dump_kernel_offset();
	dump_cpu_features();
	dump_mem_limit();
	pr_emerg("PMOSPANIC: panic detected! Hardware rebooting in 10 seconds...\n");
	mdelay(10000);
	pmos_hardware_reset();
	return 0;
}

static struct notifier_block arm64_panic_block = {
	.notifier_call = arm64_panic_block_dump
};

static int __init register_arm64_panic_block(void)
{
	atomic_notifier_chain_register(&panic_notifier_list,
				       &arm64_panic_block);
	return 0;
}
device_initcall(register_arm64_panic_block);

static int __init check_mmu_enabled_at_boot(void)
{
	if (!efi_enabled(EFI_BOOT) && mmu_enabled_at_boot)
		pr_warn("Non-EFI boot detected with MMU and caches enabled (LK bootloader - tolerated)\n");
	return 0;
}
device_initcall_sync(check_mmu_enabled_at_boot);

extern void pmos_dump_deferred_probes(void);

static void pmos_dd_work(struct work_struct *w)
{
	static unsigned int round;

	pr_emerg("PMOS-DD round %u begin\n", ++round);
	pmos_dump_deferred_probes();
	schedule_delayed_work(to_delayed_work(w), 15 * HZ);
}
static DECLARE_DELAYED_WORK(pmos_dd_dwork, pmos_dd_work);

#define PMOS_BF_BASE	0x48090000UL
#define PMOS_BF_SIZE	0x000E0000UL
#define PMOS_BF_SIG	0x43474244u

static atomic_t pmos_bf_round = ATOMIC_INIT(0);

/* Paint tail of kmsg into EVERY 1K slot of the pstore region using
 * upstream persistent_ram record headers, so ANY parser (stock MTK
 * private driver or mainline ramoops on slot A) exposes our text as
 * dmesg-ramoops-N after the next reboot. Called synchronously from
 * the module loader right before each module initcall - survives
 * later hard hangs.
 */
void pmos_paint_now(void)
{
	static char scratch[4096];
	static char txt[980];
	size_t tl = 0, cl, keep;
	struct kmsg_dump_iter iter;
	char line[192];
	void __iomem *io;
	u32 off;

	kmsg_dump_rewind(&iter);
	while (tl < sizeof(scratch) - 256 &&
	       kmsg_dump_get_line(&iter, 1, line, sizeof(line), &cl)) {
		if (cl && line[cl - 1] == '\n')
			cl--;
		line[cl] = 0;
		tl += scnprintf(scratch + tl, sizeof(scratch) - tl,
				"%s\n", line);
	}
	if (!tl)
		return;
	keep = tl > sizeof(txt) ? sizeof(txt) : tl;
	memcpy(txt, scratch + tl - keep, keep);

	io = ioremap(PMOS_BF_BASE, PMOS_BF_SIZE);
	if (!io)
		return;
	for (off = 0; off + 1024 <= PMOS_BF_SIZE; off += 1024) {
		__raw_writel(PMOS_BF_SIG, io + off);
		__raw_writel(0, io + off + 4);          /* start */
		__raw_writel((u32)keep, io + off + 8);  /* size */
		memcpy_toio(io + off + 12, txt, keep);
	}
	iounmap(io);
	pr_emerg("PMOSBF: painted %zu bytes x%u slots\n", keep,
		 (unsigned)(PMOS_BF_SIZE / 1024));
}

static void pmos_bf_work(struct work_struct *w)
{
	pmos_paint_now();
	if (atomic_inc_return(&pmos_bf_round) <= 6)
		schedule_delayed_work(to_delayed_work(w), 10 * HZ);
}
static DECLARE_DELAYED_WORK(pmos_bf_dwork, pmos_bf_work);

void pmos_hardware_reset(void)
{
	void __iomem *b = ioremap(0x10007000, 0x100);
	if (b) {
		writel(0x22000000 | (31 << 5), b + 0x04);
		writel(0x1971, b + 0x08);
		writel(0xF808, b + 0x04);
		iounmap(b);
	}
	emergency_restart();
}

static int pmos_reboot_thread(void *data)
{
	int rem;
	for (rem = 60; rem > 0; rem -= 10) {
		pr_emerg("PMOSCAPTURE: warm auto-restart in %d seconds (pull DRAM log via 0x48090000)...\n", rem);
		ssleep(10);
	}
	pr_emerg("PMOSCAPTURE: executing clean warm restart now...\n");
	kernel_restart("bootloader");
	pmos_hardware_reset();
	return 0;
}

static int __init pmos_reboot_init(void)
{
	kthread_run(pmos_reboot_thread, NULL, "pmos_reboot");
	pr_emerg("PMOSCAPTURE: 60s warm auto-restart kthread active\n");
	return 0;
}
late_initcall(pmos_reboot_init);

#include <linux/input.h>

static bool emg_pwr_down = false;
static bool emg_vol_down = false;
static unsigned long emg_pwr_press_jiffies = 0;
static int emg_pwr_tap_count = 0;
static unsigned long emg_last_tap_jiffies = 0;

static void pmos_emg_key_event(struct input_handle *handle, unsigned int type,
			       unsigned int code, int value)
{
	if (type != EV_KEY)
		return;

	if (code == KEY_POWER) {
		emg_pwr_down = (value != 0);
		if (value) {
			emg_pwr_press_jiffies = jiffies;
			if (time_before(jiffies, emg_last_tap_jiffies + msecs_to_jiffies(1200))) {
				emg_pwr_tap_count++;
				if (emg_pwr_tap_count >= 3) {
					pr_emerg("EMERGENCY: Triple Power tap detected! Instant shutdown...\n");
					kernel_power_off();
					pmos_hardware_reset();
				}
			} else {
				emg_pwr_tap_count = 1;
			}
			emg_last_tap_jiffies = jiffies;
		} else {
			if (emg_pwr_press_jiffies &&
			    time_after(jiffies, emg_pwr_press_jiffies + msecs_to_jiffies(1500))) {
				pr_emerg("EMERGENCY: Long-press Power (>1.5s) detected! Instant shutdown...\n");
				kernel_power_off();
				pmos_hardware_reset();
			}
		}
	} else if (code == KEY_VOLUMEDOWN || code == KEY_VOLUMEUP) {
		emg_vol_down = (value != 0);
	}

	if (emg_pwr_down && emg_vol_down) {
		pr_emerg("=====================================================\n");
		pr_emerg("EMERGENCY: Power + Volume key detected! Instant shutdown...\n");
		pr_emerg("=====================================================\n");
		kernel_power_off();
		pmos_hardware_reset();
	}
}

static int pmos_emg_connect(struct input_handler *handler,
			    struct input_dev *dev,
			    const struct input_device_id *id)
{
	struct input_handle *handle;
	int err;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "pmos_emg_handle";

	err = input_register_handle(handle);
	if (err) {
		kfree(handle);
		return err;
	}

	err = input_open_device(handle);
	if (err) {
		input_unregister_handle(handle);
		kfree(handle);
		return err;
	}

	return 0;
}

static void pmos_emg_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id pmos_emg_ids[] = {
	{ .flags = INPUT_DEVICE_ID_MATCH_EVBIT, .evbit = { BIT_MASK(EV_KEY) } },
	{ },
};

static struct input_handler pmos_emg_handler = {
	.event		= pmos_emg_key_event,
	.connect	= pmos_emg_connect,
	.disconnect	= pmos_emg_disconnect,
	.name		= "pmos_emg_keys",
	.id_table	= pmos_emg_ids,
};

static int __init pmos_emg_init(void)
{
	return input_register_handler(&pmos_emg_handler);
}
late_initcall(pmos_emg_init);

static int __init pmos_ufs_census(void)
{
	struct device_node *np, *sup;
	struct clk *clk;
	int i, n;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt8183-ufshci");
	if (!np) {
		pr_emerg("PMOS-UFS [1] dev-node: MISSING\n");
	} else {
		pr_emerg("PMOS-UFS [1] dev-node: %pOF\n", np);
		n = of_count_phandle_with_args(np, "clocks", "#clock-cells");
		for (i = 0; i < n; i++) {
			const char *nm = NULL;

			of_property_read_string_index(np, "clock-names", i, &nm);
			clk = of_clk_get(np, i);
			if (IS_ERR(clk))
				pr_emerg("PMOS-UFS [2] clock[%d] %s: ERR %ld\n",
					 i, nm ?: "?", PTR_ERR(clk));
			else
				pr_emerg("PMOS-UFS [2] clock[%d] %s: ok\n",
					 i, nm ?: "?");
			clk_put(clk);
		}
		sup = of_parse_phandle(np, "resets", 0);
		pr_emerg("PMOS-UFS [3] reset-provider: %s\n",
			 sup ? of_node_full_name(sup) : "MISSING");
		of_node_put(sup);
		sup = of_parse_phandle(np, "vcc-supply", 0);
		pr_emerg("PMOS-UFS [4] vcc-supply node: %s\n",
			 sup ? of_node_full_name(sup) : "MISSING");
		of_node_put(sup);
		sup = of_parse_phandle(np, "phys", 0);
		pr_emerg("PMOS-UFS [5] phy node: %s\n",
			 sup ? of_node_full_name(sup) : "MISSING");
		of_node_put(sup);
		sup = of_parse_phandle(np, "power-domains", 0);
		pr_emerg("PMOS-UFS [6] power-domain node: %s\n",
			 sup ? of_node_full_name(sup) : "MISSING");
		of_node_put(sup);

		/* Diagnostic MTK-WDT ping */
		{
			void __iomem *b = ioremap(0x10007000, 0x100);
			if (b) {
				pr_emerg("PMOSWDT pre: m=%08x\n", readl(b + 0x00));
				writel(0x22000000 | (31 << 5), b + 0x04);
				writel(0x1971, b + 0x08);
				writel(0xF808, b + 0x04);
				pr_emerg("PMOSWDT post: m=%08x\n", readl(b + 0x00));
				iounmap(b);
			}
		}
	}

	/* schedule_delayed_work(&pmos_dd_dwork, msecs_to_jiffies(12000)); */
	return 0;
}
late_initcall(pmos_ufs_census);

#include <linux/pstore_ram.h>
#include <linux/platform_device.h>
#include <linux/platform_data/simplefb.h>

static struct resource pmos_ramoops_res = DEFINE_RES_MEM(0x48090000, 0x000E0000);

static struct ramoops_platform_data pmos_ramoops_data = {
	.mem_address	= 0x48090000,
	.mem_size	= 0x000E0000,
	.record_size	= 0x00001000,
	.console_size	= 0x000D0000,
	.pmsg_size	= 0x00000000,
	.max_reason	= KMSG_DUMP_OOPS,
};

static struct platform_device pmos_ramoops_dev = {
	.name		= "ramoops",
	.id		= PLATFORM_DEVID_NONE,
	.resource	= &pmos_ramoops_res,
	.num_resources	= 1,
	.dev.platform_data = &pmos_ramoops_data,
};

static int __init pmos_ramoops_register(void)
{
	int ret = platform_device_register(&pmos_ramoops_dev);

	pr_emerg("PMOS-RAMOOPS register @0x48090000 size 0xE0000: %d\n", ret);
	return 0;
}
late_initcall(pmos_ramoops_register);

static struct simplefb_platform_data pmos_simplefb_pdata = {
	.width = 1200,
	.height = 2000,
	.stride = 1216 * 4,
	.format = "x8r8g8b8",
};

static struct resource pmos_simplefb_res = DEFINE_RES_MEM(0xfe2b0000, 1216 * 2000 * 4);

static struct platform_device pmos_simplefb_dev = {
	.name		= "pmos-framebuffer",
	.id		= PLATFORM_DEVID_NONE,
	.resource	= &pmos_simplefb_res,
	.num_resources	= 1,
	.dev.platform_data = &pmos_simplefb_pdata,
};

static int __init pmos_simplefb_register(void)
{
	int ret = platform_device_register(&pmos_simplefb_dev);

	pr_emerg("PMOS-FB: registered pmos-framebuffer platform device @0xfe2b0000: %d\n", ret);
	return 0;
}
subsys_initcall(pmos_simplefb_register);
