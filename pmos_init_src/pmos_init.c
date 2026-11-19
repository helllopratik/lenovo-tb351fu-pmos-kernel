/* PMOS TB351FU embedded init (freestanding, aarch64)
 *
 * Storage pipeline:
 *   1. Discover the Android userdata partition by GPT name (userdata)
 *      instead of assuming a fixed UFS device node.
 *   2. If a metadata key is configured, create a dm-default-key mapping
 *      over userdata.  This decrypts Android's metadata encryption layer
 *      without touching the on-disk layout or dm tables.
 *   3. Mount the userdata filesystem read-write with inlinecrypt.  The
 *      inlinecrypt option is mandatory when Android wrote the files through
 *      blk-crypto: those bios carry a crypt context and skip dm-default-key.
 *   4. Add the per-install v2 fscrypt key and open /data/pmos/rootfs.img.
 *   5. Attach the image to a loop device and mount the ext4 rootfs.
 *
 * If no keys are configured, the same code still boots unencrypted userdata,
 * raw partitions, MicroSD, USB, and the legacy fixed targets.
 *
 * Keys are injected at build time through pmos_storage_config.h.  They are
 * never stored in this source tree.
 */

#if defined(__has_include)
#  if __has_include("pmos_storage_config.h")
#    include "pmos_storage_config.h"
#  endif
#endif

#ifndef PMOS_DM_METADATA_KEY
#define PMOS_DM_METADATA_KEY ""
#endif
#ifndef PMOS_DM_CIPHER
#define PMOS_DM_CIPHER "aes-xts-plain64"
#endif
#ifndef PMOS_DM_SECTOR_SIZE
#define PMOS_DM_SECTOR_SIZE 4096
#endif
#ifndef PMOS_DM_IV_LARGE_SECTORS
#define PMOS_DM_IV_LARGE_SECTORS 1
#endif
#ifndef PMOS_DM_ALLOW_DISCARDS
#define PMOS_DM_ALLOW_DISCARDS 1
#endif
#ifndef PMOS_DM_WRAPPEDKEY
#define PMOS_DM_WRAPPEDKEY 0
#endif
#ifndef PMOS_FSCRYPT_MASTER_KEY
#define PMOS_FSCRYPT_MASTER_KEY ""
#endif
#ifndef PMOS_FSCRYPT_IDENTIFIER
#define PMOS_FSCRYPT_IDENTIFIER ""
#endif
#ifndef PMOS_DATA_INLINECRYPT
#define PMOS_DATA_INLINECRYPT 1
#endif
#ifndef PMOS_ROOTFS_SUBDIR
#define PMOS_ROOTFS_SUBDIR "pmos"
#endif
#ifndef PMOS_ROOTFS_NAME
#define PMOS_ROOTFS_NAME "rootfs.img"
#endif

#define AT_FDCWD (-100)
#define O_RDONLY 00000000
#define O_WRONLY 00000001
#define O_RDWR   00000002
#define O_CLOEXEC 02000000
/* arm64 overrides the asm-generic value; see arch/arm64/include/uapi/asm/fcntl.h */
#define O_DIRECTORY 040000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFMT  0170000
#define MS_NOSUID  2
#define MS_NODEV   4
#define MS_NOATIME 1024
#define MS_MOVE  8192
#define LOOP_SET_FD 0x4C00
#define LOOP_CLR_FD 0x4C01
#define LOOP_CTL_GET_FREE 0x4C82
#define BLKGETSIZE64 0x80081272UL

struct root_target {
	const char *path;
	long dev_t;
	const char *label;
};

/* Legacy fixed targets retained for this tablet's existing fallbacks.  The
 * generic path below discovers userdata dynamically on other devices. */
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

/* The compiler can emit calls to these even with -fno-builtin. */
void *memcpy(void *dst, const void *src, unsigned long n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	while (n--)
		*d++ = *s++;
	return dst;
}

void *memset(void *dst, int c, unsigned long n)
{
	unsigned char *d = dst;
	while (n--)
		*d++ = (unsigned char)c;
	return dst;
}

int memcmp(const void *a, const void *b, unsigned long n)
{
	const unsigned char *x = a;
	const unsigned char *y = b;
	while (n--) {
		if (*x != *y)
			return (int)*x - (int)*y;
		x++;
		y++;
	}
	return 0;
}

static unsigned long slen(const char *s)
{
	unsigned long n = 0;
	while (s[n])
		n++;
	return n;
}

static int scmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

static int scmp_ci(const char *a, const char *b)
{
	while (*a && *b) {
		unsigned char ca = (unsigned char)*a++;
		unsigned char cb = (unsigned char)*b++;
		if (ca >= 'A' && ca <= 'Z')
			ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'Z')
			cb += 'a' - 'A';
		if (ca != cb)
			return (int)ca - (int)cb;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

static void scopy(char *dst, const char *src, unsigned long max)
{
	if (!max)
		return;
	while (--max && *src)
		*dst++ = *src++;
	*dst = '\0';
}

static void sappend(char **p, char *end, const char *s)
{
	while (*s && *p < end)
		*(*p)++ = *s++;
}

static void sappend_num(char **p, char *end, unsigned long long v)
{
	char tmp[24];
	int n = 0;
	if (!v)
		tmp[n++] = '0';
	while (v) {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (n-- && *p < end)
		*(*p)++ = tmp[n];
}

static void kmsg(const char *s)
{
	long fd = syscall4(56, AT_FDCWD, (long)"/dev/kmsg",
			   O_WRONLY, 0);
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
	syscall3(101, (long)&ts, 0, 0);
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
	while (dn && n < (int)sizeof(m) - 2)
		m[n++] = d[--dn];
	m[n++] = '\n';
	m[n] = '\0';
	kmsg(m);
}

static char hexchar(unsigned int v)
{
	return (char)(v < 10 ? '0' + v : 'a' + (v - 10));
}

static void kmsg_hex(const char *pfx, const unsigned char *p, unsigned long n)
{
	char b[160];
	int i = 0;
	while (*pfx && i < 90)
		b[i++] = *pfx++;
	for (unsigned long j = 0; j < n && i < 150; j++) {
		b[i++] = hexchar(p[j] >> 4);
		b[i++] = hexchar(p[j] & 15);
	}
	b[i++] = '\n';
	b[i] = '\0';
	kmsg(b);
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int hex2bin(const char *s, unsigned char *out, unsigned long max)
{
	unsigned long n = 0;
	while (s[0] && s[1]) {
		int a = hexval(s[0]);
		int b = hexval(s[1]);
		if (a < 0 || b < 0 || n >= max)
			return -1;
		out[n++] = (unsigned char)((a << 4) | b);
		s += 2;
	}
	if (*s)
		return -1;
	return (int)n;
}

static unsigned long dev_encode(unsigned int maj, unsigned int min)
{
	return (unsigned long)((min & 0xff) | (maj << 8) |
			       ((min & ~0xffU) << 12));
}

static void dev_decode(unsigned long dev, unsigned int *maj, unsigned int *min)
{
	*maj = (unsigned int)((dev >> 8) & 0xfff);
	*min = (unsigned int)((dev & 0xff) | ((dev >> 12) & 0xfff00));
}

static int read_text(const char *path, char *buf, unsigned long max)
{
	long fd = syscall4(56, AT_FDCWD, (long)path, O_RDONLY | O_CLOEXEC, 0);
	long n;
	if (fd < 0)
		return (int)fd;
	if (!max) {
		syscall3(57, fd, 0, 0);
		return -22;
	}
	n = syscall3(63, fd, (long)buf, (long)(max - 1));
	syscall3(57, fd, 0, 0);
	if (n < 0)
		return (int)n;
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
			 buf[n - 1] == ' ' || buf[n - 1] == '\t'))
		buf[--n] = '\0';
	return (int)n;
}

static int parse_u64(const char *s, unsigned long long *out)
{
	unsigned long long v = 0;
	int digits = 0;
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (unsigned long long)(*s - '0');
		s++;
		digits++;
	}
	if (!digits)
		return -1;
	while (*s == '\n' || *s == '\r' || *s == ' ' || *s == '\t')
		s++;
	if (*s)
		return -1;
	*out = v;
	return 0;
}

static int parse_mm(const char *s, unsigned int *maj, unsigned int *min)
{
	unsigned long a = 0, b = 0;
	int got = 0;
	while (*s >= '0' && *s <= '9')
		a = a * 10 + (unsigned long)(*s++ - '0');
	if (*s++ != ':')
		return -1;
	while (*s >= '0' && *s <= '9') {
		b = b * 10 + (unsigned long)(*s++ - '0');
		got = 1;
	}
	if (!got)
		return -1;
	*maj = (unsigned int)a;
	*min = (unsigned int)b;
	return 0;
}

static void path3(char *out, unsigned long outsz, const char *a,
		  const char *b, const char *c)
{
	char *p = out;
	char *end = out + outsz - 1;
	sappend(&p, end, a);
	sappend(&p, end, b);
	sappend(&p, end, c);
	*p = '\0';
}

static int uevent_value(const char *buf, const char *key, char *out,
			unsigned long outsz)
{
	const char *p = buf;
	unsigned long kl = slen(key);

	while (*p) {
		const char *nl = p;
		while (*nl && *nl != '\n')
			nl++;
		if ((unsigned long)(nl - p) > kl &&
		    !memcmp(p, key, kl) && p[kl] == '=') {
			unsigned long n = (unsigned long)(nl - p) - kl - 1;
			if (n >= outsz)
				n = outsz - 1;
			memcpy(out, p + kl + 1, n);
			out[n] = '\0';
			return 0;
		}
		p = *nl ? nl + 1 : nl;
	}
	return -1;
}

struct userdata_info {
	char path[96];
	char name[64];
	char mm[32];
	unsigned int maj;
	unsigned int min;
	unsigned long long sectors;
};

static int userdata_from_name(const char *name, struct userdata_info *info)
{
	char p[256];
	char b[64];
	unsigned long long sectors;
	unsigned int maj, min;
	long rc;

	path3(p, sizeof(p), "/sys/class/block/", name, "/dev");
	if (read_text(p, b, sizeof(b)) <= 0 || parse_mm(b, &maj, &min) < 0)
		return -1;
	path3(p, sizeof(p), "/sys/class/block/", name, "/size");
	if (read_text(p, b, sizeof(b)) <= 0 || parse_u64(b, &sectors) < 0)
		return -1;
	if (sectors < 8)
		return -1;

	memset(info, 0, sizeof(*info));
	scopy(info->name, name, sizeof(info->name));
	scopy(info->path, "/dev/", sizeof(info->path));
	{
		unsigned long n = slen(info->path);
		scopy(info->path + n, name, sizeof(info->path) - n);
	}
	info->maj = maj;
	info->min = min;
	info->sectors = sectors;
	{
		char *q = info->mm;
		char *end = info->mm + sizeof(info->mm) - 1;
		sappend_num(&q, end, maj);
		sappend(&q, end, ":");
		sappend_num(&q, end, min);
		*q = '\0';
	}

	/* devtmpfs normally creates this; make it ourselves if needed. */
	rc = syscall4(33, AT_FDCWD, (long)info->path,
		      S_IFBLK | 0600, (long)dev_encode(maj, min));
	(void)rc;
	return 0;
}

static int find_userdata_sysfs(struct userdata_info *info)
{
	static char dirbuf[2048];
	long fd;
	long n;

	fd = syscall4(56, AT_FDCWD, (long)"/sys/class/block",
		      O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
	if (fd < 0)
		return (int)fd;

	while ((n = syscall3(61, fd, (long)dirbuf, sizeof(dirbuf))) > 0) {
		long off = 0;
		while (off < n) {
			struct linux_dirent64 {
				unsigned long d_ino;
				long d_off;
				unsigned short d_reclen;
				unsigned char d_type;
				char d_name[];
			} *d = (void *)(dirbuf + off);
			char uevent[512];
			char part[64];
			char p[256];

			if (!d->d_reclen)
				break;
			if (d->d_name[0] != '.') {
				char name[64];
				unsigned long i = 0;
				while (d->d_name[i] && i + 1 < sizeof(name)) {
					name[i] = d->d_name[i];
					i++;
				}
				name[i] = '\0';
				path3(p, sizeof(p), "/sys/class/block/",
				      name, "/uevent");
				if (read_text(p, uevent, sizeof(uevent)) > 0 &&
				    uevent_value(uevent, "PARTNAME", part,
						 sizeof(part)) == 0 &&
				    scmp_ci(part, "userdata") == 0) {
					if (userdata_from_name(name, info) == 0) {
						syscall3(57, fd, 0, 0);
						return 0;
					}
				}
			}
			off += d->d_reclen;
		}
	}
	syscall3(57, fd, 0, 0);
	return -1;
}

struct kstat64 {
	unsigned long st_dev;
	unsigned long st_ino;
	unsigned int st_mode;
	unsigned int st_nlink;
	unsigned int st_uid;
	unsigned int st_gid;
	unsigned long st_rdev;
	unsigned long __pad1;
	long st_size;
	int st_blksize;
	int __pad2;
	long st_blocks;
	long st_atime;
	unsigned long st_atime_nsec;
	long st_mtime;
	unsigned long st_mtime_nsec;
	long st_ctime;
	unsigned long st_ctime_nsec;
	unsigned int __unused4;
	unsigned int __unused5;
};

static int find_userdata_byname(struct userdata_info *info)
{
	struct kstat64 st;
	unsigned long long bytes = 0;
	unsigned int maj, min;
	long fd;
	long rc;

	rc = syscall4(79, AT_FDCWD, (long)"/dev/block/by-name/userdata",
		      (long)&st, 0);
	if (rc < 0)
		return -1;
	dev_decode(st.st_rdev, &maj, &min);

	fd = syscall4(56, AT_FDCWD, (long)"/dev/block/by-name/userdata",
		      O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	rc = syscall3(29, fd, BLKGETSIZE64, (long)&bytes);
	syscall3(57, fd, 0, 0);
	if (rc < 0 || bytes < 4096)
		return -1;

	memset(info, 0, sizeof(*info));
	scopy(info->path, "/dev/block/by-name/userdata",
	      sizeof(info->path));
	scopy(info->name, "userdata", sizeof(info->name));
	info->maj = maj;
	info->min = min;
	info->sectors = bytes >> 9;
	{
		char *q = info->mm;
		char *end = info->mm + sizeof(info->mm) - 1;
		sappend_num(&q, end, maj);
		sappend(&q, end, ":");
		sappend_num(&q, end, min);
		*q = '\0';
	}
	return 0;
}

/*
 * Last-resort discovery for this tablet: the initramfs pre-creates
 * /dev/sdc64 with the UFS userdata device number.  Opening it directly does
 * not depend on sysfs, devtmpfs symlinks, or udev, and BLKGETSIZE64 rejects
 * a node that exists before the block device is ready.  It is tried before
 * the sysfs scan so a slow or unavailable sysfs cannot hide userdata.
 */
static int userdata_from_devpath(const char *path, struct userdata_info *info)
{
	struct kstat64 st;
	unsigned long long bytes = 0;
	unsigned int maj, min;
	long fd;
	long rc;

	fd = syscall4(56, AT_FDCWD, (long)path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	rc = syscall3(80, fd, (long)&st, 0);
	if (rc < 0 || (st.st_mode & S_IFMT) != S_IFBLK) {
		syscall3(57, fd, 0, 0);
		return -1;
	}
	dev_decode(st.st_rdev, &maj, &min);

	rc = syscall3(29, fd, BLKGETSIZE64, (long)&bytes);
	syscall3(57, fd, 0, 0);
	if (rc < 0 || bytes < (1ULL << 30))
		return -1;

	memset(info, 0, sizeof(*info));
	scopy(info->path, path, sizeof(info->path));
	scopy(info->name, "userdata", sizeof(info->name));
	info->maj = maj;
	info->min = min;
	info->sectors = bytes >> 9;
	{
		char *q = info->mm;
		char *end = info->mm + sizeof(info->mm) - 1;
		sappend_num(&q, end, maj);
		sappend(&q, end, ":");
		sappend_num(&q, end, min);
		*q = '\0';
	}
	return 0;
}

static int find_userdata(struct userdata_info *info)
{
	memset(info, 0, sizeof(*info));
	if (find_userdata_byname(info) == 0 ||
	    userdata_from_devpath("/dev/sdc64", info) == 0 ||
	    find_userdata_sysfs(info) == 0) {
		kmsg("<3>PMOSINIT: userdata partition: ");
		kmsg(info->name);
		kmsg(" (");
		kmsg(info->mm);
		kmsg(")\n");
		return 0;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* device-mapper: default-key                                          */
/* ------------------------------------------------------------------ */

struct dm_ioctl {
	unsigned int version[3];
	unsigned int data_size;
	unsigned int data_start;
	unsigned int target_count;
	int open_count;
	unsigned int flags;
	unsigned int event_nr;
	unsigned int padding;
	unsigned long long dev;
	char name[128];
	char uuid[129];
	char data[7];
};

struct dm_target_spec {
	unsigned long long sector_start;
	unsigned long long length;
	int status;
	unsigned int next;
	char target_type[16];
};

#define DM_IOCTL_NR(cmd) (0xC0000000UL | \
			  ((unsigned long)sizeof(struct dm_ioctl) << 16) | \
			  (0xfdUL << 8) | (unsigned long)(cmd))
#define DM_VERSION      DM_IOCTL_NR(0)
#define DM_DEV_CREATE   DM_IOCTL_NR(3)
#define DM_DEV_SUSPEND  DM_IOCTL_NR(6)
#define DM_DEV_STATUS   DM_IOCTL_NR(7)
#define DM_TABLE_LOAD   DM_IOCTL_NR(9)
#define DM_SECURE_DATA_FLAG (1 << 15)

static unsigned int dm_ver[3] = { 4, 0, 0 };
static unsigned char dm_buf[1024] __attribute__((aligned(8)));

static void dm_init_io(struct dm_ioctl *io, unsigned int size)
{
	memset(io, 0, size);
	io->version[0] = dm_ver[0];
	io->version[1] = dm_ver[1];
	io->version[2] = dm_ver[2];
	io->data_size = size;
}

static int dm_control_open(void)
{
	char b[64];
	unsigned int maj = 10, min = 236;
	long fd;
	long rc;

	syscall3(34, AT_FDCWD, (long)"/dev/mapper", 0755);
	if (read_text("/sys/class/misc/device-mapper/dev", b, sizeof(b)) > 0)
		(void)parse_mm(b, &maj, &min);
	syscall4(33, AT_FDCWD, (long)"/dev/mapper/control",
		 S_IFCHR | 0600, (long)dev_encode(maj, min));

	fd = syscall4(56, AT_FDCWD, (long)"/dev/mapper/control",
		      O_RDWR | O_CLOEXEC, 0);
	if (fd < 0)
		return (int)fd;

	memset(dm_buf, 0, sizeof(struct dm_ioctl));
	{
		struct dm_ioctl *io = (void *)dm_buf;
		io->version[0] = 4;
		io->version[1] = 0;
		io->version[2] = 0;
		io->data_size = sizeof(struct dm_ioctl);
		rc = syscall3(29, fd, DM_VERSION, (long)io);
		if (rc < 0) {
			syscall3(57, fd, 0, 0);
			return (int)rc;
		}
		dm_ver[0] = io->version[0];
		dm_ver[1] = io->version[1];
		dm_ver[2] = io->version[2];
	}
	if (dm_ver[0] != 4) {
		syscall3(57, fd, 0, 0);
		return -93;
	}
	return (int)fd;
}

static int dm_create(int fd, const char *name, unsigned long long *enc)
{
	struct dm_ioctl *io = (void *)dm_buf;
	long rc;

	memset(dm_buf, 0, sizeof(struct dm_ioctl));
	dm_init_io(io, sizeof(struct dm_ioctl));
	scopy(io->name, name, sizeof(io->name));
	rc = syscall3(29, fd, DM_DEV_CREATE, (long)io);
	if (rc < 0 && rc != -17)
		return (int)rc;
	if (rc == -17) {
		memset(dm_buf, 0, sizeof(struct dm_ioctl));
		dm_init_io(io, sizeof(struct dm_ioctl));
		scopy(io->name, name, sizeof(io->name));
		rc = syscall3(29, fd, DM_DEV_STATUS, (long)io);
		if (rc < 0)
			return (int)rc;
	}
	if (!io->dev)
		return -1;
	*enc = io->dev;
	return 0;
}

static int dm_load(int fd, const char *name, unsigned long long sectors,
		   const char *params)
{
	struct dm_ioctl *io = (void *)dm_buf;
	struct dm_target_spec *spec;
	unsigned long params_len = slen(params);
	unsigned long spec_len = sizeof(struct dm_target_spec) + params_len + 1;
	unsigned long total;
	long rc;

	spec_len = (spec_len + 7) & ~7UL;
	total = sizeof(struct dm_ioctl) + spec_len;
	if (total > sizeof(dm_buf))
		return -7;

	memset(dm_buf, 0, total);
	dm_init_io(io, (unsigned int)total);
	io->data_start = sizeof(struct dm_ioctl);
	io->target_count = 1;
	scopy(io->name, name, sizeof(io->name));

	spec = (void *)(dm_buf + sizeof(struct dm_ioctl));
	spec->sector_start = 0;
	spec->length = sectors;
	spec->next = (unsigned int)spec_len;
	scopy(spec->target_type, "default-key", sizeof(spec->target_type));
	memcpy(spec + 1, params, params_len + 1);

	/* Ask the kernel to wipe its own copy of the target parameters. */
	io->flags = DM_SECURE_DATA_FLAG;
	rc = syscall3(29, fd, DM_TABLE_LOAD, (long)io);
	memset(dm_buf, 0, sizeof(dm_buf));
	if (rc < 0)
		return (int)rc;
	return 0;
}

static int dm_resume(int fd, const char *name)
{
	struct dm_ioctl *io = (void *)dm_buf;
	long rc;

	memset(dm_buf, 0, sizeof(struct dm_ioctl));
	dm_init_io(io, sizeof(struct dm_ioctl));
	scopy(io->name, name, sizeof(io->name));
	io->flags = 0; /* 0 = resume, DM_SUSPEND_FLAG would suspend */
	rc = syscall3(29, fd, DM_DEV_SUSPEND, (long)io);
	if (rc < 0)
		return (int)rc;
	return 0;
}

static int build_dm_params(char *out, unsigned long outsz,
			   const struct userdata_info *ud)
{
	char opts[128];
	char *op = opts;
	char *oend = opts + sizeof(opts) - 1;
	char *p = out;
	char *end = out + outsz - 1;
	int nopts = 0;

	*op = '\0';
#if PMOS_DM_ALLOW_DISCARDS
	sappend(&op, oend, "allow_discards");
	nopts++;
#endif
	if (PMOS_DM_SECTOR_SIZE != 512) {
		if (nopts)
			sappend(&op, oend, " ");
		sappend(&op, oend, "sector_size:");
		sappend_num(&op, oend, PMOS_DM_SECTOR_SIZE);
		nopts++;
#if PMOS_DM_IV_LARGE_SECTORS
		sappend(&op, oend, " iv_large_sectors");
		nopts++;
#endif
	}
#if PMOS_DM_WRAPPEDKEY
	if (nopts)
		sappend(&op, oend, " ");
	sappend(&op, oend, "wrappedkey_v0");
	nopts++;
#endif
	*op = '\0';

	sappend(&p, end, PMOS_DM_CIPHER);
	sappend(&p, end, " ");
	sappend(&p, end, PMOS_DM_METADATA_KEY);
	sappend(&p, end, " 0 ");
	sappend_num(&p, end, ud->maj);
	sappend(&p, end, ":");
	sappend_num(&p, end, ud->min);
	sappend(&p, end, " 0 ");
	sappend_num(&p, end, (unsigned long long)nopts);
	if (nopts) {
		sappend(&p, end, " ");
		sappend(&p, end, opts);
	}
	*p = '\0';
	return 0;
}

static int setup_default_key(const struct userdata_info *ud, char *devpath,
			     unsigned long devpathsz)
{
	char params[512];
	int fd;
	int rc;
	unsigned long long enc = 0;
	unsigned int maj, min;
	unsigned long long sectors = ud->sectors;

	if (!PMOS_DM_METADATA_KEY[0])
		return -2;
	if ((slen(PMOS_DM_METADATA_KEY) & 1) ||
	    slen(PMOS_DM_METADATA_KEY) > 128) {
		kmsg("<3>PMOSINIT: metadata key has invalid length\n");
		return -22;
	}
	if (sectors & 7) {
		kmsg("<3>PMOSINIT: userdata size is not 4K aligned; rounding down\n");
		sectors &= ~7ULL;
	}

	build_dm_params(params, sizeof(params), ud);
	fd = dm_control_open();
	if (fd < 0) {
		klong("<3>PMOSINIT: dm control open failed rc=", fd);
		memset(params, 0, sizeof(params));
		return fd;
	}
	rc = dm_create(fd, "pmos_userdata", &enc);
	if (rc < 0) {
		klong("<3>PMOSINIT: dm create failed rc=", rc);
		memset(params, 0, sizeof(params));
		syscall3(57, fd, 0, 0);
		return rc;
	}
	rc = dm_load(fd, "pmos_userdata", sectors, params);
	memset(params, 0, sizeof(params));
	if (rc < 0) {
		klong("<3>PMOSINIT: dm table load failed rc=", rc);
		syscall3(57, fd, 0, 0);
		return rc;
	}
	rc = dm_resume(fd, "pmos_userdata");
	if (rc < 0) {
		klong("<3>PMOSINIT: dm resume failed rc=", rc);
		syscall3(57, fd, 0, 0);
		return rc;
	}
	syscall3(57, fd, 0, 0);

	dev_decode((unsigned long)enc, &maj, &min);
	scopy(devpath, "/dev/pmos_data", devpathsz);
	syscall4(33, AT_FDCWD, (long)devpath, S_IFBLK | 0600,
		 (long)dev_encode(maj, min));
	kmsg("<3>PMOSINIT: dm-default-key mapping ready: ");
	kmsg(devpath);
	kmsg(" -> ");
	kmsg(ud->mm);
	kmsg("\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* fscrypt v2                                                          */
/* ------------------------------------------------------------------ */

struct fscrypt_key_specifier {
	unsigned int type;
	unsigned int reserved;
	union {
		unsigned char reserved[32];
		unsigned char descriptor[8];
		unsigned char identifier[16];
	} u;
};

struct fscrypt_add_key_arg {
	struct fscrypt_key_specifier key_spec;
	unsigned int raw_size;
	unsigned int key_id;
	unsigned int reserved[7];
	unsigned int flags;
	unsigned char raw[];
};

#define FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER 2
#define FS_IOC_ADD_ENCRYPTION_KEY \
	(0xC0000000UL | \
	 ((unsigned long)sizeof(struct fscrypt_add_key_arg) << 16) | \
	 ('f' << 8) | 23)

static int add_fscrypt_key(const char *path)
{
	static unsigned char argbuf[sizeof(struct fscrypt_add_key_arg) + 64]
		__attribute__((aligned(8)));
	struct fscrypt_add_key_arg *arg = (void *)argbuf;
	unsigned char key[64];
	unsigned char got[16];
	int kl;
	long fd;
	long rc;

	if (!PMOS_FSCRYPT_MASTER_KEY[0])
		return 0;

	kl = hex2bin(PMOS_FSCRYPT_MASTER_KEY, key, sizeof(key));
	if (kl < 16 || kl > 64) {
		kmsg("<3>PMOSINIT: fscrypt key must be 16-64 bytes\n");
		memset(key, 0, sizeof(key));
		return -22;
	}

	memset(argbuf, 0, sizeof(argbuf));
	arg->key_spec.type = FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER;
	arg->raw_size = (unsigned int)kl;
	memcpy(arg->raw, key, (unsigned long)kl);
	memset(key, 0, sizeof(key));

	fd = syscall4(56, AT_FDCWD, (long)path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) {
		klong("<3>PMOSINIT: open for fscrypt key failed rc=", fd);
		return (int)fd;
	}
	memset(got, 0, sizeof(got));
	rc = syscall3(29, fd, FS_IOC_ADD_ENCRYPTION_KEY, (long)arg);
	syscall3(57, fd, 0, 0);
	if (rc == 0)
		memcpy(got, arg->key_spec.u.identifier, sizeof(got));
	memset(argbuf, 0, sizeof(argbuf));

	if (rc == -17) {
		kmsg("<3>PMOSINIT: fscrypt key already present\n");
		return 0;
	}
	if (rc < 0) {
		klong("<3>PMOSINIT: add fscrypt key failed rc=", rc);
		return (int)rc;
	}

	kmsg_hex("<3>PMOSINIT: fscrypt key added, id=", got, 16);

	if (PMOS_FSCRYPT_IDENTIFIER[0]) {
		unsigned char want[16];
		if (hex2bin(PMOS_FSCRYPT_IDENTIFIER, want, sizeof(want)) != 16) {
			kmsg("<3>PMOSINIT: configured fscrypt identifier is invalid\n");
			return -22;
		}
		if (memcmp(want, got, 16) != 0) {
			kmsg("<3>PMOSINIT: fscrypt key identifier mismatch\n");
			return -1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* loop device                                                         */
/* ------------------------------------------------------------------ */

static int open_loop(int n, int img_fd, char *devpath,
		     unsigned long devpathsz, int *loop_fd_out)
{
	char path[32];
	char *p = path;
	char *end = path + sizeof(path) - 1;
	long fd;
	long rc;

	sappend(&p, end, "/dev/loop");
	sappend_num(&p, end, (unsigned long long)n);
	*p = '\0';

	syscall4(33, AT_FDCWD, (long)path, S_IFBLK | 0600,
		 (long)dev_encode(7, (unsigned int)n));
	fd = syscall4(56, AT_FDCWD, (long)path, O_RDWR | O_CLOEXEC, 0);
	if (fd < 0)
		return (int)fd;
	rc = syscall3(29, fd, LOOP_SET_FD, img_fd);
	if (rc < 0) {
		syscall3(57, fd, 0, 0);
		return (int)rc;
	}
	scopy(devpath, path, devpathsz);
	*loop_fd_out = (int)fd;
	return 0;
}

static int loop_attach(int img_fd, char *devpath, unsigned long devpathsz,
		       int *loop_fd_out)
{
	long cfd;
	long n;
	int i;

	syscall4(33, AT_FDCWD, (long)"/dev/loop-control",
		 S_IFCHR | 0600, (long)dev_encode(10, 237));
	cfd = syscall4(56, AT_FDCWD, (long)"/dev/loop-control",
		       O_RDWR | O_CLOEXEC, 0);
	if (cfd >= 0) {
		n = syscall3(29, cfd, LOOP_CTL_GET_FREE, 0);
		syscall3(57, cfd, 0, 0);
		if (n >= 0 &&
		    open_loop((int)n, img_fd, devpath, devpathsz,
			      loop_fd_out) == 0)
			return 0;
	}
	for (i = 0; i < 16; i++) {
		if (open_loop(i, img_fd, devpath, devpathsz,
			      loop_fd_out) == 0)
			return 0;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* mounts                                                              */
/* ------------------------------------------------------------------ */

static long mount_fs_flags(const char *dev, const char *mnt, const char *fs,
			   unsigned long flags, const char *opts)
{
	return syscall5(40, (long)dev, (long)mnt, (long)fs, (long)flags,
			(long)opts);
}

static long mount_fs(const char *dev, const char *mnt, const char *fs,
		     const char *opts)
{
	return mount_fs_flags(dev, mnt, fs, 0, opts);
}

#if PMOS_DATA_INLINECRYPT
static const char data_mount_opts[] = "inlinecrypt";
#else
static const char data_mount_opts[] = "";
#endif

/* erofs does not implement the inlinecrypt mount option. */
static const char *data_fs_opts(const char *fs)
{
	if (scmp(fs, "erofs") == 0)
		return "";
	return data_mount_opts;
}

static int mount_loop_rootfs(const char *imgpath, const char **mounted_target,
			     const char **mounted_fs)
{
	long img_fd;
	int loopfd = -1;
	char loopdev[32];
	int f;
	long rc;

	img_fd = syscall4(56, AT_FDCWD, (long)imgpath, O_RDWR | O_CLOEXEC, 0);
	if (img_fd < 0) {
		klong("<3>PMOSINIT: open rootfs image failed rc=", img_fd);
		return (int)img_fd;
	}
	rc = loop_attach((int)img_fd, loopdev, sizeof(loopdev), &loopfd);
	if (rc < 0) {
		klong("<3>PMOSINIT: loop attach failed rc=", rc);
		syscall3(57, img_fd, 0, 0);
		return (int)rc;
	}

	for (f = 0; f < (int)(sizeof(fs_types) / sizeof(fs_types[0])); f++) {
		rc = mount_fs(loopdev, "/root", fs_types[f], 0);
		if (rc == 0) {
			kmsg("<3>PMOSINIT: rootfs image mounted (");
			kmsg(fs_types[f]);
			kmsg(") via ");
			kmsg(loopdev);
			kmsg("\n");
			*mounted_target = "encrypted UFS loopback (/data/pmos/rootfs.img)";
			*mounted_fs = fs_types[f];
			syscall3(57, loopfd, 0, 0);
			syscall3(57, img_fd, 0, 0);
			return 0;
		}
	}
	klong("<3>PMOSINIT: mount rootfs image failed rc=", rc);
	syscall3(29, loopfd, LOOP_CLR_FD, 0);
	syscall3(57, loopfd, 0, 0);
	syscall3(57, img_fd, 0, 0);
	return (int)rc;
}

#define USERDATA_WAIT_ATTEMPTS 120

/*
 * One-shot diagnostics emitted every ten attempts.  They distinguish a
 * missing sysfs mount from a block device that has not enumerated yet, and
 * print the userdata uevent once UFS is up.
 */
static void userdata_diag(void)
{
	char b[256];
	long fd;
	long n;

	fd = syscall4(56, AT_FDCWD, (long)"/sys/class/block/sdc64/uevent",
		      O_RDONLY | O_CLOEXEC, 0);
	klong("<3>PMOSINIT: diag: sdc64 uevent open rc=", fd);
	if (fd >= 0) {
		n = syscall3(63, fd, (long)b, sizeof(b) - 1);
		syscall3(57, fd, 0, 0);
		klong("<3>PMOSINIT: diag: sdc64 uevent read rc=", n);
		if (n > 0) {
			b[n] = '\0';
			kmsg("<3>PMOSINIT: diag: sdc64 uevent: ");
			kmsg(b);
			kmsg("\n");
		}
	}

	fd = syscall4(56, AT_FDCWD, (long)"/sys/class/block",
		      O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
	klong("<3>PMOSINIT: diag: sysfs block open rc=", fd);
	if (fd >= 0)
		syscall3(57, fd, 0, 0);
}

static int try_userdata_rootfs(const char **mounted_target,
			       const char **mounted_fs)
{
	struct userdata_info ud;
	int have_ud = 0;
	int have_dm = 0;
	char dmdev[64];
	int attempt;
	int f;

	memset(&ud, 0, sizeof(ud));
	memset(dmdev, 0, sizeof(dmdev));

	for (attempt = 0; attempt < USERDATA_WAIT_ATTEMPTS; attempt++) {
		int d;
		int ndevs = 0;
		const char *devs[2];

		if (!have_ud) {
			if ((attempt % 10) == 0) {
				klong("<3>PMOSINIT: waiting for userdata, attempt ",
				      attempt);
				userdata_diag();
			}
			if (find_userdata(&ud) == 0)
				have_ud = 1;
		}
		if (have_ud) {
			if (PMOS_DM_METADATA_KEY[0]) {
				if (!have_dm) {
					int rc = setup_default_key(&ud, dmdev,
								   sizeof(dmdev));
					if (rc == 0)
						have_dm = 1;
					else
						klong("<3>PMOSINIT: dm setup failed rc=", rc);
				}
				if (have_dm)
					devs[ndevs++] = dmdev;
			}
			/* Raw is also tried: it is the correct path on devices
			 * without metadata encryption, and harmless otherwise. */
			devs[ndevs++] = ud.path;

			for (d = 0; d < ndevs; d++) {
				for (f = 0;
				     f < (int)(sizeof(fs_types) / sizeof(fs_types[0]));
				     f++) {
					long rc = mount_fs_flags(
						devs[d], "/data_host",
						fs_types[f],
						MS_NOATIME | MS_NOSUID |
							MS_NODEV,
						data_fs_opts(fs_types[f]));
					if (rc != 0) {
						kmsg("<3>PMOSINIT: mount userdata ");
						kmsg(fs_types[f]);
						kmsg(" via ");
						kmsg(devs[d]);
						kmsg(" failed rc=");
						klong("", rc);
						continue;
					}
					kmsg("<3>PMOSINIT: userdata mounted (");
					kmsg(fs_types[f]);
					kmsg(") on /data_host via ");
					kmsg(devs[d]);
					kmsg("\n");

					if (add_fscrypt_key("/data_host") != 0) {
						syscall3(39, (long)"/data_host", 0, 0);
						continue;
					}

					rc = mount_loop_rootfs(
						"/data_host/" PMOS_ROOTFS_SUBDIR
						"/" PMOS_ROOTFS_NAME,
						mounted_target, mounted_fs);
					if (rc == 0)
						return 0;
					syscall3(39, (long)"/data_host", 0, 0);
				}
			}
		}
		sleep_s(1);
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* diagnostics                                                         */
/* ------------------------------------------------------------------ */

static void dump_partitions(void)
{
	char buf[512];
	long fd = syscall4(56, AT_FDCWD, (long)"/proc/partitions",
			   O_RDONLY, 0);
	long n;
	if (fd >= 0) {
		kmsg("<3>PMOSINIT: === /proc/partitions ===\n");
		while ((n = syscall3(63, fd, (long)buf, sizeof(buf) - 1)) > 0) {
			buf[n] = '\0';
			kmsg(buf);
		}
		syscall3(57, fd, 0, 0);
		kmsg("<3>PMOSINIT: === end partitions ===\n");
	} else {
		klong("<3>PMOSINIT: /proc/partitions open failed rc=", fd);
	}
}

void _start(void)
{
	static char *argv[] = { "/sbin/init", 0 };
	static char *env[] = { "HOME=/", "TERM=linux",
			       "PATH=/bin:/sbin:/usr/bin:/usr/sbin", 0 };
	long rc = -1;
	int tries;
	int is_loopback = 0;
	const char *mounted_target = 0;
	const char *mounted_fs = 0;

	kmsg("<3>PMOSINIT: starting rootfs switch sequence (Attempt 279 - encrypted UFS loopback)\n");
	kmsg(PMOS_DM_METADATA_KEY[0] ?
	     "<3>PMOSINIT: storage config: metadata key present\n" :
	     "<3>PMOSINIT: storage config: no metadata key; raw userdata only\n");
	kmsg(PMOS_FSCRYPT_MASTER_KEY[0] ?
	     "<3>PMOSINIT: storage config: fscrypt key present\n" :
	     "<3>PMOSINIT: storage config: no fscrypt key\n");

	/* Create standard mountpoints */
	syscall3(34, AT_FDCWD, (long)"/dev", 0755);
	syscall3(34, AT_FDCWD, (long)"/proc", 0755);
	syscall3(34, AT_FDCWD, (long)"/sys", 0755);
	syscall3(34, AT_FDCWD, (long)"/root", 0755);
	syscall3(34, AT_FDCWD, (long)"/data_host", 0755);

	/* Mount core virtual filesystems */
	{
		long mdev = syscall5(40, (long)"devtmpfs", (long)"/dev",
				     (long)"devtmpfs", 0, 0);
		long mproc = syscall5(40, (long)"proc", (long)"/proc",
				      (long)"proc", 0, 0);
		long msys = syscall5(40, (long)"sysfs", (long)"/sys",
				     (long)"sysfs", 0, 0);

		klong("<3>PMOSINIT: mount devtmpfs rc=", mdev);
		klong("<3>PMOSINIT: mount proc rc=", mproc);
		klong("<3>PMOSINIT: mount sysfs rc=", msys);
	}

	/* Wait 3s for storage controllers (UFS & MSDC) to enumerate */
	sleep_s(3);

	dump_partitions();

	/* Explicitly create block device nodes for the legacy fallbacks. */
	syscall4(33, AT_FDCWD, (long)"/dev/loop0", S_IFBLK | 0660,
		 (7 << 8) | 0);
	syscall4(33, AT_FDCWD, (long)"/dev/mmcblk0", S_IFBLK | 0660,
		 (179 << 8) | 0);
	syscall4(33, AT_FDCWD, (long)"/dev/mmcblk0p1", S_IFBLK | 0660,
		 (179 << 8) | 1);
	syscall4(33, AT_FDCWD, (long)"/dev/mmcblk0p2", S_IFBLK | 0660,
		 (179 << 8) | 2);

	syscall4(33, AT_FDCWD, (long)"/dev/sdc", S_IFBLK | 0660,
		 (8 << 8) | 32);
	syscall4(33, AT_FDCWD, (long)"/dev/sdc57", S_IFBLK | 0660,
		 (259 << 8) | 41);
	syscall4(33, AT_FDCWD, (long)"/dev/sdc58", S_IFBLK | 0660,
		 (259 << 8) | 42);
	syscall4(33, AT_FDCWD, (long)"/dev/sdc62", S_IFBLK | 0660,
		 (259 << 8) | 46);
	syscall4(33, AT_FDCWD, (long)"/dev/sdc64", S_IFBLK | 0660,
		 (259 << 8) | 48);

	/* STEP 1: Android userdata -> dm-default-key -> fscrypt -> loop */
	rc = try_userdata_rootfs(&mounted_target, &mounted_fs);
	if (rc == 0) {
		is_loopback = 1;
	} else {
		kmsg("<3>PMOSINIT: encrypted/raw userdata rootfs path unavailable; trying legacy targets\n");
	}

	/* STEP 2: Legacy fixed targets (MicroSD, super, tee, etc.).
	 * Five passes are enough for late storage; more just floods pstore
	 * with expected failures on encrypted userdata. */
	for (tries = 0; tries < 5 && rc != 0; tries++) {
		int t;
		for (t = 0; t < (int)(sizeof(targets) / sizeof(targets[0])); t++) {
			int f;
			syscall4(33, AT_FDCWD, (long)targets[t].path,
				 S_IFBLK | 0660, targets[t].dev_t);
			for (f = 0;
			     f < (int)(sizeof(fs_types) / sizeof(fs_types[0]));
			     f++) {
				rc = syscall5(40, (long)targets[t].path,
					      (long)"/root",
					      (long)fs_types[f], 0, 0);
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
		kmsg("<3>PMOSINIT: mount SUCCESS! Switching root to postmarketOS (120s reboot watchdog active)...\n");

		/* Move mount /dev, /proc, /sys into rootfs */
		syscall3(34, AT_FDCWD, (long)"/root/dev", 0755);
		syscall3(34, AT_FDCWD, (long)"/root/proc", 0755);
		syscall3(34, AT_FDCWD, (long)"/root/sys", 0755);

		syscall5(40, (long)"/dev", (long)"/root/dev", 0, MS_MOVE, 0);
		syscall5(40, (long)"/proc", (long)"/root/proc", 0, MS_MOVE, 0);
		syscall5(40, (long)"/sys", (long)"/root/sys", 0, MS_MOVE, 0);

		if (is_loopback) {
			/* Keep userdata mounted under /root/data_host so the
			 * loop backing file, dm mapping and fscrypt key stay
			 * reachable after the root switch. */
			syscall3(34, AT_FDCWD, (long)"/root/data_host", 0755);
			syscall5(40, (long)"/data_host", (long)"/root/data_host",
				 0, MS_MOVE, 0);
		}

		/* Create essential character device nodes */
		syscall4(33, AT_FDCWD, (long)"/root/dev/console",
			 S_IFCHR | 0600, (5 << 8) | 1);
		syscall4(33, AT_FDCWD, (long)"/root/dev/null",
			 S_IFCHR | 0666, (1 << 8) | 3);
		syscall4(33, AT_FDCWD, (long)"/root/dev/zero",
			 S_IFCHR | 0666, (1 << 8) | 5);
		syscall4(33, AT_FDCWD, (long)"/root/dev/kmsg",
			 S_IFCHR | 0660, (1 << 8) | 11);
		syscall4(33, AT_FDCWD, (long)"/root/dev/tty0",
			 S_IFCHR | 0660, (4 << 8) | 0);
		syscall4(33, AT_FDCWD, (long)"/root/dev/tty1",
			 S_IFCHR | 0660, (4 << 8) | 1);
		syscall4(33, AT_FDCWD, (long)"/root/dev/tty7",
			 S_IFCHR | 0660, (4 << 8) | 7);
		syscall4(33, AT_FDCWD, (long)"/root/dev/tty",
			 S_IFCHR | 0666, (5 << 8) | 0);

		if (!syscall3(49, (long)"/root", 0, 0)) {
			syscall5(40, (long)".", (long)"/", 0, MS_MOVE, 0);
			syscall3(51, (long)".", 0, 0);

			kmsg("<3>PMOSINIT: executing /sbin/init (Phosh / OpenRC)...\n");
			long rc1 = syscall3(221, (long)"/sbin/init",
					    (long)argv, (long)env);
			klong("<3>PMOSINIT: /sbin/init execve failed rc=", rc1);
			long rc2 = syscall3(221, (long)"/usr/lib/systemd/systemd",
					    (long)argv, (long)env);
			klong("<3>PMOSINIT: /usr/lib/systemd/systemd execve failed rc=", rc2);
			long rc3 = syscall3(221, (long)"/init",
					    (long)argv, (long)env);
			klong("<3>PMOSINIT: /init execve failed rc=", rc3);
		}
	} else {
		klong("<3>PMOSINIT: FATAL: could not mount rootfs rc=", rc);
	}

	kmsg("<3>PMOSINIT: Boot failed or halted. Auto-rebooting in 5 seconds...\n");
	sleep_s(5);
	syscall4(142, 0xfee1dead, 672274793, 0x1234567, 0);
	for (;;) {
		sleep_s(1);
	}
}
