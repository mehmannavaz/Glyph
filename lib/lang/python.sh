#!/bin/sh
# lib/lang/python.sh — Glyph language adapter for Python.
#
# Protocol (one JSON request per line on stdin, one JSON response per line on stdout):
#   {"op":"eval","code":"<python source>"}                    -> {"value":"<repr>"}
#   {"op":"call","module":"<name>","function":"<fn>","args":[...]} -> {"value":"<repr>"}
#
# Errors: {"error":"<message>"}
#
# Plan 9 way: do one thing — read JSON, run Python, write JSON. No state.

exec python3 -c '
import sys, json, io, traceback, importlib

# Persistent global namespace — survives across requests in the same pipe session.
GLYPH_GLOBALS = {"__name__": "__glyph__", "__builtins__": __builtins__}

def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception as e:
            sys.stdout.write(json.dumps({"error": "bad JSON: " + str(e)}) + "\n")
            sys.stdout.flush()
            continue

        op = req.get("op")
        try:
            if op == "eval":
                code = req.get("code", "")
                try:
                    # Try eval first (for expressions)
                    val = eval(code, GLYPH_GLOBALS)
                except SyntaxError:
                    # Not an expression — exec it
                    exec(code, GLYPH_GLOBALS)
                    val = GLYPH_GLOBALS.get("_", None)
                sys.stdout.write(json.dumps({"value": "" if val is None else repr(val)}) + "\n")
                sys.stdout.flush()

            elif op == "call":
                module_name = req.get("module", "")
                fn_name = req.get("function", "")
                args = req.get("args", [])
                if module_name:
                    mod = importlib.import_module(module_name)
                else:
                    mod = __builtins__ if hasattr(__builtins__, fn_name) else __import__("builtins")
                fn = getattr(mod, fn_name)
                result = fn(*args)
                sys.stdout.write(json.dumps({"value": "" if result is None else repr(result)}) + "\n")
                sys.stdout.flush()
            else:
                sys.stdout.write(json.dumps({"error": "unknown op: " + str(op)}) + "\n")
                sys.stdout.flush()

        except Exception:
            err = traceback.format_exc().strip().split("\n")[-1]
            sys.stdout.write(json.dumps({"error": err}) + "\n")
            sys.stdout.flush()

main()
' "$@"
