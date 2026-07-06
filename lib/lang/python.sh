#!/bin/sh
# lib/lang/python.sh — Glyph language adapter for Python.
#
# Protocol (one JSON request per line on stdin, one JSON response per line on stdout):
#   {"op":"eval","code":"<python source>"}                    -> {"value":"<repr>"}
#   {"op":"call","module":"<name>","function":"<fn>","args":[...]} -> {"value":"<repr>"}
#
# The "value" field captures:
#   - For expressions: the repr of the result
#   - For statements: any captured stdout (print output)
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
            sys.stderr.write(json.dumps({"error": "bad JSON: " + str(e)}) + "\n")
            sys.stderr.flush()
            continue

        op = req.get("op")
        try:
            if op == "eval":
                code = req.get("code", "")
                # Capture stdout so that print() output is returned as the value
                captured = io.StringIO()
                old_stdout = sys.stdout
                sys.stdout = captured
                try:
                    try:
                        # Try eval first (for expressions)
                        val = eval(code, GLYPH_GLOBALS)
                    except SyntaxError:
                        # Not an expression — exec it
                        exec(code, GLYPH_GLOBALS)
                        val = None
                finally:
                    sys.stdout = old_stdout
                printed = captured.getvalue()
                # If the code printed something, return that as the value
                # (stripped of trailing newline). Otherwise return the eval result.
                if printed:
                    val_str = printed.rstrip("\n")
                elif val is None:
                    val_str = ""
                else:
                    val_str = repr(val)
                sys.stdout.write(json.dumps({"value": val_str}) + "\n")
                sys.stdout.flush()

            elif op == "call":
                module_name = req.get("module", "")
                fn_name = req.get("function", "")
                args = req.get("args", [])
                if module_name:
                    mod = importlib.import_module(module_name)
                else:
                    mod = __builtins__ if hasattr(__builtins__, fn_name) else __import__("builtins")
                captured = io.StringIO()
                old_stdout = sys.stdout
                sys.stdout = captured
                try:
                    fn = getattr(mod, fn_name)
                    result = fn(*args)
                finally:
                    sys.stdout = old_stdout
                printed = captured.getvalue()
                if printed:
                    val_str = printed.rstrip("\n")
                elif result is None:
                    val_str = ""
                else:
                    val_str = repr(result)
                sys.stdout.write(json.dumps({"value": val_str}) + "\n")
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
