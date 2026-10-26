/* PMOS TB351FU embedded init (freestanding, aarch64)
 * Hardware storage pipeline is 100% built into the kernel.
 * Supports UFS 2.2 internal 64GB loopback rootfs (/data/pmos/rootfs.img on sdc64),
 * raw UFS partitions, and MicroSD (mmcblk0p2) with ext4, f2fs, and erofs auto-detection.
 */

#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define O_RDONLY 00000000
#define O_WRONLY 00000001
#define O_RDWR   00000002
#define MS_MOVE  8192
#define LOOP_SET_FD 0x4C00

struct root_target {
	const char *path;
	long dev_t;
	const char *label;
};

static const struct root_target targets[] = {
	{ "/dev/sdc64",     (259 << 8) | 48, "UFS userdata (sdc64)" },
	{ "/dev/sdc62",     (259 << 8) | 46, "UFS super (sdc62)" },
	{ "/dev/mmcblk0p2", (179 << 8) | 2,  "MicroSD p2 (mmcblk0p2)" },
	{ "/dev/sdc57",     (259 << 8) | 41, "UFS tee_b (sdc57)" },
	{ "/dev/sdc58",     (259 << 8) | 42, "UFS lenovoraw (sdc58)" },
};

static const char *fs_types[] = {
	"ext4",
	"f2fs",
	"erofs",
};

static long syscall3(long nr, long a, long b, long c)
{
	register long x8 asm("x8") = nr;
	register long x0 asm("x0") = a;
	register long x1 asm("x1") = b;
	register long x2 asm("x2") = c;
	asm volatile("svc #0" : "+r"(x0)
		     : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
	return x0;
}

static long syscall4(long nr, long a, long b, long c, long d)
{
	register long x8 asm("x8") = nr;
	register long x0 asm("x0") = a;
	register long x1 asm("x1") = b;
	register long x2 asm("x2") = c;
	register long x3 asm("x3") = d;
	asm volatile("svc #0" : "+r"(x0)
		     : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
		     : "memory", "cc");
	return x0;
}

static long syscall5(long nr, long a, long b, long c, long d, long e)
{
	register long x8 asm("x8") = nr;
	register long x0 asm("x0") = a;
	register long x1 asm("x1") = b;
	register long x2 asm("x2") = c;
	register long x3 asm("x3") = d;
	register long x4 asm("x4") = e;
	asm volatile("svc #0" : "+r"(x0)
		     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
		     : "memory", "cc");
	return x0;
}

static void kmsg(const char *s)
{
	long fd = syscall4(56, -100 /*AT_FDCWD*/, (long)"/dev/kmsg",
			   1 /*O_WRONLY*/, 0);
	const char *p = s;
	if (fd < 0)
		return;
	while (*p)
		p++;
	syscall3(64, fd, (long)s, (long)(p - s));
	syscall3(57, fd, 0, 0);
}

static void sleep_s(long s)
{
	static struct {
		long sec;
		long nsec;
	} ts;

	ts.sec = s;
	ts.nsec = 0;
	syscall3(101 /*nanosleep*/, (long)&ts, 0, 0);
}

static void klong(const char *pfx, long v)
{
	char m[96];
	int n = 0;
	char d[24];
	int dn = 0;
	unsigned long u;

	for (; pfx[n] && n < 60; n++)
		m[n] = pfx[n];
	if (v < 0) {
		m[n++] = '-';
		u = (unsigned long)(-v);
	} else {
		u = (unsigned long)v;
	}
	if (!u)
		d[dn++] = '0';
	while (u) {
		d[dn++] = '0' + (char)(u % 10);
		u /= 10;
	}
	while (dn && n < sizeof(m) - 2)
		m[n++] = d[--dn];
	m[n++] = '\n';
	m[n] = '\0';
	kmsg(m);
}

static void dump_partitions(void)
{
	char buf[512];
	long fd = syscall4(56 /*openat*/, -100, (long)"/proc/partitions", O_RDONLY, 0);
	long n;
	if (fd >= 0) {
		kmsg("<3>PMOSINIT: === /proc/partitions ===\n");
		while ((n = syscall3(63 /*read*/, fd, (long)buf, sizeof(buf) - 1)) > 0) {
			buf[n] = '\0';
			kmsg(buf);
		}
		syscall3(57 /*close*/, fd, 0, 0);
		kmsg("<3>PMOSINIT: === end partitions ===\n");
	} else {
		klong("<3>PMOSINIT: /proc/partitions open failed rc=", fd);
	}
}

void _start(void)
{
	static char *argv[] = { "/sbin/init", 0 };
	static char *env[] = { "HOME=/", "TERM=linux", "PATH=/bin:/sbin:/usr/bin:/usr/sbin", 0 };
	long rc = -1;
	int tries;
	int is_loopback = 0;
	const char *mounted_target = 0;
	const char *mounted_fs = 0;

	kmsg("<3>PMOSINIT: starting rootfs switch sequence (Attempt 278 - UFS 64GB Loopback)\n");

	/* Create standard mountpoints */
	syscall3(34 /*mkdirat*/, -100, (long)"/dev", 0755);
	syscall3(34 /*mkdirat*/, -100, (long)"/proc", 0755);
	syscall3(34 /*mkdirat*/, -100, (long)"/sys", 0755);
	syscall3(34 /*mkdirat*/, -100, (long)"/root", 0755);
	syscall3(34 /*mkdirat*/, -100, (long)"/data_host", 0755);

	/* Mount core virtual filesystems */
	syscall5(40 /*mount*/, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0);
	syscall5(40 /*mount*/, (long)"proc", (long)"/proc", (long)"proc", 0, 0);
	syscall5(40 /*mount*/, (long)"sysfs", (long)"/sys", (long)"sysfs", 0, 0);

	/* Wait 3s for storage controllers (UFS & MSDC) to enumerate */
	sleep_s(3);

	/* Dump kernel partitions list */
	dump_partitions();

	/* Explicitly create block device nodes for both MicroSD and UFS */
	syscall4(33 /*mknodat*/, -100, (long)"/dev/loop0", S_IFBLK | 0660, (7 << 8) | 0);
	syscall4(33 /*mknodat*/, -100, (long)"/dev/mmcblk0", S_IFBLK | 0660, (179 << 8) | 0);
	syscall4(33 /*mknodat*/, -100, (long)"/dev/mmcblk0p1", S_IFBLK | 0660, (179 << 8) | 1);
	syscall4(33 /*mknodat*/, -100, (long)"/dev/mmcblk0p2", S_IFBLK | 0660, (179 << 8) | 2);

	syscall4(33 /*mknodat*/, -100, (long)"/dev/sdc", S_IFBLK | 0660, (8 << 8) | 32);
	syscall4(33 /*mknodat*/, -100, (long)"/dev/sdc57", S_IFBLK | 0660, (259 << 8) | 41);
	syscall4(33 /*mknodat*/, -100, (long)"/dev/sdc58", S_IFBLK | 0660, (259 << 8) | 42);
	syscall4(33 /*mknodat*/, -100, (long)"/dev/sdc62", S_IFBLK | 0660, (259 << 8) | 46);
	syscall4(33 /*mknodat*/, -100, (long)"/dev/sdc64", S_IFBLK | 0660, (259 << 8) | 48);

	/* STEP 1: Try UFS 64GB Loopback Image (/data/pmos/rootfs.img on sdc64) */
	for (tries = 0; tries < 25 && rc != 0; tries++) {
		syscall4(33 /*mknodat*/, -100, (long)"/dev/sdc64", S_IFBLK | 0660, (259 << 8) | 48);
		long data_rc = syscall5(40 /*mount*/, (long)"/dev/sdc64", (long)"/data_host", (long)"f2fs", 0, 0);
		if (data_rc == 0) {
			kmsg("<3>PMOSINIT: /dev/sdc64 (UFS userdata) mounted on /data_host!\n");
			long img_fd = syscall4(56 /*openat*/, -100, (long)"/data_host/pmos/rootfs.img", O_RDWR, 0);
			if (img_fd >= 0) {
				kmsg("<3>PMOSINIT: found /data_host/pmos/rootfs.img (64GB)! Configuring /dev/loop0...\n");
				syscall4(33 /*mknodat*/, -100, (long)"/dev/loop0", S_IFBLK | 0660, (7 << 8) | 0);
				long loop_fd = syscall4(56 /*openat*/, -100, (long)"/dev/loop0", O_RDWR, 0);
				if (loop_fd >= 0) {
					long lrc = syscall3(29 /*ioctl*/, loop_fd, LOOP_SET_FD, img_fd);
					if (lrc == 0) {
						kmsg("<3>PMOSINIT: loop0 attached! Mounting 64GB ext4 rootfs...\n");
						rc = syscall5(40 /*mount*/, (long)"/dev/loop0", (long)"/root", (long)"ext4", 0, 0);
						if (rc == 0) {
							kmsg("<3>PMOSINIT: 64GB postmarketOS rootfs mount SUCCESS on /root!\n");
							mounted_target = "UFS 64GB loopback (/data/pmos/rootfs.img)";
							mounted_fs = "ext4";
							is_loopback = 1;
							syscall3(57 /*close*/, loop_fd, 0, 0);
							syscall3(57 /*close*/, img_fd, 0, 0);
							break;
						} else {
							klong("<3>PMOSINIT: mount /dev/loop0 ext4 failed rc=", rc);
						}
					} else {
						klong("<3>PMOSINIT: LOOP_SET_FD ioctl failed rc=", lrc);
					}
					syscall3(57 /*close*/, loop_fd, 0, 0);
				}
				syscall3(57 /*close*/, img_fd, 0, 0);
			} else {
				kmsg("<3>PMOSINIT: /data_host/pmos/rootfs.img not found\n");
			}
			/* If rootfs.img not found or failed, unmount /data_host to allow other targets */
			if (rc != 0)
				syscall3(52 /*umount2*/, (long)"/data_host", 0, 0);
		}
		if (rc != 0)
			sleep_s(1);
	}

	/* STEP 2: Fall back to partition probes (MicroSD /dev/mmcblk0p2, etc.) if loopback not found */
	for (tries = 0; tries < 15 && rc != 0; tries++) {
		int t;
		for (t = 0; t < (int)(sizeof(targets) / sizeof(targets[0])); t++) {
			int f;
			syscall4(33 /*mknodat*/, -100, (long)targets[t].path, S_IFBLK | 0660, targets[t].dev_t);
			for (f = 0; f < (int)(sizeof(fs_types) / sizeof(fs_types[0])); f++) {
				rc = syscall5(40 /*mount*/, (long)targets[t].path, (long)"/root", (long)fs_types[f], 0, 0);
				if (rc == 0) {
					mounted_target = targets[t].label;
					mounted_fs = fs_types[f];
					kmsg("<3>PMOSINIT: rootfs mount SUCCESS: ");
					kmsg(mounted_target);
					kmsg(" (");
					kmsg(mounted_fs);
					kmsg(") on /root!\n");
					break;
				}
			}
			if (rc == 0)
				break;
		}
		if (rc != 0)
			sleep_s(1);
	}

	if (rc == 0) {
		kmsg("<3>PMOSINIT: mount SUCCESS! Switching root to postmarketOS...\n");

		/* Move mount /dev, /proc, /sys into rootfs */
		syscall3(34 /*mkdirat*/, -100, (long)"/root/dev", 0755);
		syscall3(34 /*mkdirat*/, -100, (long)"/root/proc", 0755);
		syscall3(34 /*mkdirat*/, -100, (long)"/root/sys", 0755);

		syscall5(40, (long)"/dev", (long)"/root/dev", 0, MS_MOVE, 0);
		syscall5(40, (long)"/proc", (long)"/root/proc", 0, MS_MOVE, 0);
		syscall5(40, (long)"/sys", (long)"/root/sys", 0, MS_MOVE, 0);

		if (is_loopback) {
			/* Keep /data_host mounted under /root/data_host so loop0 backing file remains open */
			syscall3(34 /*mkdirat*/, -100, (long)"/root/data_host", 0755);
			syscall5(40, (long)"/data_host", (long)"/root/data_host", 0, MS_MOVE, 0);
		}

		/* Create essential character device nodes */
		syscall4(33 /*mknodat*/, -100, (long)"/root/dev/console", S_IFCHR | 0600, (5 << 8) | 1);
		syscall4(33 /*mknodat*/, -100, (long)"/root/dev/null", S_IFCHR | 0666, (1 << 8) | 3);
		syscall4(33 /*mknodat*/, -100, (long)"/root/dev/zero", S_IFCHR | 0666, (1 << 8) | 5);
		syscall4(33 /*mknodat*/, -100, (long)"/root/dev/kmsg", S_IFCHR | 0660, (1 << 8) | 11);
		syscall4(33 /*mknodat*/, -100, (long)"/root/dev/tty0", S_IFCHR | 0660, (4 << 8) | 0);
		syscall4(33 /*mknodat*/, -100, (long)"/root/dev/tty", S_IFCHR | 0666, (5 << 8) | 0);

		if (!syscall3(49 /*chdir*/, (long)"/root", 0, 0)) {
			syscall5(40, (long)".", (long)"/", 0, MS_MOVE, 0);
			syscall3(51 /*chroot*/, (long)".", 0, 0);

			kmsg("<3>PMOSINIT: executing /sbin/init (Phosh / OpenRC)...\n");
			syscall3(221 /*execve*/, (long)"/sbin/init", (long)argv, (long)env);
			syscall3(221 /*execve*/, (long)"/usr/lib/systemd/systemd", (long)argv, (long)env);
			syscall3(221 /*execve*/, (long)"/init", (long)argv, (long)env);
		}
	} else {
		klong("<3>PMOSINIT: FATAL: could not mount rootfs rc=", rc);
	}

	for (;;) {
		sleep_s(30);
	}
}
