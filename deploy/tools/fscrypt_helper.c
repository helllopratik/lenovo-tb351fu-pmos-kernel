/* Freestanding aarch64 helper for fscrypt policy/key tests.
 * Reads policies and can add a new key / set a policy on an empty dir.
 * Never touches existing policies or keys.
 */
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;
typedef long s64;

#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_CLOEXEC 02000000

#define FSCRYPT_MAX_KEY_SIZE 64
#define FSCRYPT_KEY_DESCRIPTOR_SIZE 8
#define FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR 1
#define FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER 2
#define FSCRYPT_POLICY_V1 0
#define FSCRYPT_MODE_AES_256_XTS 1
#define FSCRYPT_MODE_AES_256_CTS 4
#define FSCRYPT_POLICY_FLAGS_PAD_16 0x02

#define IOC_NRBITS 8
#define IOC_TYPEBITS 8
#define IOC_SIZEBITS 14
#define IOC_DIRBITS 2
#define IOC_NRSHIFT 0
#define IOC_TYPESHIFT (IOC_NRSHIFT + IOC_NRBITS)
#define IOC_SIZESHIFT (IOC_TYPESHIFT + IOC_TYPEBITS)
#define IOC_DIRSHIFT (IOC_SIZESHIFT + IOC_SIZEBITS)
#define IOC_WRITE 1
#define IOC_READ 2
#define IOC(dir, type, nr, size) \
	(((dir) << IOC_DIRSHIFT) | ((type) << IOC_TYPESHIFT) | \
	 ((nr) << IOC_NRSHIFT) | ((size) << IOC_SIZESHIFT))

struct fscrypt_key_specifier {
	u32 type;
	u32 reserved;
	union {
		u8 reserved[32];
		u8 descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE];
		u8 identifier[16];
	} u;
};

struct fscrypt_add_key_arg {
	struct fscrypt_key_specifier key_spec;
	u32 raw_size;
	u32 key_id;
	u32 reserved[7];
	u32 flags;
	u8 raw[];
};

struct fscrypt_policy_v1 {
	u8 version;
	u8 contents_encryption_mode;
	u8 filenames_encryption_mode;
	u8 flags;
	u8 master_key_descriptor[FSCRYPT_KEY_DESCRIPTOR_SIZE];
};

struct fscrypt_policy_v2 {
	u8 version;
	u8 contents_encryption_mode;
	u8 filenames_encryption_mode;
	u8 flags;
	u8 log2_data_unit_size;
	u8 reserved[3];
	u8 master_key_identifier[16];
};

struct fscrypt_get_policy_ex_arg {
	u64 policy_size;
	union {
		u8 version;
		struct fscrypt_policy_v1 v1;
		u8 v2[24];
	} policy;
};

#define FS_IOC_GET_ENCRYPTION_POLICY_EX \
	IOC(IOC_READ | IOC_WRITE, 'f', 22, 9)
#define FS_IOC_ADD_ENCRYPTION_KEY \
	IOC(IOC_READ | IOC_WRITE, 'f', 23, sizeof(struct fscrypt_add_key_arg))
#define FS_IOC_REMOVE_ENCRYPTION_KEY \
	IOC(IOC_READ | IOC_WRITE, 'f', 24, 64)
#define FS_IOC_SET_ENCRYPTION_POLICY \
	IOC(IOC_READ, 'f', 19, sizeof(struct fscrypt_policy_v1))
#define FS_IOC_GET_ENCRYPTION_KEY_STATUS \
	IOC(IOC_READ | IOC_WRITE, 'f', 26, 128)

static long syscall3(long n, long a, long b, long c)
{
	register long x8 asm("x8") = n;
	register long x0 asm("x0") = a;
	register long x1 asm("x1") = b;
	register long x2 asm("x2") = c;
	asm volatile("svc #0" : "+r"(x0)
		     : "r"(x8), "r"(x1), "r"(x2) : "memory");
	return x0;
}

static long sys_open(const char *path, long flags)
{
	return syscall3(56, AT_FDCWD, (long)path, flags);
}

static long sys_ioctl(long fd, unsigned long req, void *arg)
{
	return syscall3(29, fd, (long)req, (long)arg);
}

static long sys_close(long fd)
{
	return syscall3(57, fd, 0, 0);
}

static long sys_write(long fd, const void *buf, unsigned long len)
{
	return syscall3(64, fd, (long)buf, len);
}

static long sys_read(long fd, void *buf, unsigned long len)
{
	return syscall3(63, fd, (long)buf, len);
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

static void puthex(const u8 *p, unsigned long n)
{
	static const char h[] = "0123456789abcdef";
	char out[3];
	for (unsigned long i = 0; i < n; i++) {
		out[0] = h[p[i] >> 4];
		out[1] = h[p[i] & 15];
		out[2] = ' ';
		sys_write(1, out, 3);
	}
	put("\n");
}

static void puthexc(const u8 *p, unsigned long n)
{
	static const char h[] = "0123456789abcdef";
	char out[2];
	for (unsigned long i = 0; i < n; i++) {
		out[0] = h[p[i] >> 4];
		out[1] = h[p[i] & 15];
		sys_write(1, out, 2);
	}
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
		buf[i++] = '0' + (u % 10);
		u /= 10;
	}
	if (neg)
		buf[i++] = '-';
	while (i--)
		sys_write(1, &buf[i], 1);
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

static int parse_hex(const char *s, u8 *out, unsigned long max)
{
	unsigned long n = 0;
	while (s[0] && s[1]) {
		int a = hexval(s[0]);
		int b = hexval(s[1]);
		if (a < 0 || b < 0 || n >= max)
			return -1;
		out[n++] = (u8)((a << 4) | b);
		s += 2;
	}
	if (*s)
		return -1;
	return (int)n;
}

static int read_hex_file(const char *path, u8 *out, unsigned long max)
{
	char buf[FSCRYPT_MAX_KEY_SIZE * 2 + 8];
	unsigned long start = 0, end;
	long fd, n;

	fd = sys_open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = sys_read(fd, buf, sizeof(buf) - 1);
	sys_close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	end = (unsigned long)n;
	while (start < end && (buf[start] == ' ' || buf[start] == '\t' ||
			       buf[start] == '\n' || buf[start] == '\r'))
		start++;
	while (end > start && (buf[end - 1] == ' ' || buf[end - 1] == '\t' ||
			       buf[end - 1] == '\n' || buf[end - 1] == '\r'))
		end--;
	buf[end] = '\0';
	return parse_hex(buf + start, out, max);
}

static void getpolicy(const char *path)
{
	u8 buf[sizeof(struct fscrypt_get_policy_ex_arg)] = {0};
	struct fscrypt_get_policy_ex_arg *arg =
		(struct fscrypt_get_policy_ex_arg *)buf;
	long fd = sys_open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		put("open error ");
		putnum(-fd);
		put("\n");
		return;
	}
	arg->policy_size = sizeof(arg->policy);
	long rc = sys_ioctl(fd, FS_IOC_GET_ENCRYPTION_POLICY_EX, arg);
	sys_close(fd);
	if (rc < 0) {
		put("no policy (rc=");
		putnum(rc);
		put(")\n");
		return;
	}
	put("policy_size=");
	putnum((long)arg->policy_size);
	put(" version=");
	putnum(arg->policy.version);
	put(" bytes: ");
	puthex(buf + 8, arg->policy_size);
}

/* Print only the v2 master-key identifier, in compact hex. */
static void getpolicy2ident(const char *path)
{
	u8 buf[sizeof(struct fscrypt_get_policy_ex_arg)] = {0};
	struct fscrypt_get_policy_ex_arg *arg =
		(struct fscrypt_get_policy_ex_arg *)buf;
	long fd = sys_open(path, O_RDONLY | O_CLOEXEC);
	long rc;
	if (fd < 0) {
		put("open_error=");
		putnum(-fd);
		put("\n");
		return;
	}
	arg->policy_size = sizeof(arg->policy);
	rc = sys_ioctl(fd, FS_IOC_GET_ENCRYPTION_POLICY_EX, arg);
	sys_close(fd);
	if (rc < 0) {
		put("no_policy rc=");
		putnum(rc);
		put("\n");
		return;
	}
	if (arg->policy_size < 24 || arg->policy.version != 2) {
		put("unsupported_policy version=");
		putnum(arg->policy.version);
		put("\n");
		return;
	}
	/* policy starts at buf + 8; v2 identifier starts 8 bytes in. */
	put("identifier=");
	puthexc(buf + 16, 16);
	put("\n");
}

/* Minimal status struct matching the kernel ABI (128 bytes). */
struct fscrypt_get_key_status_arg2 {
	struct fscrypt_key_specifier key_spec;
	u32 reserved[6];
	u32 status;
	u32 status_flags;
	u32 user_count;
	u32 out_reserved[13];
};

static void status(const char *path, const char *desc_hex)
{
	u8 desc[FSCRYPT_KEY_DESCRIPTOR_SIZE] = {0};
	u8 buf[sizeof(struct fscrypt_get_key_status_arg2)] = {0};
	struct fscrypt_get_key_status_arg2 *arg =
		(struct fscrypt_get_key_status_arg2 *)buf;
	if (parse_hex(desc_hex, desc, sizeof(desc)) != (int)sizeof(desc)) {
		put("bad descriptor\n");
		return;
	}
	arg->key_spec.type = FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR;
	for (int i = 0; i < 8; i++)
		arg->key_spec.u.descriptor[i] = desc[i];
	long fd = sys_open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		put("open error ");
		putnum(-fd);
		put("\n");
		return;
	}
	long rc = sys_ioctl(fd, FS_IOC_GET_ENCRYPTION_KEY_STATUS, arg);
	sys_close(fd);
	if (rc < 0) {
		put("status ioctl error ");
		putnum(-rc);
		put("\n");
		return;
	}
	put("status=");
	putnum(arg->status);
	put(" flags=");
	putnum(arg->status_flags);
	put(" users=");
	putnum(arg->user_count);
	put("\n");
}

static void addkey(const char *path, const char *desc_hex, const char *key_hex)
{
	u8 desc[FSCRYPT_KEY_DESCRIPTOR_SIZE] = {0};
	u8 key[FSCRYPT_MAX_KEY_SIZE] = {0};
	u8 buf[sizeof(struct fscrypt_add_key_arg) + FSCRYPT_MAX_KEY_SIZE] = {0};
	struct fscrypt_add_key_arg *arg = (struct fscrypt_add_key_arg *)buf;
	int dl = parse_hex(desc_hex, desc, sizeof(desc));
	int kl = parse_hex(key_hex, key, sizeof(key));
	if (dl != (int)sizeof(desc) || kl <= 0) {
		put("bad descriptor or key\n");
		return;
	}
	arg->key_spec.type = FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR;
	for (int i = 0; i < 8; i++)
		arg->key_spec.u.descriptor[i] = desc[i];
	arg->raw_size = (u32)kl;
	arg->key_id = 0;
	for (int i = 0; i < kl; i++)
		arg->raw[i] = key[i];
	long fd = sys_open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		put("open error ");
		putnum(-fd);
		put("\n");
		return;
	}
	long rc = sys_ioctl(fd, FS_IOC_ADD_ENCRYPTION_KEY, arg);
	sys_close(fd);
	put("addkey rc=");
	putnum(rc);
	if (rc < 0) {
		put(" errno=");
		putnum(-rc);
	}
	put("\n");
	for (unsigned long i = 0; i < sizeof(key); i++)
		key[i] = 0;
	for (unsigned long i = 0; i < sizeof(buf); i++)
		buf[i] = 0;
}

static void addkey2_do(const char *path, const char *key_hex,
		       const char *key_file)
{
	u8 key[FSCRYPT_MAX_KEY_SIZE] = {0};
	u8 buf[sizeof(struct fscrypt_add_key_arg) + FSCRYPT_MAX_KEY_SIZE] = {0};
	struct fscrypt_add_key_arg *arg = (struct fscrypt_add_key_arg *)buf;
	int kl;
	long fd, rc;

	kl = key_file ? read_hex_file(key_file, key, sizeof(key))
		      : parse_hex(key_hex, key, sizeof(key));
	if (kl <= 0) {
		put("bad key\n");
		return;
	}
	arg->key_spec.type = FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER;
	arg->raw_size = (u32)kl;
	for (int i = 0; i < kl; i++)
		arg->raw[i] = key[i];
	fd = sys_open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		put("open error ");
		putnum(-fd);
		put("\n");
		goto out;
	}
	rc = sys_ioctl(fd, FS_IOC_ADD_ENCRYPTION_KEY, arg);
	sys_close(fd);
	put("addkey2 rc=");
	putnum(rc);
	put("\n");
	if (rc == 0) {
		put("identifier=");
		puthexc(arg->key_spec.u.identifier, 16);
		put("\n");
	} else {
		put("errno=");
		putnum(-rc);
		put("\n");
	}
out:
	for (unsigned long i = 0; i < sizeof(key); i++)
		key[i] = 0;
	for (unsigned long i = 0; i < sizeof(buf); i++)
		buf[i] = 0;
}

static void addkey2(const char *path, const char *key_hex)
{
	addkey2_do(path, key_hex, 0);
}

static void addkey2f(const char *path, const char *key_file)
{
	addkey2_do(path, 0, key_file);
}

static void setpolicy2(const char *path, const char *ident_hex)
{
	u8 ident[16] = {0};
	struct fscrypt_policy_v2 pol;
	if (parse_hex(ident_hex, ident, sizeof(ident)) != (int)sizeof(ident)) {
		put("bad identifier\n");
		return;
	}
	for (unsigned long i = 0; i < sizeof(pol); i++)
		((u8 *)&pol)[i] = 0;
	pol.version = 2;
	pol.contents_encryption_mode = FSCRYPT_MODE_AES_256_XTS;
	pol.filenames_encryption_mode = FSCRYPT_MODE_AES_256_CTS;
	pol.flags = FSCRYPT_POLICY_FLAGS_PAD_16;
	pol.log2_data_unit_size = 0;
	for (int i = 0; i < 16; i++)
		pol.master_key_identifier[i] = ident[i];
	long fd = sys_open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		put("open error ");
		putnum(-fd);
		put("\n");
		return;
	}
	long rc = sys_ioctl(fd, FS_IOC_SET_ENCRYPTION_POLICY, &pol);
	sys_close(fd);
	put("setpolicy2 rc=");
	putnum(rc);
	if (rc < 0) {
		put(" errno=");
		putnum(-rc);
	}
	put("\n");
	for (unsigned long i = 0; i < sizeof(pol); i++)
		((u8 *)&pol)[i] = 0;
}

static void delkey2(const char *path, const char *ident_hex)
{
	u8 ident[16] = {0};
	u8 buf[64] = {0};
	if (parse_hex(ident_hex, ident, sizeof(ident)) != (int)sizeof(ident)) {
		put("bad identifier\n");
		return;
	}
	*(u32 *)(buf + 0) = 2; /* FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER */
	for (int i = 0; i < 16; i++)
		buf[8 + i] = ident[i];
	long fd = sys_open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		put("open error ");
		putnum(-fd);
		put("\n");
		return;
	}
	long rc = sys_ioctl(fd, FS_IOC_REMOVE_ENCRYPTION_KEY, buf);
	sys_close(fd);
	put("delkey2 rc=");
	putnum(rc);
	if (rc < 0) {
		put(" errno=");
		putnum(-rc);
	}
	put("\n");
	for (unsigned long i = 0; i < sizeof(buf); i++)
		buf[i] = 0;
}

static void delkey(const char *path, const char *desc_hex)
{
	u8 desc[FSCRYPT_KEY_DESCRIPTOR_SIZE] = {0};
	u8 buf[80] = {0};
	u8 *arg = buf;
	if (parse_hex(desc_hex, desc, sizeof(desc)) != (int)sizeof(desc)) {
		put("bad descriptor\n");
		return;
	}
	*(u32 *)(arg + 0) = FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR;
	for (int i = 0; i < 8; i++)
		arg[8 + i] = desc[i];
	long fd = sys_open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		put("open error ");
		putnum(-fd);
		put("\n");
		return;
	}
	long rc = sys_ioctl(fd, FS_IOC_REMOVE_ENCRYPTION_KEY, arg);
	sys_close(fd);
	put("delkey rc=");
	putnum(rc);
	if (rc < 0) {
		put(" errno=");
		putnum(-rc);
	}
	put("\n");
}

static void setpolicy(const char *path, const char *desc_hex)
{
	u8 desc[FSCRYPT_KEY_DESCRIPTOR_SIZE] = {0};
	struct fscrypt_policy_v1 pol;
	if (parse_hex(desc_hex, desc, sizeof(desc)) != (int)sizeof(desc)) {
		put("bad descriptor\n");
		return;
	}
	for (unsigned long i = 0; i < sizeof(pol); i++)
		((u8 *)&pol)[i] = 0;
	pol.version = FSCRYPT_POLICY_V1;
	pol.contents_encryption_mode = FSCRYPT_MODE_AES_256_XTS;
	pol.filenames_encryption_mode = FSCRYPT_MODE_AES_256_CTS;
	pol.flags = FSCRYPT_POLICY_FLAGS_PAD_16;
	for (int i = 0; i < 8; i++)
		pol.master_key_descriptor[i] = desc[i];
	long fd = sys_open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		put("open error ");
		putnum(-fd);
		put("\n");
		return;
	}
	long rc = sys_ioctl(fd, FS_IOC_SET_ENCRYPTION_POLICY, &pol);
	sys_close(fd);
	put("setpolicy rc=");
	putnum(rc);
	if (rc < 0) {
		put(" errno=");
		putnum(-rc);
	}
	put("\n");
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		put("usage: helper getpolicy PATH | getpolicy2ident PATH | "
		    "status PATH DESC | addkey PATH DESC KEY | "
		    "addkey2 PATH KEY | addkey2f PATH KEYFILE | "
		    "delkey PATH DESC | delkey2 PATH IDENT | "
		    "setpolicy PATH DESC | setpolicy2 PATH IDENT\n");
		return 2;
	}
	if (!scmp(argv[1], "getpolicy")) {
		getpolicy(argv[2]);
	} else if (!scmp(argv[1], "getpolicy2ident")) {
		getpolicy2ident(argv[2]);
	} else if (!scmp(argv[1], "status")) {
		status(argv[2], argv[3]);
	} else if (!scmp(argv[1], "addkey")) {
		addkey(argv[2], argv[3], argv[4]);
	} else if (!scmp(argv[1], "addkey2")) {
		addkey2(argv[2], argv[3]);
	} else if (!scmp(argv[1], "addkey2f")) {
		addkey2f(argv[2], argv[3]);
	} else if (!scmp(argv[1], "delkey")) {
		delkey(argv[2], argv[3]);
	} else if (!scmp(argv[1], "delkey2")) {
		delkey2(argv[2], argv[3]);
	} else if (!scmp(argv[1], "setpolicy")) {
		setpolicy(argv[2], argv[3]);
	} else if (!scmp(argv[1], "setpolicy2")) {
		setpolicy2(argv[2], argv[3]);
	} else {
		put("unknown command\n");
		return 2;
	}
	return 0;
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
