#!/usr/bin/env python3
"""push2onu.py — push a local file into the ZTE ONU (192.168.1.1) telnet shell.

Channel analysis (docs/migration.md): the ONU has no sshd/nc/base64/stty,
its FTP is read-only, and modem->router TCP is filtered. The only
bidirectional pipe is the vendor telnet shell, so we transfer as follows:

  1. login via ztel.py cred dance (same as zte_cmd.py)
  2. queue one decoder line on the shell (parsed as a whole line BEFORE any
     payload arrives):
       mkdir -p DIR; { while read -r l; do printf '%b' "$l"; done; } > PATH;
       echo SIZE=$(wc -c < PATH); echo @@PUSHDONE@@
  3. blast the payload as hex-escape lines: 900 bytes -> "\\xNN"*900 = 3600
     chars, under the 4095 MAX_CANON tty line limit. echo is left on (no
     stty applet exists), a reader thread drains the echo and watches for
     the marker so the socket never deadlocks.
  4. send Ctrl-D (VEOF) on an empty line -> `read` returns EOF -> the tail
     commands run and SIZE is printed.

verified on-device: busybox printf '%b' handles \\x00 (wc -c == real size).

usage: push2onu.py LOCAL REMOTE [--chmod 755]
"""
import re
import socket
import subprocess
import sys
import threading
import time
import os

ZTEL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ztel.py")
TARGET = "192.168.1.1"
CHUNK = 249  # bytes -> 996 hex chars + LF per line; the ONU telnetd/ash
             # KILLS the session on lines >1102 chars (probed 1102 ok /
             # 1103 dead), so stay well under: 996+1 = 997
MARKER = "@@PUSHDONE@@"

IAC, WILL, WONT, DO, DONT, SB, SE = 255, 251, 252, 253, 254, 250, 240


class TN:
    """telnet IAC stripping + marker watch (drains everything else)."""

    def __init__(self, sock):
        self.s = sock
        self.st = 0
        self.verb = 0
        self.tail = bytearray()
        self.done = threading.Event()
        self.sizeline = ""
        self.lock = threading.Lock()

    def feed(self, data):
        buf = bytearray()
        for b in data:
            if self.st == 0:
                if b == IAC:
                    self.st = 1
                else:
                    buf.append(b)
            elif self.st == 1:
                if b == IAC:
                    buf.append(255)
                    self.st = 0
                elif b in (WILL, WONT, DO, DONT):
                    self.verb = b
                    self.st = 2
                elif b == SB:
                    self.st = 3
                else:
                    self.st = 0
            elif self.st == 2:
                try:
                    if self.verb == WILL:
                        self.s.sendall(bytes([IAC, DONT, b]))
                    elif self.verb == DO:
                        self.s.sendall(bytes([IAC, WONT, b]))
                except Exception:
                    pass
                self.st = 0
            elif self.st == 3:
                if b == IAC:
                    self.st = 4
            elif self.st == 4:
                self.st = 0 if b == SE else 3
        if buf:
            with self.lock:
                self.tail += buf
                if len(self.tail) > 65536:
                    del self.tail[:-8192]
                if not self.done.is_set():
                    # must not trip on the echoed *command* (it also contains
                    # the marker): require SIZE=<digits> on its own output line
                    m = re.search(rb"SIZE=(\d+)[^\r\n]*[\r\n]+[^\r\n]*"
                                  + re.escape(MARKER.encode()), bytes(self.tail))
                    if m:
                        self.sizeline = m.group(1).decode()
                        self.done.set()

    def reader(self):
        while not self.done.is_set():
            try:
                d = self.s.recv(4096)
            except socket.timeout:
                continue
            if not d:
                break
            self.feed(d)

    def wait_marker(self, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.done.is_set():
                return True
            time.sleep(0.1)
        return self.done.is_set()


def connect(total=60):
    t0 = time.time()
    last = None
    while time.time() - t0 < total:
        try:
            return socket.create_connection((TARGET, 23), 5)
        except Exception as e:
            last = e
            time.sleep(2)
    raise last


def get_tokens():
    p = subprocess.run([sys.executable, ZTEL, "-u", "CUAdmin", "-p", "123456", TARGET],
                       capture_output=True, timeout=90, encoding="utf-8",
                       errors="replace")
    mu = re.search(r"Username:\s*(\S+)", p.stdout)
    mp = re.search(r"Password:\s*(\S+)", p.stdout)
    if not mu or not mp:
        raise RuntimeError("ztel creds parse failed: %r" % p.stdout[-300:])
    return mu.group(1), mp.group(1)


def login(tn, user, password, timeout=30):
    def expect(pat, to):
        rx = re.compile(pat, re.I | re.S)
        t0 = time.time()
        buf = bytearray()
        while time.time() - t0 < to:
            tn.s.settimeout(1.0)
            try:
                d = tn.s.recv(4096)
            except socket.timeout:
                d = b""
            if d:
                tn.feed(d)
                with tn.lock:
                    buf += tn.tail
                    tn.tail.clear()
            if rx.search(buf.decode("latin1", "replace")):
                return buf.decode("latin1", "replace")
        out = buf.decode("latin1", "replace")
        raise RuntimeError("expect timeout: %s (got %r)" % (pat, out[-200:]))

    expect(r"(login|username)\s*:\s*$", timeout)
    tn.s.sendall(user.encode() + b"\r\n")
    expect(r"password\s*:\s*$", 15)
    tn.s.sendall(password.encode() + b"\r\n")
    r = expect(r"[#$]\s*$", 25)
    if "incorrect" in r.lower():
        raise RuntimeError("login failed")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    local, remote = sys.argv[1], sys.argv[2]
    chmod = None
    if "--chmod" in sys.argv:
        chmod = sys.argv[sys.argv.index("--chmod") + 1]

    data = open(local, "rb").read()
    remote_dir = os.path.dirname(remote) or "."
    print("push %s (%d bytes) -> %s:%s" % (local, len(data), TARGET, remote))

    user, password = get_tokens()  # ztel first, THEN connect (like zte_cmd.py)
    sock = connect()
    sock.settimeout(1.0)
    tn = TN(sock)
    login(tn, user, password)

    cmd = ("mkdir -p %s; { while read -r l; do printf '%%b' \"$l\"; done; } > %s; "
           "echo SIZE=$(wc -c < %s); echo %s"
           % (remote_dir, remote, remote, MARKER))
    if chmod:
        cmd = cmd.replace("echo SIZE=", "chmod %s %s; echo SIZE=" % (chmod, remote))
    sock.sendall(cmd.encode() + b"\r\n")
    time.sleep(0.4)

    th = threading.Thread(target=tn.reader, daemon=True)
    th.start()

    total = len(data)
    sent = 0
    t0 = time.time()
    for i in range(0, total, CHUNK):
        chunk = data[i:i + CHUNK]
        line = "".join("\\x%02x" % b for b in chunk) + "\n"
        sock.sendall(line.encode("ascii"))
        sent += len(chunk)
        if i % (CHUNK * 200) == 0:
            print("  %d/%d" % (sent, total))
    sock.sendall(b"\x04")  # VEOF -> read loop EOF

    if not tn.wait_marker(90):
        print("FAIL: marker not seen (last tail=%r)" % bytes(tn.tail[-400:]))
        return 1
    got = int(tn.sizeline) if tn.sizeline else -1
    took = time.time() - t0
    print("remote SIZE=%d local=%d in %.1fs" % (got, total, took))
    if got != total:
        print("FAIL: size mismatch")
        return 1
    print("OK")
    try:
        sock.sendall(b"exit\r\n")
        sock.close()
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
