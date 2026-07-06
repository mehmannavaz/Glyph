#!/bin/sh
# lib/lang/bash.sh — Glyph language adapter for bash.
# Bash is itself a shell, so we just eval directly.
#
# Protocol: see lib/lang/python.sh

exec python3 -c '
import sys, json, subprocess, traceback

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
            code = req.get("code", "")
            r = subprocess.run(["bash", "-c", code],
                               capture_output=True, text=True, timeout=30)
            if r.returncode != 0:
                err = r.stderr.strip().split("\n")[-1] if r.stderr else "exit " + str(r.returncode)
                print(json.dumps({"error": err}), flush=True)
            else:
                print(json.dumps({"value": r.stdout.strip()}), flush=True)
        elif req.get("op") == "call":
            # bash has no modules — translate to sourcing a file and calling a function
            mod = req.get("module", "")
            fn  = req.get("function", "")
            args = req.get("args", [])
            if mod:
                cmd = "source " + mod + " && " + fn + " " + " ".join("\"" + str(a) + "\"" for a in args)
            else:
                cmd = fn + " " + " ".join("\"" + str(a) + "\"" for a in args)
            r = subprocess.run(["bash", "-c", cmd],
                               capture_output=True, text=True, timeout=30)
            if r.returncode != 0:
                print(json.dumps({"error": r.stderr.strip()[:200]}), flush=True)
            else:
                print(json.dumps({"value": r.stdout.strip()}), flush=True)
        else:
            print(json.dumps({"error": "unknown op"}), flush=True)
    except Exception:
        err = traceback.format_exc().strip().split("\n")[-1]
        print(json.dumps({"error": err}), flush=True)
'
