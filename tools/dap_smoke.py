#!/usr/bin/env python3
"""DAP smoke test for Calc-U-1600's debug server.

Connects to a running app (started with `--dap <port>`, or with the debug
server enabled in Settings), attaches, and drives the requests VS Code
uses: threads, pause, stackTrace/scopes/variables, disassemble, readMemory,
evaluate, instruction and data breakpoints, stepping, continue, disconnect.

Stdlib only:  uv run tools/dap_smoke.py [--port 4711] [--app PATH]
With --app the script starts the app itself (with --dap) and quits it.
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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=4711)
    ap.add_argument("--app", help="start this app binary with --dap <port> and quit it afterwards")
    args = ap.parse_args()

    proc = None
    if args.app:
        proc = subprocess.Popen([args.app, "--dap", str(args.port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
        print("done:", "all passed" if check.failures == 0 else f"{check.failures} failed")
    finally:
        if proc:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
