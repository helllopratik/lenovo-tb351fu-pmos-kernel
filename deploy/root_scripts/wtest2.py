import fcntl, os, struct, time
fd = os.open("/dev/spmpoke", os.O_RDONLY)
def pack(a,v,mask,count): return bytearray(struct.pack("QQII",a,v,mask,count))
def rd(a):
    b=pack(a,0,0,0); fcntl.ioctl(fd,0xC0185301,b); return struct.unpack("QQII",bytes(b))[1]
def wr(a,v):
    fcntl.ioctl(fd,0xC0185302,pack(a,v,0,0))
print("== ISLAND/POWER ==")
for a in (0x10006304,0x1000616c,0x10006170,0x10006000): print("  0x%08x = 0x%08x"%(a,rd(a)))
print("== DRIFT WATCH (6 x 0.4s) ==")
for i in range(6):
    print("  t%d : 21144=%08x 2610=%08x 2614=%08x 2618=%08x 2660=%08x 7104=%08x 7108=%08x"%(
        i,rd(0x18002144),rd(0x18002610),rd(0x18002614),rd(0x18002618),rd(0x18002660),rd(0x18007104),rd(0x18007108)))
    time.sleep(0.4)
print("== 0x1800701c bank / set test ==")
print("  before        : 0x%08x"%rd(0x1800701c))
wr(0x1800701c,0x0000ff80|0x100)
print("  after write   : 0x%08x"%rd(0x1800701c))
print("== conn_infra_peri bank 0x18007000..0x1800707c ==")
for a in range(0x18007000,0x18007080,4): print("  0x%08x = 0x%08x"%(a,rd(a)))
print("== connac 0x18002600..0x1800267c ==")
for a in range(0x18002600,0x18002680,4): print("  0x%08x = 0x%08x"%(a,rd(a)))
