#!/usr/bin/env python3
"""
Behringer WING simulator for testing WING Follow without a console.

Speaks the parts of the WING OSC protocol the plugin uses:
  * UDP 2223: queries (address, no args) answered as ",sff" / ",sfi" / ",s" like the console,
    "/*S" subscription (one subscriber, last requester wins, expires after 10 s), "/?" info.
  * UDP 2222: "WING?" discovery.
Because the plugin must never change the console, any message WITH arguments is reported as a
violation and the simulator exits with status 2 (use --allow-set to only warn).

Run it, then point the plugin at 127.0.0.1 (or this machine's IP), and type commands:
  fader ch 1 -5        mute dca 2 on        name ch 1 Vocals
  patch ch 3 A 5       (channel 3 main input = source A.5)
  alt ch 3 B 9 on      (channel 3 alt input = B.9, and switch to it)
  out CRD 12 A 7       (card output 12 carries source A.7; also: out CRD 12 BUS 3 / USR 2)
  user 2 CH 9          (user signal 2 taps channel 9)
  mode USB 1 ST        (source USB.1 is a stereo pair)
  sweep ch 1           (move the fader up and down for a few seconds)
  scene                (burst: randomise every fader and mute, like a scene recall)
  steal on|off         (pretend another app, e.g. Companion, took the subscription)
  show ch 1            help            quit

Standard library only; Python 3.8+.
"""
import argparse
import random
import socket
import struct
import sys
import threading
import time

STRIPS = {"ch": 40, "aux": 8, "bus": 16, "main": 4, "mtx": 8, "dca": 16}
MUTE_GROUPS = 8
IN_GROUPS = {"LCL": 8, "AUX": 8, "A": 48, "B": 48, "C": 48, "SC": 32, "USB": 48, "CRD": 64,
             "MOD": 64, "PLAY": 4, "AES": 2, "USR": 24, "OSC": 2}
OUT_GROUPS = {"LCL": 8, "AUX": 8, "A": 48, "B": 48, "C": 48, "SC": 32, "USB": 48, "CRD": 64,
              "MOD": 64, "REC": 4, "AES": 2}


def pad(b):
    return b + b"\0" * (4 - len(b) % 4)


def osc_str(s):
    return pad(s.encode())


def osc_msg(addr, tags, *args):
    out = osc_str(addr) + osc_str("," + tags)
    for t, a in zip(tags, args):
        if t == "s":
            out += osc_str(a)
        elif t == "f":
            out += struct.pack(">f", a)
        elif t == "i":
            out += struct.pack(">i", a)
    return out


def osc_parse(data):
    def rstr(pos):
        end = data.index(b"\0", pos)
        s = data[pos:end].decode(errors="replace")
        return s, (end + 4) & ~3
    addr, pos = rstr(0)
    args = []
    if pos < len(data):
        tags, pos = rstr(pos)
        for t in tags[1:]:
            if t in "if":
                args.append(struct.unpack(">" + t, data[pos:pos + 4])[0])
                pos += 4
            elif t == "s":
                s, pos = rstr(pos)
                args.append(s)
    return addr, args


def fader_raw(db):
    # Rough position for display only; the plugin uses the dB value.
    return 0.0 if db <= -144 else max(0.0, min(1.0, (db + 60) / 70))


class Wing:
    def __init__(self, name, ip):
        self.name, self.ip = name, ip
        self.lock = threading.Lock()
        self.v = {}  # address -> ("f", dB) | ("i", int) | ("s", str)
        for t, n in STRIPS.items():
            for i in range(1, n + 1):
                self.v[f"/{t}/{i}/fdr"] = ("f", -144.0 if t != "main" else 0.0)
                self.v[f"/{t}/{i}/mute"] = ("i", 0)
                self.v[f"/{t}/{i}/name"] = ("s", "")
        for i in range(1, MUTE_GROUPS + 1):
            self.v[f"/mgrp/{i}/mute"] = ("i", 0)
            self.v[f"/mgrp/{i}/name"] = ("s", "")
        for t in ("ch", "aux"):
            for i in range(1, STRIPS[t] + 1):
                grp, num = ("A", i) if t == "ch" else ("AUX", i)
                self.v[f"/{t}/{i}/in/conn/grp"] = ("s", grp)
                self.v[f"/{t}/{i}/in/conn/in"] = ("i", num)
                self.v[f"/{t}/{i}/in/conn/altgrp"] = ("s", "OFF")
                self.v[f"/{t}/{i}/in/conn/altin"] = ("i", 1)
                self.v[f"/{t}/{i}/in/set/altsrc"] = ("i", 0)
        for g, n in IN_GROUPS.items():
            for i in range(1, n + 1):
                self.v[f"/io/in/{g}/{i}/mode"] = ("s", "M")
                self.v[f"/io/in/{g}/{i}/name"] = ("s", "")
        for g, n in OUT_GROUPS.items():
            for i in range(1, n + 1):
                routed = g == "CRD" and i <= 48
                self.v[f"/io/out/{g}/{i}/grp"] = ("s", "A" if routed else "OFF")
                self.v[f"/io/out/{g}/{i}/in"] = ("i", i if routed else 1)
        for i in range(1, 25):
            self.v[f"/io/user/{i}/grp"] = ("s", "OFF")
            self.v[f"/io/user/{i}/in"] = ("i", 1)
        self.v["/io/altsw"] = ("i", 0)
        # A little sample content.
        for i, (nm, db) in enumerate([("Kick", -5), ("Snare", -8), ("Bass", -4), ("Keys", -10),
                                      ("Gtr", -9), ("Vox 1", 0), ("Vox 2", -2), ("Pastor", 2)], 1):
            self.v[f"/ch/{i}/name"] = ("s", nm)
            self.v[f"/ch/{i}/fdr"] = ("f", float(db))
        self.v["/dca/1/name"] = ("s", "Band")
        self.v["/dca/1/fdr"] = ("f", 0.0)
        self.v["/dca/2/name"] = ("s", "Vocals")
        self.v["/dca/2/fdr"] = ("f", 0.0)

    def reply(self, addr):
        kind, val = self.v[addr]
        if kind == "f":
            s = "-oo" if val <= -144 else f"{val:.1f}"
            return osc_msg(addr, "sff", s, fader_raw(val), float(val))
        if kind == "i":
            return osc_msg(addr, "sfi", str(val), float(val), int(val))
        return osc_msg(addr, "s", val)

    def info(self):
        return f"WING,{self.ip},{self.name},ngc-full,SIM00001,3.0.6-sim"


class Server:
    def __init__(self, wing, port, allow_set, verbose):
        self.wing, self.allow_set, self.verbose = wing, allow_set, verbose
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", port))
        self.disc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.disc.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.disc.bind(("0.0.0.0", 2222))
        except OSError as e:
            print(f"(discovery on 2222 unavailable: {e})")
            self.disc = None
        self.subscriber, self.sub_until = None, 0.0
        self.steal = False
        self.stats = {"queries": 0, "subscribes": 0}

    def set(self, addr, kind, val):
        with self.wing.lock:
            if addr not in self.wing.v:
                print(f"unknown address {addr}")
                return
            self.wing.v[addr] = (kind, val)
            pkt = self.wing.reply(addr)
        if self.subscriber and time.time() < self.sub_until and not self.steal:
            self.sock.sendto(pkt, self.subscriber)

    def serve(self):
        threading.Thread(target=self._loop, args=(self.sock,), daemon=True).start()
        if self.disc:
            threading.Thread(target=self._discovery, daemon=True).start()

    def _discovery(self):
        while True:
            data, src = self.disc.recvfrom(2048)
            if data.startswith(b"WING?"):
                self.disc.sendto(self.wing.info().encode(), src)

    def _loop(self, sock):
        while True:
            data, src = sock.recvfrom(65536)
            try:
                addr, args = osc_parse(data)
            except Exception:
                continue
            if args:
                print(f"\n!!! VIOLATION: plugin tried to SET {addr} {args} (from {src})")
                if not self.allow_set:
                    sys.stdout.flush()
                    import os
                    os._exit(2)
                continue
            if self.verbose:
                print(f"<- {addr} from {src}")
            if addr in ("/*S", "/*s"):
                self.stats["subscribes"] += 1
                if self.subscriber != src:
                    print(f"\n[subscription now held by {src}]")
                self.subscriber, self.sub_until = src, time.time() + 10
                continue
            if addr == "/?":
                sock.sendto(osc_msg("/?", "s", self.wing.info()), src)
                continue
            self.stats["queries"] += 1
            with self.wing.lock:
                pkt = self.wing.reply(addr) if addr in self.wing.v else None
            if pkt:
                sock.sendto(pkt, src)


def on_off(s):
    return 1 if s.lower() in ("1", "on", "yes", "true", "mute", "muted") else 0


def repl(srv):
    w = srv.wing
    print("WING simulator ready. Type 'help' for commands.")
    while True:
        try:
            line = input("wing> ").strip()
        except (EOFError, KeyboardInterrupt):
            return
        if not line:
            continue
        p = line.split()
        c = p[0].lower()
        try:
            if c in ("quit", "exit"):
                return
            elif c == "help":
                print(__doc__)
            elif c == "fader":
                db = -144.0 if p[3] in ("-oo", "-inf") else float(p[3])
                srv.set(f"/{p[1]}/{p[2]}/fdr", "f", db)
            elif c == "mute":
                srv.set(f"/{p[1]}/{p[2]}/mute", "i", on_off(p[3]))
            elif c == "name":
                srv.set(f"/{p[1]}/{p[2]}/name", "s", " ".join(p[3:]))
            elif c == "patch":
                srv.set(f"/{p[1]}/{p[2]}/in/conn/grp", "s", p[3])
                srv.set(f"/{p[1]}/{p[2]}/in/conn/in", "i", int(p[4]))
            elif c == "alt":
                srv.set(f"/{p[1]}/{p[2]}/in/conn/altgrp", "s", p[3])
                srv.set(f"/{p[1]}/{p[2]}/in/conn/altin", "i", int(p[4]))
                srv.set(f"/{p[1]}/{p[2]}/in/set/altsrc", "i", on_off(p[5]) if len(p) > 5 else 1)
            elif c == "out":
                srv.set(f"/io/out/{p[1]}/{p[2]}/grp", "s", p[3])
                srv.set(f"/io/out/{p[1]}/{p[2]}/in", "i", int(p[4]))
            elif c == "user":
                srv.set(f"/io/user/{p[1]}/grp", "s", p[2])
                srv.set(f"/io/user/{p[1]}/in", "i", int(p[3]))
            elif c == "mode":
                srv.set(f"/io/in/{p[1]}/{p[2]}/mode", "s", p[3])
            elif c == "sweep":
                addr = f"/{p[1]}/{p[2]}/fdr"

                def run():
                    for k in range(60):
                        srv.set(addr, "f", round(-40 + 45 * abs(((k / 30.0) % 2) - 1), 1))
                        time.sleep(0.05)
                threading.Thread(target=run, daemon=True).start()
            elif c == "scene":
                for t, n in STRIPS.items():
                    for i in range(1, n + 1):
                        srv.set(f"/{t}/{i}/fdr", "f", round(random.uniform(-30, 5), 1))
                        srv.set(f"/{t}/{i}/mute", "i", int(random.random() < 0.2))
                print("scene recalled")
            elif c == "steal":
                srv.steal = on_off(p[1]) if len(p) > 1 else not srv.steal
                print("subscription pushes", "BLOCKED (polling only)" if srv.steal else "active")
            elif c == "show":
                base = f"/{p[1]}/{p[2]}"
                with w.lock:
                    for k, v in w.v.items():
                        if k.startswith(base + "/"):
                            print(f"  {k} = {v[1]}")
            elif c == "stats":
                print(srv.stats, "subscriber:", srv.subscriber)
            else:
                print("unknown command; try 'help'")
        except (IndexError, ValueError) as e:
            print(f"bad arguments ({e}); try 'help'")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=2223)
    ap.add_argument("--name", default="WING-SIM")
    ap.add_argument("--ip", default="127.0.0.1", help="IP reported in discovery replies")
    ap.add_argument("--allow-set", action="store_true", help="warn instead of exiting on SET messages")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every received message")
    a = ap.parse_args()
    srv = Server(Wing(a.name, a.ip), a.port, a.allow_set, a.verbose)
    srv.serve()
    print(f"WING simulator '{a.name}' listening on UDP {a.port} (OSC) and 2222 (discovery)")
    repl(srv)


if __name__ == "__main__":
    main()
