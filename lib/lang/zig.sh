#!/bin/sh
# lib/lang/zig.sh — Glyph language adapter for Zig.
#
# Zig is compiled. We compile each eval request as a one-shot program.
# Slow but correct.
#
# Protocol: see lib/lang/python.sh

exec python3 -c '
import sys, json, os, subprocess, tempfile, traceback

def zig_eval(code):
    # The user code becomes the body of main(). We provide `print` as a
    # std.debug.print wrapper. Zig 0.16 reorganised std.io, so we use the
    # debug print which is stable.
    full = """
const std = @import(\"std\");

pub fn print(comptime fmt: []const u8, args: anytype) void {
    std.debug.print(fmt, args);
}

pub fn main() void {
    """ + code + """
}
"""
    with tempfile.NamedTemporaryFile(suffix=".zig", mode="w", delete=False) as f:
        f.write(full)
        path = f.name
    try:
        out = path + ".out"
        r = subprocess.run(["zig", "build-exe", "-O", "ReleaseFast",
                            "-femit-bin=" + out, path],
                           capture_output=True, text=True, timeout=30)
        if r.returncode != 0:
            err_lines = [l for l in r.stderr.strip().split("\\n") if "error:" in l]
            return None, err_lines[-1].strip() if err_lines else "compile error"
        r = subprocess.run([out], capture_output=True, text=True, timeout=10)
        # std.debug.print goes to stderr, not stdout
        if r.returncode != 0:
            return None, "runtime error: " + r.stderr.strip().split("\\n")[-1]
        return (r.stdout + r.stderr).strip(), None
    finally:
        for p in [path, out, out + ".o"]:
            try: os.unlink(p)
            except: pass

for line in sys.stdin:
    line = line.strip()
    if not line: continue
    try:
        req = json.loads(line)
    except Exception as e:
        print(json.dumps({"error": "bad JSON: " + str(e)}), flush=True)
        continue
    try:
        if req.get("op") == "eval":
            val, err = zig_eval(req.get("code", ""))
            if err: print(json.dumps({"error": err}), flush=True)
            else:   print(json.dumps({"value": val or ""}), flush=True)
        elif req.get("op") == "call":
            print(json.dumps({"error": "lang_call not supported for Zig (no runtime reflection)"}), flush=True)
        else:
            print(json.dumps({"error": "unknown op"}), flush=True)
    except Exception:
        err = traceback.format_exc().strip().split("\\n")[-1]
        print(json.dumps({"error": err}), flush=True)
'
