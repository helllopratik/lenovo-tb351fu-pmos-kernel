import fcntl, os, struct, sys
RD = 0xC0185301
fd = os.open("/dev/spmpoke", os.O_RDONLY)
for a in sys.argv[1:]:
    p = int(a, 16)
    buf = bytearray(struct.pack("QQII", p, 0, 0, 0))
    try:
        rc = fcntl.ioctl(fd, RD, buf)
        v = struct.unpack("QQII", bytes(buf))[1]
        print("0x%08x = 0x%08x (rc=%s)" % (p, v, rc))
    except OSError as e:
        print("0x%08x ERR %s" % (p, e))
