#!/usr/bin/env python3
"""DAP smoke test for Calc-U-1600's debug server.

Connects to a running app (started with `--dap <port>`, or with the debug
server enabled in Settings), attaches, and drives the requests VS Code
uses: threads, pause, stackTrace/scopes/variables, disassemble, readMemory,
evaluate, instruction and data breakpoints, stepping, continue, disconnect.

Stdlib only:  uv run tools/dap_smoke.py [--port 4711] [--app PATH]
With --app the script starts the app itself (with --dap) and quits it
through the calcu1600/quit request.
"""

import argparse
import base64
import json
import os
import socket
import subprocess
import sys
import time


class Dap:
    def __init__(self, port, timeout=10.0):
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=2.0)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.25)
        self.sock.settimeout(timeout)
        self.buf = b""
        self.seq = 1
        self.events = []

    def _read_message(self):
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buf += chunk
        header, _, rest = self.buf.partition(b"\r\n\r\n")
        length = int([l for l in header.split(b"\r\n") if l.lower().startswith(b"content-length:")][0].split(b":")[1])
        while len(rest) < length:
            rest += self.sock.recv(65536)
        self.buf = rest[length:]
        return json.loads(rest[:length])

    def request(self, command, **arguments):
        seq = self.seq
        self.seq += 1
        body = json.dumps({"seq": seq, "type": "request", "command": command, "arguments": arguments}).encode()
        self.sock.sendall(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        while True:
            msg = self._read_message()
            if msg["type"] == "event":
                self.events.append(msg)
            elif msg["type"] == "response" and msg["request_seq"] == seq:
                if not msg["success"]:
                    raise RuntimeError(f"{command} failed: {msg.get('message')}")
                return msg.get("body", {})

    def wait_event(self, name, timeout=10.0, match=None):
        deadline = time.time() + timeout
        while True:
            for i, e in enumerate(self.events):
                if e["event"] == "output":
                    print("       |", e["body"]["output"].rstrip())
                    self.events.pop(i)
                    break
            for i, e in enumerate(self.events):
                if e["event"] == name and (match is None or match(e.get("body", {}))):
                    return self.events.pop(i).get("body", {})
            if time.time() > deadline:
                raise TimeoutError(f"no '{name}' event within {timeout}s (have: {[e['event'] for e in self.events]})")
            self.sock.settimeout(max(0.1, deadline - time.time()))
            try:
                self.events.append(self._read_message())
            except socket.timeout:
                pass


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        check.failures += 1


check.failures = 0


REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MEMTEST_ASM = os.path.join(REPO, "Core/tests/fixtures/listings/sdas-lh5801/memtest.asm")


def top_frame(dap, tid):
    return dap.request("stackTrace", threadId=tid, startFrame=0, levels=1)["stackFrames"][0]


def program_run(port):
    """Build & Load: a plain PC-1500 (preset), memtest with its listing, stop at its entry."""
    print("program run:")
    dap = Dap(port)
    dap.request("initialize", adapterID="calcu1600")
    dap.wait_event("initialized")
    dap.request("attach", preset=os.path.join(REPO, "examples/startup/default-pc1500.pc1500"),
                program={"bin": os.path.join(REPO, "examples/memtest_stock.bin"),
                         "listing": os.path.join(REPO, "Core/tests/fixtures/listings/sdas-lh5801/memtest.rst"),
                         "address": "0x40C5", "after": "stopOnEntry"})
    bps = dap.request("setBreakpoints", source={"path": MEMTEST_ASM}, breakpoints=[{"line": 88}])["breakpoints"]
    check(bps and bps[0]["verified"] and bps[0]["line"] == 89, f"source breakpoint 88 -> {bps and bps[0].get('line')}")
    dap.request("configurationDone")
    stop = dap.wait_event("stopped", timeout=30)
    top = top_frame(dap, stop["threadId"])
    check(stop.get("reason") == "entry" and top.get("line") == 74 and top.get("source", {}).get("path") == MEMTEST_ASM,
          f"stopped at the entry: {stop.get('reason')} {top.get('source', {}).get('name')}:{top.get('line')}")
    dap.request("continue", threadId=1)
    stop = dap.wait_event("stopped", timeout=10)
    check(stop.get("reason") == "breakpoint" and top_frame(dap, 1).get("line") == 89, "source breakpoint hit at line 89")
    dap.request("next", threadId=1)
    dap.wait_event("stopped")
    check(top_frame(dap, 1).get("line") == 90, "next: line 90")
    dap.request("stepIn", threadId=1)
    dap.wait_event("stopped")
    check(top_frame(dap, 1).get("line") == 94, "stepIn: line 94")
    ev = dap.request("evaluate", expression="[ERR_FLAG]", frameId=1000, context="watch")
    check(ev["result"].startswith("0x00"), f"evaluate [ERR_FLAG] = {ev['result']}")
    dap.request("setBreakpoints", source={"path": MEMTEST_ASM}, breakpoints=[])
    # Build & Load again (what the extension sends): clean start, load,
    # auto-start, stop at the entry.
    body = dap.request("calcu1600/load", bin=os.path.join(REPO, "examples/memtest_stock.bin"),
                       listing=os.path.join(REPO, "Core/tests/fixtures/listings/sdas-lh5801/memtest.rst"),
                       address="0x40C5", after="stopOnEntry")
    check(body.get("start") == "1:40C5", f"calcu1600/load -> {body}")
    stop = dap.wait_event("stopped", timeout=30)
    check(stop.get("reason") == "entry" and top_frame(dap, 1).get("line") == 74, "reloaded and stopped at the entry again")
    dap.request("stepOut", threadId=1)
    stop = dap.wait_event("stopped", timeout=10)
    top = top_frame(dap, 1)
    check(stop.get("reason") == "step" and "source" not in top, f"stepOut back into the ROM: {top['name']}")
    dap.request("disconnect")
    dap.sock.close()


def pc1600_run(port):
    """PC-1600: the zasm ROM dumper with its listing; two CPU threads."""
    print("PC-1600 run:")
    dumper = os.path.join(REPO, "Core/tests/fixtures/listings/zasm/pc1600-rom-dumper")
    dap = Dap(port)
    dap.request("initialize", adapterID="calcu1600")
    dap.wait_event("initialized")
    dap.request("attach", preset=os.path.join(REPO, "examples/startup/default-pc1600.pc1600"),
                program={"bin": dumper + ".bin", "listing": dumper + ".lst", "address": "0xC0C5",
                         "after": "stopOnEntry"})
    dap.request("configurationDone")
    stop = dap.wait_event("stopped", timeout=60)
    top = top_frame(dap, stop["threadId"])
    check(stop.get("reason") == "entry" and top.get("source", {}).get("name") == "pc1600-rom-dumper.asm",
          f"stopped at the entry: {top.get('source', {}).get('name')}:{top.get('line')} ({top['name']})")
    names = [t["name"] for t in dap.request("threads")["threads"]]
    check(len(names) == 2 and any("LH5803" in n for n in names), f"threads: {names}")
    line = top.get("line")
    dap.request("next", threadId=1)
    dap.wait_event("stopped")
    check(top_frame(dap, 1).get("line", 0) > (line or 0), f"next: line {line} -> {top_frame(dap, 1).get('line')}")
    scopes = dap.request("scopes", frameId=1000)["scopes"]
    banks = dap.request("variables", variablesReference=scopes[1]["variablesReference"])["variables"]
    check(len(banks) == 4, "banks: " + ", ".join(f"{b['name'][:6]} {b['value']}" for b in banks))
    dap.request("continue", threadId=1)
    dap.request("disconnect")
    dap.sock.close()


def rom_run(port):
    """ROM research: reset and stop before the first instruction, then single-step."""
    print("ROM run:")
    dap = Dap(port)
    dap.request("initialize", adapterID="calcu1600")
    dap.wait_event("initialized")
    dap.request("attach")
    dap.request("configurationDone")
    dap.request("calcu1600/reset", kind="reset", stop=True)
    stop = dap.wait_event("stopped")
    tid = stop["threadId"]
    top = top_frame(dap, tid)
    check(stop.get("reason") == "entry" and top["instructionPointerReference"] in ("1:E000", "1:0000"),
          f"reset stops at the vector: {top['name']}")
    for _ in range(5):
        dap.request("stepIn", threadId=tid, granularity="instruction")
        dap.wait_event("stopped")
    frames = dap.request("stackTrace", threadId=tid, startFrame=0, levels=10)["stackFrames"]
    check(len(frames) == 6, f"history after 5 steps: {len(frames) - 1} ({frames[-1]['name']} .. {frames[1]['name']})")
    dap.request("continue", threadId=tid)
    dap.request("disconnect")
    dap.sock.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=4711)
    ap.add_argument("--app", help="start this app binary with --dap <port> and quit it afterwards")
    args = ap.parse_args()

    proc = None
    if args.app:
        # -ApplePersistenceIgnoreState: no macOS window restoration for this run.
        proc = subprocess.Popen([args.app, "--dap", str(args.port), "-ApplePersistenceIgnoreState", "YES"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)
    try:
        dap = Dap(args.port, timeout=20.0)
        caps = dap.request("initialize", adapterID="calcu1600", linesStartAt1=True, columnsStartAt1=True)
        check(caps.get("supportsInstructionBreakpoints"), "initialize: capabilities")
        dap.wait_event("initialized")
        dap.request("attach", stopOnEntry=True)
        dap.request("configurationDone")
        stop = dap.wait_event("stopped")
        check(stop.get("reason") == "entry", f"stopOnEntry -> stopped ({stop.get('reason')})")

        threads = dap.request("threads")["threads"]
        check(len(threads) >= 1, f"threads: {[t['name'] for t in threads]}")
        tid = stop["threadId"]

        frames = dap.request("stackTrace", threadId=tid, startFrame=0, levels=25)
        stack = frames["stackFrames"]
        check(len(stack) == 21, f"stackTrace: {len(stack)} frames; top '{stack[0]['name']}'")
        pc_ref = stack[0]["instructionPointerReference"]
        scopes = dap.request("scopes", frameId=stack[0]["id"])["scopes"]
        regs = dap.request("variables", variablesReference=scopes[0]["variablesReference"])["variables"]
        check(len(regs) >= 8, "registers: " + ", ".join(f"{v['name']}={v['value']}" for v in regs[:6]))
        flags = [v for v in regs if v["name"] == "Flags"]
        if flags:
            bits = dap.request("variables", variablesReference=flags[0]["variablesReference"])["variables"]
            check(len(bits) >= 4, "flags: " + " ".join(f"{b['name']}{b['value']}" for b in bits))

        dis = dap.request("disassemble", memoryReference=pc_ref, instructionOffset=-4, instructionCount=10)["instructions"]
        check(len(dis) == 10 and any(i["address"] == pc_ref for i in dis),
              f"disassemble around {pc_ref}: " + "; ".join(i["instruction"] for i in dis[3:6]))

        mem = dap.request("readMemory", memoryReference=pc_ref, count=16)
        check(len(base64.b64decode(mem["data"])) == 16, f"readMemory 16 bytes at {pc_ref}")

        ev = dap.request("evaluate", expression="1 + 2 * 3", frameId=stack[0]["id"], context="repl")
        check(ev["result"].startswith("0x07"), f"evaluate: {ev['result']}")

        # Step one instruction.
        dap.request("stepIn", threadId=tid, granularity="instruction")
        stop = dap.wait_event("stopped")
        check(stop.get("reason") == "step", "stepIn (instruction)")

        # An instruction breakpoint on something just executed comes back.
        after = dap.request("stackTrace", threadId=tid, startFrame=0, levels=3)["stackFrames"]
        target = after[1]["instructionPointerReference"]
        bps = dap.request("setInstructionBreakpoints", breakpoints=[{"instructionReference": target}])["breakpoints"]
        check(bps and bps[0]["verified"], f"instruction breakpoint at {target}")
        dap.request("continue", threadId=tid)
        try:
            stop = dap.wait_event("stopped", timeout=10)
            check(stop.get("reason") == "breakpoint", f"breakpoint hit ({stop.get('hitBreakpointIds')})")
        except TimeoutError:
            check(False, "breakpoint hit within 10 s")
            dap.request("pause", threadId=tid)
            dap.wait_event("stopped")
        dap.request("setInstructionBreakpoints", breakpoints=[])

        dap.request("continue", threadId=tid)
        time.sleep(0.3)
        dap.request("pause", threadId=tid)
        stop = dap.wait_event("stopped")
        check(stop.get("reason") == "pause", "continue + pause")

        dap.request("disconnect")
        dap.sock.close()

        program_run(args.port)
        pc1600_run(args.port)
        rom_run(args.port)
        print("done:", "all passed" if check.failures == 0 else f"{check.failures} failed")
    finally:
        if proc:
            # Quit it the normal way (a kill makes macOS offer to restore
            # its windows on the next launch); kill only as a last resort.
            try:
                Dap(args.port, timeout=5).request("calcu1600/quit")
                proc.wait(timeout=10)
            except Exception:
                proc.kill()
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
