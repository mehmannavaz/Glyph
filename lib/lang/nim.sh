#!/bin/sh
# lib/lang/nim.sh — Glyph language adapter for Nim.
#
# Nim has `nim secret` (a REPL) but it is limited. We use compile-and-run.
#
# Protocol: see lib/lang/python.sh

exec python3 -c '
import sys, json, os, subprocess, tempfile, traceback

def nim_eval(code):
    # Nim scripts: use nim e - to evaluate a Nimscript file
    full = """
import std/strutils, std/sequtils, std/algorithm
""" + code + """
"""
    with tempfile.NamedTemporaryFile(suffix=".nim", mode="w", delete=False) as f:
        f.write(full)
        path = f.name
    try:
        r = subprocess.run(["nim", "e", "--hints:off", "--warnings:off", path],
                           capture_output=True, text=True, timeout=30)
        if r.returncode != 0:
            err_lines = r.stderr.strip().split("\n")
            return None, err_lines[-1] if err_lines else "compile error"
        return r.stdout.strip(), None
    finally:
        try: os.unlink(path)
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
            val, err = nim_eval(req.get("code", ""))
            if err: print(json.dumps({"error": err}), flush=True)
            else:   print(json.dumps({"value": val or ""}), flush=True)
        elif req.get("op") == "call":
            print(json.dumps({"error": "lang_call not supported for Nim (no runtime reflection)"}), flush=True)
        else:
            print(json.dumps({"error": "unknown op"}), flush=True)
    except Exception:
        err = traceback.format_exc().strip().split("\n")[-1]
        print(json.dumps({"error": err}), flush=True)
'
