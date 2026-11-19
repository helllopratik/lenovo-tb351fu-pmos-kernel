/* Freestanding aarch64 sparse file copy/verify helper.
 * Copies only data extents (SEEK_DATA/SEEK_HOLE) and truncates to the
 * logical size, so holes stay holes.  "verify" compares only the source
 * data extents plus the logical size.
 *
 * Usage: sparse_copy copy SRC DST
 *        sparse_copy verify SRC DST
 */

typedef unsigned char u8;
typedef unsigned long u64;

#define AT_FDCWD (-100)
#define O_RDONLY 00000000
#define O_WRONLY 00000001
#define O_CREAT  00000100
#define O_TRUNC  00001000
#define O_CLOEXEC 02000000
#define SEEK_DATA 3
#define SEEK_HOLE 4

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

static long sys_open(const char *path, long flags, long mode)
{
	return syscall4(56, AT_FDCWD, (long)path, flags, mode);
}

static long sys_read(long fd, void *buf, u64 len)
{
	return syscall3(63, fd, (long)buf, (long)len);
}

static long sys_write(long fd, const void *buf, u64 len)
{
	return syscall3(64, fd, (long)buf, (long)len);
}

static long sys_pread(long fd, void *buf, u64 len, u64 off)
{
	register long x8 asm("x8") = 67;
	register long x0 asm("x0") = fd;
	register long x1 asm("x1") = (long)buf;
	register long x2 asm("x2") = (long)len;
	register long x3 asm("x3") = (long)off;
	asm volatile("svc #0" : "+r"(x0)
		     : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
		     : "memory", "cc");
	return x0;
}

static long sys_pwrite(long fd, const void *buf, u64 len, u64 off)
{
	register long x8 asm("x8") = 68;
	register long x0 asm("x0") = fd;
	register long x1 asm("x1") = (long)buf;
	register long x2 asm("x2") = (long)len;
	register long x3 asm("x3") = (long)off;
	asm volatile("svc #0" : "+r"(x0)
		     : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
		     : "memory", "cc");
	return x0;
}

static long sys_lseek(long fd, long off, long whence)
{
	return syscall3(62, fd, off, whence);
}

static long sys_ftruncate(long fd, long len)
{
	return syscall3(46, fd, len, 0);
}

static long sys_fsync(long fd)
{
	return syscall3(82, fd, 0, 0);
}

static long sys_close(long fd)
{
	return syscall3(57, fd, 0, 0);
}

static long sys_exit(long code)
{
	return syscall3(93, code, 0, 0);
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

static void put(const char *s)
{
	sys_write(1, s, slen(s));
}

static void putnum(long v)
{
	char buf[32];
	int i = 0;
	int neg = 0;
	unsigned long u;
	if (v < 0) {
		neg = 1;
		u = (unsigned long)(-v);
	} else {
		u = (unsigned long)v;
	}
	if (!u)
		buf[i++] = '0';
	while (u) {
		buf[i++] = '0' + (char)(u % 10);
		u /= 10;
	}
	if (neg)
		buf[i++] = '-';
	while (i--)
		sys_write(1, &buf[i], 1);
}

static void putsz(const char *label, u64 v)
{
	put(label);
	putnum((long)v);
	put("\n");
}

static u8 buf[1 << 20];
static u8 buf2[1 << 20];

static int copy_range(long sfd, long dfd, u64 start, u64 end)
{
	u64 pos = start;

	while (pos < end) {
		u64 want = end - pos;
		long r, done;
		if (want > sizeof(buf))
			want = sizeof(buf);
		r = sys_pread(sfd, buf, want, pos);
		if (r <= 0)
			return -1;
		done = 0;
		while (done < r) {
			long w = sys_pwrite(dfd, buf + done, (u64)(r - done),
					    pos + (u64)done);
			if (w <= 0)
				return -1;
			done += w;
		}
		pos += (u64)r;
	}
	return 0;
}

static int do_copy(const char *src, const char *dst)
{
	long sfd, dfd, size, off = 0;
	u64 data_bytes = 0;

	sfd = sys_open(src, O_RDONLY | O_CLOEXEC, 0);
	if (sfd < 0) {
		put("open src failed ");
		putnum(-sfd);
		put("\n");
		return 1;
	}
	dfd = sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (dfd < 0) {
		put("open dst failed ");
		putnum(-dfd);
		put("\n");
		sys_close(sfd);
		return 1;
	}
	size = sys_lseek(sfd, 0, 2 /*SEEK_END*/);
	if (size < 0) {
		put("src size failed\n");
		sys_close(dfd);
		sys_close(sfd);
		return 1;
	}
	while (off < size) {
		long d = sys_lseek(sfd, off, SEEK_DATA);
		long h;
		if (d < 0)
			break; /* ENXIO: no more data */
		h = sys_lseek(sfd, d, SEEK_HOLE);
		if (h < 0 || h > size)
			h = size;
		if (h <= d)
			break;
		if (copy_range(sfd, dfd, (u64)d, (u64)h) != 0) {
			put("copy failed at ");
			putnum(d);
			put("\n");
			sys_close(dfd);
			sys_close(sfd);
			return 1;
		}
		data_bytes += (u64)(h - d);
		off = h;
	}
	if (sys_ftruncate(dfd, size) != 0) {
		put("ftruncate failed\n");
		sys_close(dfd);
		sys_close(sfd);
		return 1;
	}
	sys_fsync(dfd);
	sys_close(dfd);
	sys_close(sfd);
	putsz("logical_size=", (u64)size);
	putsz("data_bytes=", data_bytes);
	putsz("hole_bytes=", (u64)size - data_bytes);
	return 0;
}

static int do_verify(const char *src, const char *dst)
{
	long sfd, dfd, size, dsize, off = 0;
	u64 compared = 0;

	sfd = sys_open(src, O_RDONLY | O_CLOEXEC, 0);
	if (sfd < 0) {
		put("open src failed ");
		putnum(-sfd);
		put("\n");
		return 1;
	}
	dfd = sys_open(dst, O_RDONLY | O_CLOEXEC, 0);
	if (dfd < 0) {
		put("open dst failed ");
		putnum(-dfd);
		put("\n");
		sys_close(sfd);
		return 1;
	}
	size = sys_lseek(sfd, 0, 2);
	dsize = sys_lseek(dfd, 0, 2);
	if (size < 0 || dsize != size) {
		put("size mismatch src=");
		putnum(size);
		put(" dst=");
		putnum(dsize);
		put("\n");
		sys_close(dfd);
		sys_close(sfd);
		return 1;
	}
	while (off < size) {
		long d = sys_lseek(sfd, off, SEEK_DATA);
		long h;
		if (d < 0)
			break;
		h = sys_lseek(sfd, d, SEEK_HOLE);
		if (h < 0 || h > size)
			h = size;
		if (h <= d)
			break;
		{
			u64 pos = (u64)d;
			while (pos < (u64)h) {
				u64 want = (u64)h - pos;
				long r1, r2, i;
				if (want > sizeof(buf))
					want = sizeof(buf);
				r1 = sys_pread(sfd, buf, want, pos);
				if (r1 <= 0)
					goto bad;
				r2 = sys_pread(dfd, buf2, want, pos);
				if (r2 != r1)
					goto bad;
				for (i = 0; i < r1; i++) {
					if (buf[i] != buf2[i])
						goto bad;
				}
				pos += (u64)r1;
			}
		}
		compared += (u64)(h - d);
		off = h;
	}
	putsz("verified_size=", (u64)size);
	putsz("verified_data_bytes=", compared);
	sys_close(dfd);
	sys_close(sfd);
	return 0;

bad:
	put("verify mismatch near ");
	putnum(off);
	put("\n");
	sys_close(dfd);
	sys_close(sfd);
	return 1;
}

int main(int argc, char **argv)
{
	if (argc != 4) {
		put("usage: sparse_copy copy|verify SRC DST\n");
		return 2;
	}
	if (!scmp(argv[1], "copy"))
		return do_copy(argv[2], argv[3]);
	if (!scmp(argv[1], "verify"))
		return do_verify(argv[2], argv[3]);
	put("unknown mode\n");
	return 2;
}

__attribute__((naked)) void _start(void)
{
	asm volatile(
		"mov x1, sp\n"
		"ldr w0, [x1], #8\n"
		"bl main\n"
		"mov x8, #93\n"
		"svc #0\n");
}
