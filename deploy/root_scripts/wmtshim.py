#!/usr/bin/env python3
"""wmtshim.py — v18 userspace pre-setup for wmt_drv.ko on the TB351FU (MT6789) pmOS port.

v18 = v17 with ONLY the SET-HIF ioctl arg corrected: 0x13 -> 0x23. The
bits[7:4] nibble of that arg is the FM-comm type (kernel wmt_lib_set_hif
stores (arg>>4)&0xf at conf+0xC = gDevWmt+0x104 = opid-0 payload byte D3),
NOT co-clock. Stock wmt_launcher packs arg = (high<<8)|(fm<<4)|stp with
fm=2, stp=3 -> 0x23; our 0x13 sent FM-comm 1, so the chip's opid-0 event
"configure FM comm" answered byte5 = 0x02 (its strap) vs our 0x01 and
failed the 1-byte verify -> assert retry storm -> sw_init -27 -> pwr-on
fail. One nibble, that was the whole bug.

v17 = v16 with the fd-holder upgraded from a pure sleeper to a POLL+READ
drain loop that mirrors the stock `wmt_launcher` binary layout
(/tmp/opencode/stock/wmt_full_dis.txt, decoded 2026-11-01).

Why: the v16 holder kept the fd open but never READ it. Stock main() (VA
0x5570) after the conf chain runs a blocking `poll([stpwmt fd], POLLIN,
timeout=-1)` -> `read(fd, buf, 0x100)` loop (VA 0x6650-0x6ab0) forever —
stock's kernel-side events are CONSUMED by the same process that holds the
fd. A held-but-never-read fd is NOT stock behavior: if the kernel-side rx
queue fills with delivered events (REG_EVT / kill / re-init msgs), the
producers may block/wedge. The v16 crash postmortem (console died abruptly
at t=34.2585 s mid-flood, no oops captured) is consistent with a wedge in
exactly that class of path, so v17 replicates stock's consumer loop.

Stock wmt_launcher ground truth (pulled /vendor/bin/wmt_launcher, 52 KB
aarch64 PIE, Android 36 linker; disassembly at
/tmp/opencode/stock/wmt_full_dis.txt):
  - main waits for prop `vendor.connsys.driver.ready` (300 ms poll loop,
    VA 0x5624-0x5654) BEFORE opening the node
  - fd = open("/dev/stpwmt", 0x102 = O_RDWR|O_NOCTTY), stored in a global
    at .data+0x260 (VA 0x5658-0x5670); on <0: log + usleep(300 ms) + retry
    (VA 0x5690-0x56c4). NOT O_CLOEXEC.
  - chip id via ioctl(fd, 0x8004A00C, NULL) (VA 0x4174, "chip_id from kernel
    by ioctl" — getter case in the kernel ioctl dispatch chain)
  - conf chain on the SAME fd (VA 0x60b0-0x6104):
      ioctl(0xC008A015, common-if-name buf)   # name from WMT.cfg
      ioctl(0x4004A005, value)                # SET-HIF, 4-byte VALUE arg
                                              # (same ABI as our v15/v16 call)
      ioctl(0x4004A00D, NULL)                 # 4-byte value arg
  - sigactions SIGCHLD(17)/SIGPIPE(13)/INT(2)/HUP(1)/TERM(15). The handler
    at VA 0x78d0 logs, sets global flag .data+0x2c0 (=1) and ioctls
    0x4004A00D with arg 1 (teardown flavor).
  - pwr_on_conn thread (fn 0x7960, arg 12): loops <=20x:
      ioctl(fd, 0x4004A007, ARG); on non-zero: ioctl(fd, 0x4004A007, NULL)
      + log + usleep(1 s). 0x4004A007 = connsys POWER-ON trigger. On our
      port the kernel wifi-on (`echo 1 > /dev/wmtWifi`) plays that role; we
      do NOT add this ioctl (v7: single attempt, no retries).
  - ROM patch table via ioctl 0x4008A01F (VA 0x77f4) — same as our shim.
  - watchdog (main thread): poll(fd, 1, -1) -> on POLLIN read(fd, <=0x100) ->
    dispatch (strncmp vs global keyword at .bss 0xc238); on "re-init"-class
    message or shutdown flag: close(fd) + detach worker + REOPEN retry —
    VA 0x6644-0x6664 is the ONLY close of the fd in the whole binary.
  - /proc/568/task on stock shows exactly ONE thread at idle (main):
    pwr_on_conn / dump_fwemi_log helper threads are transient
    (pthread_detach + exit after finishing).

v16 lesson that motivated the drain: the "stock holds no wmt fd" runtime
claim from the slot_a fd-watch was INVALID data — `ls /proc/568/fd` returns
EPERM for shell uid (hidepid), `2>/dev/null` swallowed it, so all 170 ticks
proved nothing. The binary decode is the deciding evidence instead; on pmOS
we are root, so fd presence is live-verifiable.

History:
  v15 proved the full BTIF front chain runs AND kernel-side STP state ends
  up stock-identical (decode: wmt_core_stp_init runs stp_open ->
  stp_enable(0) -> stp_ready(0) -> set_mode(8) -> stp_enable(1); stp_open
  sets bit2 of gDevWmt+0x1b0 which is the wmt_ctrl_stp_conf gate, so all
  steps executed. v15 = v14 with SET-HIF arg 0x14 -> 0x13: 0x14 picks the
  low-nib-4 branch -> conf[0]=1 (SDIO) and sent wmt_core_stp_init down the
  first-time SDIO arm where mtk_wcn_hif_sdio_wmt_control failed
  "no supported func probed" -> "SDIO_FUNC_STP func fail (-8)" -> pwr-on
  fail; 0x13 picks the BTIF branch (low-nib 3 -> conf[0]=2) but carried
  FM nibble 1 (v18 corrected the nibble -> 0x23).)

Replicates, over plain ioctls on /dev/stpwmt, the parts of the stock flow
pmOS is missing, BEFORE the first `echo 1 > /dev/wmtWifi`:

  1. WMT_IOCTL_SET_HIF  (0x4004a005)  arg passed as a VALUE (bitfield), not a buffer
        0x23  =  bits[3:0]=3  -> low-nib-3 branch -> conf[0]=2 (BTIF):
                   stp_if_tx_type(2), comm_if_type(2)
                  bits[7:4]=2 -> FM-comm type 2 -> stored at conf+0xC
                   (gDevWmt+0x104, opid-0 payload byte D3). STOCK-IDENTICAL:
                   wmt_launcher packs arg = (high<<8) | (fm<<4) | stp with
                   fm = bss+968 = 2 (BTIF) and stp = bss+964 = 3 (BTIF) -> 0x23
                   (default-path stamps [0xc3c4]=3 @0x6d44, [0xc3c8]=2
                   @0x582c/0x58cc; packers 0x53185d02 = ubfm<<8,
                   0x331c0d22 = bfi w2,w9,#4,#4, 0x33000d42 = bfi w2,w10,#0,#4
                   at 0x60e8-0x60f0; the high byte, likely the -o flag value
                   from bss+1500, is inert on the kernel BTIF path). v17 sent
                   0x13 with FM nibble 1: chip event byte5 answered 0x02
                   (strap) vs our 0x01 -> 1-byte verify fail -> "configure FM
                   comm fail" assert retry -> sw_init -27.
        (wmt_lib_set_hif @0x2d4dc: low-nib-3 branch writes conf[0]=2 (BTIF,
        HIF struct {hifType=2,fcCtrl=0} at gDevWmt+0x248) and skips the plat
        comm call; common tail @0x2d568 stores (arg>>4)&0xf into conf+0xC =
        gDevWmt+0x104 and returns -2 for FM nibbles outside {1,2}; the ioctl
        then queues opId 0 = WMT_OPID_HIF_CONF with the 40-byte struct and
        runs it synchronously — opfunc_hif_conf memcpy's it to
        gMtkWmtCtx+0x34, the ONLY runtime setter of eWmtHifType; it also sets
        the first-time bit gMtkWmtCtx+0x30 bit0 that wmt_core_stp_init
        requires (without it v13 aborted "no hif info!" -> pwr-on fail(-8)).
        The ioctl also latches its own hif_info flag after a successful set
        (kernel write @0x4eff0; a resend with hif_info==1 warns + returns 0
        without touching the chip), so send it exactly once per boot.)

  2. ROM patch table, WMT_IOCTL_SET_ROM_PATCH (0x4008a01f), one 264-byte entry per type:
        entry[0..3]   u32 type      (must be < 5)
        entry[4..7]   u32 EMI offset, little-endian ("4 address bytes". rom_patch_dwn
                      rebuilds the 24-bit value as (b[2]<<16)|(b[1]<<8)|b[0])
        entry[8..263] char name[256] — passed VERBATIM to kernel request_firmware()
        (WMT_unlocked_ioctl nr 0x1f -> copy_from_user 264 B -> wmt_lib_set_rom_patch_info
        @0x305e8: idempotent, kmalloc 264 B per type, stored at gDevWmt+0x22d8+type*8.)

        With the type-4 (MCU) slot set, op-34 (wmt_ctrl_get_rom_patch_info) skips the
        "srh_rom_patch" chip query that timed out 6 s in v13 ("failed to get patch
        (type: 0, ret: -1)" -> "can not go into idle loop!"), and serves patch info
        from the table instead. The patch BYTES are fetched kernel-side by
        wmt_dev_patch_get via request_firmware() -> the .bin files must exist in
        /lib/firmware/ on pmOS (stock got them from /vendor/firmware).
        Types with no entry print "NULL patchinfo pointer" + "no need to download (N)"
        — stock-normal noise for types 1/2.

  3. fix A (gWmtClose wall, from v16): while >=1 fds keep the node open,
     WMT_open has cleared gWmtClose; the rx path keeps delivering. The v15
     per-op open/close left gWmtClose=1 at wifi-on, wmt_dev_is_close() shed
     the chip REG EVT, and the STP handshake died with "Rx REG_EVT fail!(0)
     len(0,16)". Stock holds the fd for the process lifetime, so gWmtClose
     is 0 through every STP exchange.

  4. fix B (v17 drain): emulate stock's watchdog read so the kernel-side
     event queue never has a dead consumer.

Run me BEFORE `echo 1 > /dev/wmtWifi` (same boot, after wmt_drv.ko is loaded).
Safe to re-run: both ioctl paths are idempotent; every run adds one holder.

Ground truth = stock Android journal (wifi_toggle_dmesg.txt lines 692-712):
  Type 0: soc2_2_ram_bt_1d_1_hdr.bin    EmiOffset=0x170000 Size=0xa8e34
  Type 1: NULL (no need)                Type 2: NULL (no need)
  Type 3: soc2_2_ram_wifi_1d_1_hdr.bin  EmiOffset=0x2a0000 Size=0x3afe0
  Type 4: soc2_2_ram_mcu_1d_1_hdr.bin   EmiOffset=0x0      Size=0x30560
"""

import fcntl
import os
import select
import struct
import sys
import time

WMT_DEV = "/dev/stpwmt"  # WMT_unlocked_ioctl node: mtk_wcn_common_drv_init registers
                         # it as class "stpwmt" (major = mtk_stp_wmt, 190). There is no
                         # /dev/wmt on gen3 — wmtd's control channel is /dev/stpwmt.
WMT_SET_HIF = 0x4004A005  # IOW, type 0xA0, nr 0x05 — arg = bitfield value
WMT_SET_ROM_PATCH = 0x4008A01F  # IOW, type 0xA0, nr 0x1F — arg = 264-byte buffer
WMT_GET_CHIP_ID = 0x8004A00C  # IOR, type 0xA0, nr 0x0C — value getter; stock queries
                              # it right after opening the node (non-destructive)
WMT_READ_CHUNK = 0x100  # stock watchdog reads <=0x100 bytes per poll wake (VA 0x6a2c)

# type -> (firmware file name (request_firmware search: /lib/firmware), 24-bit EMI offset)
PATCH = {
    0: ("soc2_2_ram_bt_1d_1_hdr.bin", 0x170000),
    3: ("soc2_2_ram_wifi_1d_1_hdr.bin", 0x2A0000),
    4: ("soc2_2_ram_mcu_1d_1_hdr.bin", 0x000000),
}


def rom_patch_entry(rom_type, name, emi):
    """Pack one 264-byte ROM patch table entry."""
    entry = struct.pack("<II256s", rom_type, emi, name)
    assert len(entry) == 264, "entry must be 264 bytes"
    return entry


def check_bins():
    """Warn if a needed /lib/firmware file is missing (request_firmware would fail)."""
    for fname, _emi in PATCH.values():
        path = "/lib/firmware/" + fname
        if not os.path.exists(path):
            print("wmtshim: WARNING %s missing in /lib/firmware" % fname, file=sys.stderr)


def _kmsg(msg):
    """Best-effort kmsg line so v18 holder state stays visible from a shell."""
    try:
        with open("/dev/kmsg", "w") as fh:
            fh.write("<6>PMOSWIFI: v18 holder: %s\n" % msg)
    except OSError:
        pass


def spawn_wmt_holder(fd, pid_log="/run/wmt_holder.pid"):
    """v18 fix A+B: keep the WMT fd open AND drained for the life of this boot.

    Fork (never exec — the child must keep the inherited file description).
    Child = stock main() analog (binary VA 0x6650-0x6ab0):
      - poll([fd], POLLIN, 500 ms)  (stock uses timeout -1; ours just admits kill)
      - on POLLIN: ONE os.read(fd, 0x100) per wake — drains kernel-delivered
        events (the same read stock performs)
      - on 0-byte read / hard errors: log; on persistent failure reopen at
        300 ms (stock's open-retry loop VA 0x5690-0x56c4)
    Parent records the holder pid (re-running the shim stacks another holder,
    matching stock's 're-init' reopen semantics closely enough, all idempotent).
    Returns child pid, or 0 if the fork failed.
    """
    try:
        child = os.fork()
    except OSError as exc:
        print("wmtshim: v18 holder fork failed: %s" % exc, file=sys.stderr)
        return 0
    if child == 0:
        # holder child: drain loop, never execs, never writes back
        import signal
        signal.signal(signal.SIGHUP, signal.SIG_IGN)
        signal.signal(signal.SIGPIPE, signal.SIG_IGN)
        try:
            os.setsid()
        except OSError:
            pass
        try:
            os.close(0)
            os.close(1)
            os.close(2)
        except OSError:
            pass
        poller = select.poll()
        poller.register(fd, select.POLLIN)
        nread_bytes, nzero, nwake = 0, 0, 0
        while True:
            try:
                events = poller.poll(500)
            except (OSError, InterruptedError):
                continue
            if not events:
                continue
            if events[0][1] & (select.POLLIN | select.POLLPRI):
                nwake += 1
                try:
                    data = os.read(fd, WMT_READ_CHUNK)
                except OSError as exc:
                    _kmsg("evt#%d read err %s - reopening at 300ms" % (nwake, exc))
                    try:
                        time.sleep(0.3)
                        fd = os.open(WMT_DEV, os.O_RDWR | os.O_NOCTTY)
                        poller = select.poll()
                        poller.register(fd, select.POLLIN)
                        _kmsg("holder reopened fd ok")
                    except OSError as exc2:
                        _kmsg("holder reopen failed: %s - 1s backoff" % exc2)
                        time.sleep(1.0)
                    continue
                if data:
                    nread_bytes += len(data)
                    if nwake <= 5 or nwake % 50 == 0:
                        _kmsg("evt#%d read %d B (total %d B)"
                              % (nwake, len(data), nread_bytes))
                else:
                    nzero += 1
                    if nzero <= 3:
                        _kmsg("evt#%d read 0 B (EOF class)" % nwake)
            elif events[0][1] & (select.POLLERR | select.POLLHUP | select.POLLNVAL):
                _kmsg("poll err/hup revents=0x%x" % events[0][1])
                time.sleep(1.0)
    try:
        with open(pid_log, "w") as fh:
            fh.write("%d\n" % child)
    except OSError:
        pass
    return child


def main():
    try:
        # stock syscall is open(path, 0x102) = O_RDWR|O_NOCTTY (VA 0x5660) —
        # keep ours bit-identical (O_NOCTTY is a no-op for a chrdev).
        fd = os.open(WMT_DEV, os.O_RDWR | os.O_NOCTTY)
    except OSError as exc:
        print("wmtshim: cannot open %s (is wmt_drv.ko loaded?): %s" % (WMT_DEV, exc),
              file=sys.stderr)
        return 3
    # fix A: spawn the holder BEFORE any ioctl, so gWmtClose==0 from the very
    # first kernel interaction onwards (stock: open then instant conf).
    holder = spawn_wmt_holder(fd)
    if holder:
        print("wmtshim: v18 holder pid=%d (poll+read drain) keeps %s open (gWmtClose=0)"
              % (holder, WMT_DEV))
    else:
        print("wmtshim: WARN no holder; wifi-on will run with gWmtClose kept clear "
              "only while this process lives", file=sys.stderr)
    try:
        ret = fcntl.ioctl(fd, WMT_SET_HIF, 0x23)   # fm-comm 2 | stp 3 (stock-packed)
        print("wmtshim: SET-HIF(0x23) -> %r" % (ret,))
        if ret != 0:
            print("wmtshim: unexpected SET-HIF result", file=sys.stderr)
            return 2
    except OSError as exc:
        print("wmtshim: SET-HIF failed: %s" % exc, file=sys.stderr)
        return 2
    try:
        for rom_type in sorted(PATCH):
            fname, emi = PATCH[rom_type]
            ret = fcntl.ioctl(fd, WMT_SET_ROM_PATCH,
                              rom_patch_entry(rom_type, fname.encode(), emi))
            # The handler echoes the stored 264-byte entry back into the buffer
            # (python returns the modified copy). Print the short verdict only.
            if isinstance(ret, bytes) and len(ret) >= 4:
                echoed, = struct.unpack_from("<I", ret)
                ok = "echo-back type=%d ok" % echoed if echoed == rom_type else \
                     "echo-back type=%d MISMATCH" % echoed
            else:
                ok = repr(ret)
            print("wmtshim: SET-ROM-PATCH type=%d %s emi=0x%06x -> %s"
                  % (rom_type, fname, emi, ok))
    except OSError as exc:
        print("wmtshim: SET-ROM-PATCH failed: %s" % exc, file=sys.stderr)
        return 2
    # v17: stock queries the chip id right after open (VA 0x4174, read ioctl,
    # non-destructive). Its value cross-checks the chip-id state stock's flow
    # relies on; non-fatal if the wake direction differs in this kernel build.
    try:
        chip = fcntl.ioctl(fd, WMT_GET_CHIP_ID, 0)
        print("wmtshim: GET-CHIP-ID(0x8004a00c) -> %r" % (chip,))
    except OSError as exc:
        print("wmtshim: GET-CHIP-ID failed (non-fatal): %s" % exc, file=sys.stderr)
    # Parent exits here on every path; the holder child's inherited fd keeps
    # the kernel WMT refcount >=1 AND drains events like stock's watchdog.
    print("wmtshim: hif + rompatch were set; run `echo 1 > /dev/wmtWifi` now")
    return 0


if __name__ == "__main__":
    sys.exit(main())
