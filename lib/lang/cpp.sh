#!/bin/sh
# lib/lang/cpp.sh — Glyph language adapter for C++.
#
# Compile-and-run. Slow but correct.
#
# Protocol: see lib/lang/python.sh

exec python3 -c '
import sys, json, os, subprocess, tempfile, traceback

def cpp_eval(code):
    full = """
#include <iostream>
#include <string>
#include <cstdint>
#include <vector>
#include <map>
#include <algorithm>
#include <numeric>
int main() {
    using namespace std;
""" + code + """
    return 0;
}
"""
    with tempfile.NamedTemporaryFile(suffix=".cpp", mode="w", delete=False) as f:
        f.write(full)
        path = f.name
    try:
        out = path + ".out"
        r = subprocess.run(["g++", "-std=c++17", "-O2", "-o", out, path],
                           capture_output=True, text=True, timeout=30)
        if r.returncode != 0:
            err_lines = r.stderr.strip().split("\n")
            return None, err_lines[-1] if err_lines else "compile error"
        r = subprocess.run([out], capture_output=True, text=True, timeout=10)
        if r.returncode != 0:
            return None, "runtime error: " + r.stderr.strip().split("\n")[-1]
        return r.stdout.strip(), None
    finally:
        for p in [path, out]:
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
            val, err = cpp_eval(req.get("code", ""))
            if err: print(json.dumps({"error": err}), flush=True)
            else:   print(json.dumps({"value": val or ""}), flush=True)
        elif req.get("op") == "call":
            print(json.dumps({"error": "lang_call not supported for C++ (no runtime reflection)"}), flush=True)
        else:
            print(json.dumps({"error": "unknown op"}), flush=True)
    except Exception:
        err = traceback.format_exc().strip().split("\n")[-1]
        print(json.dumps({"error": err}), flush=True)
'
