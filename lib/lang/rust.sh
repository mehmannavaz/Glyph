#!/bin/sh
# lib/lang/rust.sh — Glyph language adapter for Rust.
#
# Rust is compiled, not interpreted. We use the `evcxr` REPL if available,
# otherwise fall back to a quick compile-and-run via rustc.
#
# Protocol: see lib/lang/python.sh

if command -v evcxr >/dev/null 2>&1; then
    # evcxr is an actual Rust REPL — use it for persistent state
    exec evcxr --repl-lang rust <<'EOF'
EOF
else
    # Fall back: compile and run each request as a one-shot program.
    # Slow but correct.
    exec python3 -c '
import sys, json, os, subprocess, tempfile, traceback

def rust_eval(code):
    # Wrap user code in a main function and compile
    full = """
fn main() {
    let result = (|| {
        """ + code + """
    })();
    println!(\"{:?}\", result);
}
"""
    with tempfile.NamedTemporaryFile(suffix=".rs", mode="w", delete=False) as f:
        f.write(full)
        path = f.name
    try:
        out = path + ".out"
        r = subprocess.run(["rustc", "--edition", "2021", "-O", "-o", out, path],
                           capture_output=True, text=True, timeout=30)
        if r.returncode != 0:
            return None, r.stderr.strip().split("\n")[-1]
        r = subprocess.run([out], capture_output=True, text=True, timeout=10)
        if r.returncode != 0:
            return None, "runtime error: " + r.stderr.strip().split("\n")[-1]
        return r.stdout.strip(), None
    finally:
        try: os.unlink(path)
        except: pass
        try: os.unlink(out)
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
            val, err = rust_eval(req.get("code", ""))
            if err: print(json.dumps({"error": err}), flush=True)
            else:   print(json.dumps({"value": val or ""}), flush=True)
        elif req.get("op") == "call":
            # Rust has no runtime reflection — would need a registry.
            print(json.dumps({"error": "lang_call not supported for Rust (no runtime reflection)"}), flush=True)
        else:
            print(json.dumps({"error": "unknown op"}), flush=True)
    except Exception:
        err = traceback.format_exc().strip().split("\n")[-1]
        print(json.dumps({"error": err}), flush=True)
'
fi
