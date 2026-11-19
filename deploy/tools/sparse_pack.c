/* SPDX-License-Identifier: MIT */
/* Freestanding aarch64 sparse pack applier/verifier.
 *
 * A pack carries only the allocated extents of a sparse image, so adb never
 * has to transfer a hole as allocated data.  Pack format (little endian):
 *
 *   magic "PMOSPK01"          8 bytes
 *   logical_size              u64
 *   extent_count              u64
 *   for each extent:
 *     offset                  u64
 *     length                  u64
 *     data                    length bytes
 *     crc32                   u32   (standard CRC-32 of data)
 *
 * Usage: sparse_pack apply PACK DST
 *        sparse_pack verify PACK DST
 *        sparse_pack pack SRC PACK
 */

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

#define AT_FDCWD (-100)
#define O_RDONLY 00000000
#define O_WRONLY 00000001
#define O_CREAT  00000100
#define O_TRUNC  00001000
#define O_CLOEXEC 02000000
#define SEEK_END 2
#define SEEK_DATA 3
#define SEEK_HOLE 4
#define ENXIO 6
#define MAX_EXTENTS (1ULL << 20)

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
	return syscall4(67, fd, (long)buf, (long)len, (long)off);
}

static long sys_pwrite(long fd, const void *buf, u64 len, u64 off)
{
	return syscall4(68, fd, (long)buf, (long)len, (long)off);
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

static void putu64(u64 v)
{
	char b[24];
	int i = 0;
	if (!v)
		b[i++] = '0';
	while (v) {
		b[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	while (i--)
		sys_write(1, &b[i], 1);
}

static u32 get_u32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

static u64 get_u64(const u8 *p)
{
	u64 v = 0;
	int i;
	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static void set_u32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static void set_u64(u8 *p, u64 v)
{
	int i;
	for (i = 0; i < 8; i++) {
		p[i] = (u8)v;
		v >>= 8;
	}
}

static u32 crc_table[256];

static void crc_init(void)
{
	u32 i;
	int k;
	for (i = 0; i < 256; i++) {
		u32 c = i;
		for (k = 0; k < 8; k++)
			c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		crc_table[i] = c;
	}
}

static u32 crc_start(void)
{
	return 0xFFFFFFFFu;
}

static u32 crc_update(u32 c, const u8 *p, u64 n)
{
	while (n--)
		c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
	return c;
}

static u32 crc_finish(u32 c)
{
	return ~c;
}

static int bytes_eq(const u8 *a, const u8 *b, u64 n)
{
	while (n--) {
		if (*a++ != *b++)
			return 0;
	}
	return 1;
}

/* 1 MiB streaming buffer plus a comparison buffer for verify. */
static u8 buf[1 << 20];
static u8 buf2[1 << 20];

static int read_full(long fd, void *dst, u64 len)
{
	u8 *p = dst;
	while (len) {
		u64 want = len > sizeof(buf) ? sizeof(buf) : len;
		long r = sys_read(fd, p, want);
		if (r <= 0)
			return -1;
		p += r;
		len -= (u64)r;
	}
	return 0;
}

static int read_at(long fd, void *dst, u64 len, u64 off)
{
	u8 *p = dst;
	while (len) {
		u64 want = len > sizeof(buf) ? sizeof(buf) : len;
		long r = sys_pread(fd, p, want, off);
		if (r <= 0) {
			put("pread failed err=");
			putu64(r < 0 ? (u64)(-r) : 0);
			put(" offset=");
			putu64(off);
			put(" len=");
			putu64(want);
			put("\n");
			return -1;
		}
		p += r;
		off += (u64)r;
		len -= (u64)r;
	}
	return 0;
}

static int write_at(long fd, const void *src, u64 len, u64 off)
{
	const u8 *p = src;
	while (len) {
		u64 want = len > sizeof(buf) ? sizeof(buf) : len;
		long w = sys_pwrite(fd, p, want, off);
		if (w <= 0) {
			put("pwrite failed err=");
			putu64(w < 0 ? (u64)(-w) : 0);
			put(" offset=");
			putu64(off);
			put("\n");
			return -1;
		}
		p += w;
		off += (u64)w;
		len -= (u64)w;
	}
	return 0;
}

static int open_pack(const char *path, long *pfd, u64 *size, u64 *count)
{
	u8 hdr[24];
	long fd;

	fd = sys_open(path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (read_full(fd, hdr, sizeof(hdr)) != 0)
		goto bad;
	if (hdr[0] != 'P' || hdr[1] != 'M' || hdr[2] != 'O' ||
	    hdr[3] != 'S' || hdr[4] != 'P' || hdr[5] != 'K' ||
	    hdr[6] != '0' || hdr[7] != '1')
		goto bad;
	*size = get_u64(hdr + 8);
	*count = get_u64(hdr + 16);
	if (*count > MAX_EXTENTS)
		goto bad;
	*pfd = fd;
	return 0;
bad:
	sys_close(fd);
	return -1;
}

static int fail(const char *msg)
{
	put("error: ");
	put(msg);
	put("\n");
	return 1;
}

static int do_apply(const char *pack, const char *dst)
{
	long pfd, dfd;
	u64 size = 0, count = 0, data_bytes = 0, e, done;
	u8 hdr[16], tr[4];

	if (open_pack(pack, &pfd, &size, &count) != 0)
		return fail("cannot read pack header");
	/* Do not use O_TRUNC: the destination may be a block device (the
	 * offline whole-device installer writes userdata directly).  A regular
	 * file is resized with ftruncate; on a block device ftruncate fails with
	 * EINVAL and the device size is checked with SEEK_END instead. */
	dfd = sys_open(dst, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
	if (dfd < 0) {
		sys_close(pfd);
		return fail("cannot open destination");
	}
	if (sys_ftruncate(dfd, (long)size) != 0) {
		long dsize = sys_lseek(dfd, 0, SEEK_END);
		/* A block device cannot be truncated.  It is acceptable as long
		 * as it is at least as large as the image. */
		if (dsize < 0 || (u64)dsize < size) {
			sys_close(dfd);
			sys_close(pfd);
			return fail("destination size mismatch");
		}
	}

	for (e = 0; e < count; e++) {
		u64 off, len;
		u32 crc;

		if (read_full(pfd, hdr, sizeof(hdr)) != 0)
			goto io_error;
		off = get_u64(hdr);
		len = get_u64(hdr + 8);
		if (!len || off > size || len > size - off)
			goto format_error;

		crc = crc_start();
		done = 0;
		while (done < len) {
			u64 chunk = len - done;
			if (chunk > sizeof(buf))
				chunk = sizeof(buf);
			if (read_full(pfd, buf, chunk) != 0)
				goto io_error;
			crc = crc_update(crc, buf, chunk);
			if (write_at(dfd, buf, chunk, off + done) != 0)
				goto io_error;
			done += chunk;
		}
		if (read_full(pfd, tr, sizeof(tr)) != 0)
			goto io_error;
		if (crc_finish(crc) != get_u32(tr)) {
			sys_close(dfd);
			sys_close(pfd);
			return fail("extent checksum mismatch");
		}
		data_bytes += len;
	}

	if (sys_fsync(dfd) != 0) {
		put("fsync failed\n");
		goto io_error;
	}
	sys_close(dfd);
	sys_close(pfd);
	put("applied extents=");
	putu64(count);
	put(" logical_size=");
	putu64(size);
	put(" data_bytes=");
	putu64(data_bytes);
	put("\n");
	return 0;

format_error:
	sys_close(dfd);
	sys_close(pfd);
	return fail("invalid extent in pack");
io_error:
	sys_close(dfd);
	sys_close(pfd);
	return fail("I/O error while applying pack");
}

static int do_verify(const char *pack, const char *dst)
{
	long pfd, dfd, dsize;
	u64 size = 0, count = 0, data_bytes = 0, e, done;
	u8 hdr[16], tr[4];

	if (open_pack(pack, &pfd, &size, &count) != 0)
		return fail("cannot read pack header");
	dfd = sys_open(dst, O_RDONLY | O_CLOEXEC, 0);
	if (dfd < 0) {
		sys_close(pfd);
		return fail("cannot open destination");
	}
	dsize = sys_lseek(dfd, 0, SEEK_END);
	if (dsize < 0 || (u64)dsize < size) {
		sys_close(dfd);
		sys_close(pfd);
		return fail("destination size mismatch");
	}

	for (e = 0; e < count; e++) {
		u64 off, len;
		u32 crc;

		if (read_full(pfd, hdr, sizeof(hdr)) != 0)
			goto io_error;
		off = get_u64(hdr);
		len = get_u64(hdr + 8);
		if (!len || off > size || len > size - off)
			goto format_error;

		crc = crc_start();
		done = 0;
		while (done < len) {
			u64 chunk = len - done;
			if (chunk > sizeof(buf))
				chunk = sizeof(buf);
			if (read_full(pfd, buf, chunk) != 0)
				goto io_error;
			crc = crc_update(crc, buf, chunk);
			if (read_at(dfd, buf2, chunk, off + done) != 0)
				goto io_error;
			if (!bytes_eq(buf, buf2, chunk)) {
				sys_close(dfd);
				sys_close(pfd);
				return fail("destination data mismatch");
			}
			done += chunk;
		}
		if (read_full(pfd, tr, sizeof(tr)) != 0)
			goto io_error;
		if (crc_finish(crc) != get_u32(tr)) {
			sys_close(dfd);
			sys_close(pfd);
			return fail("extent checksum mismatch");
		}
		data_bytes += len;
	}

	sys_close(dfd);
	sys_close(pfd);
	put("verified extents=");
	putu64(count);
	put(" logical_size=");
	putu64(size);
	put(" data_bytes=");
	putu64(data_bytes);
	put("\n");
	return 0;

format_error:
	sys_close(dfd);
	sys_close(pfd);
	return fail("invalid extent in pack");
io_error:
	sys_close(dfd);
	sys_close(pfd);
	return fail("I/O error while verifying pack");
}

static int emit_extent(long sfd, long dfd, u64 *pos, u64 off, u64 len,
		       u64 *data_bytes)
{
	u8 ext[16], tr[4];
	u32 crc = crc_start();
	u64 done = 0;

	set_u64(ext, off);
	set_u64(ext + 8, len);
	if (write_at(dfd, ext, sizeof(ext), *pos) != 0)
		return -1;
	*pos += sizeof(ext);

	while (done < len) {
		u64 chunk = len - done;
		if (chunk > sizeof(buf))
			chunk = sizeof(buf);
		if (read_at(sfd, buf, chunk, off + done) != 0)
			return -1;
		if (write_at(dfd, buf, chunk, *pos) != 0)
			return -1;
		crc = crc_update(crc, buf, chunk);
		*pos += chunk;
		done += chunk;
	}

	set_u32(tr, crc_finish(crc));
	if (write_at(dfd, tr, sizeof(tr), *pos) != 0)
		return -1;
	*pos += sizeof(tr);
	*data_bytes += len;
	return 0;
}

static int do_pack(const char *src, const char *pack)
{
	long sfd, dfd, end;
	u64 size, count = 0, data_bytes = 0, pos = 24, off = 0;
	u8 hdr[24];

	sfd = sys_open(src, O_RDONLY | O_CLOEXEC, 0);
	if (sfd < 0)
		return fail("cannot open source");
	end = sys_lseek(sfd, 0, SEEK_END);
	if (end < 0) {
		sys_close(sfd);
		return fail("cannot determine source size");
	}
	size = (u64)end;

	dfd = sys_open(pack, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (dfd < 0) {
		sys_close(sfd);
		return fail("cannot open pack output");
	}

	hdr[0] = 'P';
	hdr[1] = 'M';
	hdr[2] = 'O';
	hdr[3] = 'S';
	hdr[4] = 'P';
	hdr[5] = 'K';
	hdr[6] = '0';
	hdr[7] = '1';
	set_u64(hdr + 8, size);
	set_u64(hdr + 16, 0);
	if (write_at(dfd, hdr, sizeof(hdr), 0) != 0)
		goto io_error;

	while (off < size) {
		long data = sys_lseek(sfd, (long)off, SEEK_DATA);
		long hole;
		u64 start, stop;

		if (data < 0) {
			/* ENXIO means there is no more data; any other error
			 * means SEEK_DATA is unsupported, so transfer the
			 * remaining bytes as one extent. */
			if (data != -ENXIO) {
				if (emit_extent(sfd, dfd, &pos, off, size - off,
						&data_bytes) != 0)
					goto io_error;
				count++;
			}
			break;
		}
		start = (u64)data;
		if (start >= size)
			break;
		hole = sys_lseek(sfd, data, SEEK_HOLE);
		stop = hole < 0 ? size : (u64)hole;
		if (stop <= start)
			break;
		if (stop > size)
			stop = size;
		if (count >= MAX_EXTENTS)
			goto format_error;
		if (emit_extent(sfd, dfd, &pos, start, stop - start,
				&data_bytes) != 0)
			goto io_error;
		count++;
		off = stop;
	}

	/* A filesystem may report no extents for a non-empty file.  Keep the
	 * pack usable by falling back to a full extent. */
	if (!count && size) {
		if (emit_extent(sfd, dfd, &pos, 0, size, &data_bytes) != 0)
			goto io_error;
		count = 1;
	}

	set_u64(hdr + 16, count);
	if (write_at(dfd, hdr, sizeof(hdr), 0) != 0)
		goto io_error;
	if (sys_fsync(dfd) != 0)
		goto io_error;

	sys_close(dfd);
	sys_close(sfd);
	put("packed extents=");
	putu64(count);
	put(" logical_size=");
	putu64(size);
	put(" data_bytes=");
	putu64(data_bytes);
	put("\n");
	return 0;

format_error:
	sys_close(dfd);
	sys_close(sfd);
	return fail("too many extents");
io_error:
	sys_close(dfd);
	sys_close(sfd);
	return fail("I/O error while packing");
}

int main(int argc, char **argv)
{
	if (argc != 4) {
		put("usage: sparse_pack apply|verify PACK DST\n");
		put("       sparse_pack pack SRC PACK\n");
		return 2;
	}
	crc_init();
	if (!scmp(argv[1], "apply"))
		return do_apply(argv[2], argv[3]);
	if (!scmp(argv[1], "verify"))
		return do_verify(argv[2], argv[3]);
	if (!scmp(argv[1], "pack"))
		return do_pack(argv[2], argv[3]);
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
		"svc #0");
}
