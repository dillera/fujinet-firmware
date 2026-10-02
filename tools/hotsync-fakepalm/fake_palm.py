#!/usr/bin/env python3
"""A stand-in Palm that HotSyncs with FujiNet over network HotSync (NetSync).

It answers the DLP requests a FujiNet sync makes, keeps its Date Book in a
JSON file between runs, and prints what changed. Use it to exercise the
HotSync service, on a FujiNet or FujiNet-PC, without a device in a cradle.

    fake_palm.py 192.168.1.252             # sync once
    fake_palm.py localhost --port 14238 --state palm.json --show
    fake_palm.py 192.168.1.252 --user "Test Palm" --state test.json

Only what FujiNet's sync uses is implemented: user and system info, opening
DatebookDB, reading, writing and deleting its records, the clock, the sync
log and EndOfSync. Other databases do not exist, so nothing is backed up.
"""

import argparse
import datetime
import json
import os
import socket
import struct
import sys

DATEBOOK = "DatebookDB"

# DLP function codes (Palm OS SDK DLCommon.h).
F = {
    "ReadUserInfo": 0x10, "WriteUserInfo": 0x11, "ReadSysInfo": 0x12,
    "GetSysDateTime": 0x13, "SetSysDateTime": 0x14, "ReadDBList": 0x16,
    "OpenDB": 0x17, "CreateDB": 0x18, "CloseDB": 0x19, "DeleteDB": 0x1A,
    "ReadRecord": 0x20, "WriteRecord": 0x21, "DeleteRecord": 0x22,
    "AddSyncLogEntry": 0x2A, "OpenConduit": 0x2E, "EndOfSync": 0x2F,
}
NAMES = {v: k for k, v in F.items()}
ERR_NOT_FOUND = 5


class Link:
    """NetSync framing: type 1, xid, u32 length, payload."""

    def __init__(self, sock):
        self.sock = sock
        self.xid = 0

    def recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("FujiNet closed the connection")
            buf += chunk
        return buf

    def recv(self):
        _type, xid, length = struct.unpack(">BBI", self.recv_exact(6))
        self.xid = xid
        return self.recv_exact(length)

    def send(self, payload):
        self.sock.sendall(struct.pack(">BBI", 1, self.xid, len(payload)) + payload)


def parse_args(body):
    argc = body[1]
    pos, args = 2, {}
    for _ in range(argc):
        aid = body[pos]
        if aid & 0x80:      # short form
            length = struct.unpack(">H", body[pos + 2:pos + 4])[0]
            pos += 4
        elif aid & 0x40:    # long form
            length = struct.unpack(">I", body[pos + 2:pos + 6])[0]
            pos += 6
        else:
            length = body[pos + 1]
            pos += 2
        args[(aid & 0x3F) - 0x20] = body[pos:pos + length]
        pos += length
    return args


def reply(func, error=0, arg=None):
    out = bytes([func | 0x80, 1 if arg is not None else 0]) + struct.pack(">H", error)
    if arg is not None:
        if len(arg) <= 0xFF:
            out += bytes([0x20, len(arg)])
        else:
            out += bytes([0xA0, 0]) + struct.pack(">H", len(arg))
        out += arg
    return out


def dlp_date(t):
    return struct.pack(">HBBBBBB", t.year, t.month, t.day, t.hour, t.minute, t.second, 0)


# ---- Date Book records (see lib/hotsync/Datebook.cpp) ----

def unpack_date(v):
    return "%04d-%02d-%02d" % ((v >> 9) + 1904, (v >> 5) & 0x0F, v & 0x1F)


def describe(record):
    sh, sm, eh, em, date, flags = struct.unpack(">BBBBHB", record[:7])
    pos = 8
    when = "all day" if sh == 0xFF else "%02d:%02d-%02d:%02d" % (sh, sm, eh, em)
    until = ""
    if flags & 0x40:
        pos += 2
    if flags & 0x20:
        rtype, _, end = struct.unpack(">BBH", record[pos:pos + 4])
        if rtype == 1 and end != 0xFFFF:
            until = " through " + unpack_date(end)
        pos += 8
    if flags & 0x08:
        pos += 2 + 2 * struct.unpack(">H", record[pos:pos + 2])[0]
    desc = note = b""
    if flags & 0x04:
        end = record.index(b"\0", pos)
        desc, pos = record[pos:end], end + 1
    if flags & 0x10:
        end = record.index(b"\0", pos)
        note = record[pos:end]
    text = "%s %-11s %s%s" % (unpack_date(date), when, desc.decode("cp1252", "replace"), until)
    if note:
        text += "  [" + note.decode("cp1252", "replace").replace("\n", " ") + "]"
    return text


class Palm:
    def __init__(self, state):
        self.state = state
        self.user = state.setdefault("user", {"name": "", "id": 0})
        self.records = {int(k): v for k, v in state.setdefault("datebook", {}).items()}
        self.next_id = max([0x600000] + [k + 1 for k in self.records])
        self.log = []
        self.changes = []

    def save(self):
        self.state["datebook"] = {str(k): v for k, v in self.records.items()}

    def handle(self, body):
        func = body[0]
        args = parse_args(body)
        a = args.get(0, b"")
        name = NAMES.get(func, hex(func))

        if func == F["ReadUserInfo"]:
            u = self.user["name"].encode("latin-1")
            zero = b"\0" * 8
            return reply(func, 0, struct.pack(">III", self.user["id"], 0, 0) + zero + zero +
                         bytes([len(u) + 1 if u else 0, 0]) + (u + b"\0" if u else b""))
        if func == F["WriteUserInfo"]:
            user_id = struct.unpack(">I", a[0:4])[0]
            flags = a[20]
            name_len = a[21]
            if flags & 0x80:
                self.user["id"] = user_id
            if flags & 0x10:
                self.user["name"] = a[22:22 + name_len].split(b"\0")[0].decode("latin-1")
            return reply(func)
        if func == F["ReadSysInfo"]:
            return reply(func, 0, struct.pack(">IIBB", 0x03303000, 0, 0, 0))
        if func == F["GetSysDateTime"]:
            return reply(func, 0, dlp_date(datetime.datetime.now()))
        if func == F["ReadDBList"]:
            return reply(func, ERR_NOT_FOUND)
        if func == F["OpenDB"]:
            db = a[2:].split(b"\0")[0].decode("latin-1")
            if db != DATEBOOK:
                return reply(func, ERR_NOT_FOUND)
            return reply(func, 0, bytes([1]))
        if func == F["ReadRecord"]:
            if 0 not in args:
                return reply(func, ERR_NOT_FOUND)
            rid = struct.unpack(">I", a[2:6])[0]
            rec = self.records.get(rid)
            if rec is None:
                return reply(func, ERR_NOT_FOUND)
            data = bytes.fromhex(rec["data"])
            return reply(func, 0, struct.pack(">IHHBB", rid, 0, len(data), rec["attr"], 0) + data)
        if func == F["WriteRecord"]:
            rid = struct.unpack(">I", a[2:6])[0]
            data = a[8:]
            if rid == 0:
                rid = self.next_id
                self.next_id += 1
                self.changes.append("+ " + describe(data))
            elif rid in self.records:
                self.changes.append("~ " + describe(data))
            else:
                return reply(func, ERR_NOT_FOUND)
            self.records[rid] = {"data": data.hex(), "attr": 0}
            return reply(func, 0, struct.pack(">I", rid))
        if func == F["DeleteRecord"]:
            rid = struct.unpack(">I", a[2:6])[0]
            rec = self.records.pop(rid, None)
            if rec is None:
                return reply(func, ERR_NOT_FOUND)
            self.changes.append("- " + describe(bytes.fromhex(rec["data"])))
            return reply(func)
        if func == F["AddSyncLogEntry"]:
            self.log.append(a.split(b"\0")[0].decode("cp1252", "replace").strip())
            return reply(func)
        if func == F["EndOfSync"]:
            return reply(func)
        # OpenConduit, CloseDB and anything else succeed with no data.
        return reply(func)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("host")
    p.add_argument("--port", type=int, default=14238)
    p.add_argument("--state", default=os.path.join(os.path.dirname(__file__), "palm.json"),
                   help="JSON file holding this Palm's user and Date Book")
    p.add_argument("--show", action="store_true", help="print the whole Date Book afterwards")
    p.add_argument("--verbose", action="store_true", help="print each DLP request")
    p.add_argument("--user", default="Fake Palm",
                   help="user name of a new Palm; keep it apart from real devices, since "
                        "FujiNet keeps each user's records under its name")
    opts = p.parse_args()

    state = {}
    if os.path.exists(opts.state):
        with open(opts.state) as f:
            state = json.load(f)
    palm = Palm(state)
    if not palm.user["name"]:
        palm.user["name"] = opts.user
        palm.user["id"] = 0x46414B45

    sock = socket.create_connection((opts.host, opts.port), timeout=60)
    link = Link(sock)
    # The handshake payloads are opaque; FujiNet checks only their lengths.
    for size in (22, 50):
        link.send(bytes(size))
        link.recv()
    link.send(bytes(8))

    while True:
        body = link.recv()
        if opts.verbose:
            print("  <", NAMES.get(body[0], hex(body[0])), file=sys.stderr)
        link.send(palm.handle(body))
        if body[0] == F["EndOfSync"]:
            break
    sock.close()

    palm.save()
    with open(opts.state, "w") as f:
        json.dump(state, f, indent=1)

    print("User: %s" % palm.user["name"])
    for line in palm.log:
        print("Log:  " + line)
    for line in palm.changes:
        print(line)
    if opts.show:
        print("Date Book (%d):" % len(palm.records))
        for line in sorted(describe(bytes.fromhex(r["data"])) for r in palm.records.values()):
            print("  " + line)


if __name__ == "__main__":
    main()
