#!/bin/sh
# lib/lang/elm.sh — Glyph language adapter for Elm.
#
# Elm compiles to JS. We compile each request to JS and run via node.
# This is slow (~5s per request) but correct.
#
# Protocol: see lib/lang/python.sh

exec python3 -c '
import sys, json, os, subprocess, tempfile, shutil, traceback

def elm_eval(code):
    tmpdir = tempfile.mkdtemp(prefix="glyph-elm-")
    try:
        # Write an Elm module with the user code in main
        elm_src = """
port module GlyphEval exposing (..)

import Platform

""" + code + """

main : Program () () ()
main =
    Platform.worker
        { init = \\_ -> ( (), Cmd.none )
        , update = \\_ model -> ( model, Cmd.none )
        , subscriptions = \\_ -> Sub.none
        }
"""
        with open(os.path.join(tmpdir, "GlyphEval.elm"), "w") as f:
            f.write(elm_src)
        with open(os.path.join(tmpdir, "elm.json"), "w") as f:
            f.write(json.dumps({
                "type": "application",
                "source-directories": ["."],
                "elm-version": "0.19.1",
                "dependencies": {
                    "direct": {"elm/browser": "1.0.2", "elm/core": "1.0.5", "elm/html": "1.0.0"},
                    "indirect": {"elm/json": "1.1.3", "elm/time": "1.0.0", "elm/url": "1.0.0", "elm/virtual-dom": "1.0.3"}
                },
                "test-dependencies": {"direct": {}, "indirect": {}}
            }))

        r = subprocess.run(["elm", "make", "GlyphEval.elm", "--output=elm.js", "--optimize"],
                           cwd=tmpdir, capture_output=True, text=True, timeout=30)
        if r.returncode != 0:
            err_lines = r.stderr.strip().split("\n")
            return None, err_lines[-1] if err_lines else "compile error"

        # Run via node, calling init()
        runner = """
const fs = require("fs");
const Elm = require("./elm.js");
const app = Elm.GlyphEval.init();
"""
        with open(os.path.join(tmpdir, "runner.js"), "w") as f:
            f.write(runner)
        r = subprocess.run(["node", "runner.js"], cwd=tmpdir, capture_output=True, text=True, timeout=10)
        if r.returncode != 0:
            return None, "runtime error: " + r.stderr.strip().split("\n")[-1]
        return r.stdout.strip(), None
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

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
            val, err = elm_eval(req.get("code", ""))
            if err: print(json.dumps({"error": err}), flush=True)
            else:   print(json.dumps({"value": val or ""}), flush=True)
        elif req.get("op") == "call":
            print(json.dumps({"error": "lang_call not supported for Elm"}), flush=True)
        else:
            print(json.dumps({"error": "unknown op"}), flush=True)
    except Exception:
        err = traceback.format_exc().strip().split("\n")[-1]
        print(json.dumps({"error": err}), flush=True)
'
