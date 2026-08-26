/* PMOS TB351FU embedded init (freestanding, aarch64)
 * Hardware storage pipeline is 100% built into the kernel.
 * Dumps /proc/partitions, creates mknod /dev/mmcblk0p2, mounts ext4 rootfs, boots postmarketOS Phosh.
 */

#define S_IFCHR 0020000
#define S_IFBLK 0060000

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

#define O_RDONLY 00000000
#define O_WRONLY 00000001
#define S_IFBLK 0060000
#define MS_MOVE 8192

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
	long rc;
	int tries;

	kmsg("<3>PMOSINIT: starting rootfs switch sequence (Attempt 276)\n");

	/* Create standard mountpoints */
	syscall3(34 /*mkdirat*/, -100, (long)"/dev", 0755);
	syscall3(34 /*mkdirat*/, -100, (long)"/proc", 0755);
	syscall3(34 /*mkdirat*/, -100, (long)"/sys", 0755);
	syscall3(34 /*mkdirat*/, -100, (long)"/root", 0755);

	/* Mount core virtual filesystems */
	syscall5(40 /*mount*/, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0);
	syscall5(40 /*mount*/, (long)"proc", (long)"/proc", (long)"proc", 0, 0);
	syscall5(40 /*mount*/, (long)"sysfs", (long)"/sys", (long)"sysfs", 0, 0);

	/* Wait 3s for kernel MSDC enumeration to settle */
	sleep_s(3);

	/* Dump kernel partitions list */
	dump_partitions();

	/* Explicitly create block device nodes */
	syscall4(33 /*mknodat*/, -100, (long)"/dev/mmcblk0", S_IFBLK | 0660, (179 << 8) | 0);
	syscall4(33 /*mknodat*/, -100, (long)"/dev/mmcblk0p1", S_IFBLK | 0660, (179 << 8) | 1);
	syscall4(33 /*mknodat*/, -100, (long)"/dev/mmcblk0p2", S_IFBLK | 0660, (179 << 8) | 2);

	/* Mount rootfs on /root with retries (wait up to 35 seconds) */
	rc = -1;
	for (tries = 0; tries < 35; tries++) {
		syscall4(33, -100, (long)"/dev/mmcblk0p2", S_IFBLK | 0660, (179 << 8) | 2);
		rc = syscall5(40 /*mount*/, (long)"/dev/mmcblk0p2", (long)"/root", (long)"ext4", 0, 0);
		if (rc == 0) {
			kmsg("<3>PMOSINIT: /dev/mmcblk0p2 ext4 mount SUCCESS on /root!\n");
			break;
		}
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

			kmsg("<3>PMOSINIT: executing /sbin/init (Phosh)...\n");
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
