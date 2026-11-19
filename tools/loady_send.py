#!/usr/bin/env python3
"""Send a file to U-Boot's "loady" over the serial console (YMODEM batch).

U-Boot on this board offers loadb/loadx/loady, so a file can be delivered over
the UART that is already attached. That needs no SD card, no TFTP server and no
root, which is what makes it the usable path when the card cannot be reached.

YMODEM here is the batch variant U-Boot implements:
  - wait for 'C', send SOH 00 FF 00*128 crc16
  - send the header packet: "name\\0size mtime mode\\0"
  - send data in 1024-byte STX packets, numbered from 1
  - finish data with EOT, then an empty header packet (name = "")
  - YMODEM expects the receiver's ACK after each block.

Usage:
  loady_send.py --dev /dev/cu.usbserial-01025FC4 --baud 921600 \\
      --file output/ramboot-unit3/meson-g12a-x96-max-p-usb.dtb --address 0x08000000

The caller must already have the box sitting in a U-Boot prompt; this script
types the "loady <address> <baud>" command itself, waits for the receiver, then
streams the file and finally restores the port to the console baud rate.
"""
import argparse
import os
import sys
import termios
import time
import tty

SOH, STX, EOT, ACK, NAK, CAN, CRC_CHAR = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, ord("C")


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def make_packet(seq, payload):
    body = bytes([seq & 0xFF]) + bytes([(seq ^ 0xFF) & 0xFF]) + payload
    crc = crc16(payload)
    return bytes([STX if len(payload) > 128 else SOH]) + body + bytes([crc >> 8, crc & 0xFF])


def open_port(dev, baud):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = attrs[5] = termios_baud(baud)
    attrs[6][termios.VMIN] = 1
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def termios_baud(baud):
    """Map a rate to termios' constant.

    macOS' termios tops out at B230400, so faster rates cannot be set through
    the standard API even though the CP2104 itself supports 2 Mbps.
    """
    table = {9600: termios.B9600, 19200: termios.B19200, 38400: termios.B38400,
             57600: termios.B57600, 115200: termios.B115200, 230400: termios.B230400}
    if baud not in table:
        raise ValueError(
            "unsupported baud: %s (macOS termios supports up to 230400)" % baud)
    return table[baud]


def set_baud(fd, baud):
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = termios_baud(baud)
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIFLUSH)


def write_all(fd, data, timeout=10.0):
    """Write every byte to a non-blocking fd.

    The port is opened O_NONBLOCK so reads never stall the control loop; that
    also makes a large write fail with EAGAIN when the kernel buffer is full,
    which happened mid-transfer and aborted a run at 79%. Retry until the whole
    packet is out or the deadline passes.
    """
    view = memoryview(data)
    end = time.time() + timeout
    while view:
        try:
            written = os.write(fd, view)
        except BlockingIOError:
            written = 0
        except InterruptedError:
            continue
        if written:
            view = view[written:]
        else:
            if time.time() > end:
                raise TimeoutError("serial write timed out")
            time.sleep(0.005)


def read_byte(fd, timeout):
    import select
    end = time.time() + timeout
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if ready:
            try:
                data = os.read(fd, 4096)
            except OSError:
                continue
            if data:
                return data
    return b""


def send_block(fd, packet, seq, retries=10, accept_c=False):
    """Send one block and read the receiver's answer.

    Every byte arriving after the write is collected rather than the first
    chunk only: U-Boot's loady echoes nothing but does emit 'C' while it still
    wants data, so a lone 'C' means "not received yet" and ACK means "got it".
    A CAN or a NAK triggers a retry of the same block.
    """
    for attempt in range(retries):
        write_all(fd, packet)
        end = time.time() + 8.0
        seen = bytearray()
        while time.time() < end:
            got = read_byte(fd, 0.3)
            if not got:
                if seen and ACK in seen:
                    return True
                continue
            seen.extend(got)
            if CAN in seen:
                raise RuntimeError("receiver cancelled at block %d" % seq)
            if ACK in seen:
                return True
        if seen and accept_c and set(seen) <= {CRC_CHAR}:
            # Still waiting for this block: resend without counting a retry.
            sys.stderr.write("  block %d not taken yet (C only)\n" % seq)
        else:
            sys.stderr.write("  block %d retry %d (saw %r)\n" % (seq, attempt + 1, bytes(seen[:12])))
    raise RuntimeError("no ACK for block %d" % seq)


def wait_for_c(fd, timeout):
    end = time.time() + timeout
    seen = b""
    while time.time() < end:
        got = read_byte(fd, 0.5)
        if not got:
            continue
        seen += got
        if CRC_CHAR in got:
            return True
        if CAN in got:
            raise RuntimeError("receiver cancelled before the header")
    sys.stderr.write("no 'C' from receiver; last bytes %r\n" % seen[-80:])
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", required=True)
    ap.add_argument("--baud", type=int, default=230400,
                    help="transfer baud rate (macOS supports up to 230400)")
    ap.add_argument("--console-baud", type=int, default=115200)
    ap.add_argument("--file", required=True)
    ap.add_argument("--address", required=True)
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()

    data = open(args.file, "rb").read()
    size = len(data)
    name = os.path.basename(args.file)
    header = ("%s\x00%d 0 0\x00" % (name, size)).encode()
    header += b"\x00" * (128 - len(header)) if len(header) <= 128 else b""

    # One port for the whole transfer: closing and reopening it would drop the
    # single 'C' U-Boot sends after "Ready for binary ... download".
    fd = open_port(args.dev, args.console_baud)
    try:
        write_all(fd, ("loady %s %d\r" % (args.address, args.baud)).encode())
        banner = read_byte(fd, 3.0)
        if b"Ready for binary" not in banner:
            sys.stderr.write("no download banner: %r\n" % banner[-120:])
            return 1
        set_baud(fd, args.baud)
        print("waiting for receiver 'C' at %d baud" % args.baud, file=sys.stderr)
        if not wait_for_c(fd, args.timeout):
            return 1
        print("sending header (%s, %d bytes)" % (name, size), file=sys.stderr)
        send_block(fd, make_packet(0, header), 0)
        seq = 1
        sent = 0
        while sent < size:
            chunk = data[sent:sent + 1024]
            if len(chunk) < 1024:
                chunk += b"\x1a" * (1024 - len(chunk))
            send_block(fd, make_packet(seq, chunk), seq, accept_c=True)
            sent += 1024
            if seq % 512 == 0:
                print("  %d/%d KiB" % (min(sent, size) // 1024, (size + 1023) // 1024),
                      file=sys.stderr)
            seq = (seq + 1) & 0xFF
        # EOT, then the empty header that ends the batch.
        for _ in range(3):
            write_all(fd, bytes([EOT]))
            got = read_byte(fd, 5.0)
            if got and ACK in got:
                break
        wait_for_c(fd, 10.0)
        write_all(fd, make_packet(0, b""))
        read_byte(fd, 5.0)
        print("sent %d bytes" % size, file=sys.stderr)
        return 0
    finally:
        try:
            set_baud(fd, args.console_baud)
        except (OSError, ValueError):
            pass
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
