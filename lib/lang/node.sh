#!/bin/sh
# lib/lang/node.sh — Glyph language adapter for Node.js / V8 / JavaScript.
#
# Protocol: see lib/lang/python.sh
# Print output is captured and returned as the value.

exec node -e '
const readline = require("readline");
const rl = readline.createInterface({ input: process.stdin });

// Persistent global namespace.
const GLYPH_GLOBAL = {};

rl.on("line", (line) => {
    line = line.trim();
    if (!line) return;
    let req;
    try { req = JSON.parse(line); } catch (e) {
        process.stdout.write(JSON.stringify({ error: "bad JSON: " + e.message }) + "\n");
        return;
    }
    try {
        if (req.op === "eval") {
            const code = req.code || "";
            // Capture console.log output
            let captured = "";
            const origLog = console.log;
            const origInfo = console.info;
            const origWarn = console.warn;
            const origErr = console.error;
            console.log = (...args) => { captured += args.join(" ") + "\n"; };
            console.info = console.log;
            console.warn = (...args) => { captured += args.join(" ") + "\n"; };
            console.error = (...args) => { captured += args.join(" ") + "\n"; };
            let result;
            try {
                try {
                    result = eval("(" + code + ")");
                } catch (e) {
                    result = eval(code);
                }
            } finally {
                console.log = origLog;
                console.info = origInfo;
                console.warn = origWarn;
                console.error = origErr;
            }
            const val = captured.trim() || (result === undefined ? "" : String(result));
            process.stdout.write(JSON.stringify({ value: val }) + "\n");
        } else if (req.op === "call") {
            let mod;
            if (req.module) {
                mod = require(req.module);
            } else {
                mod = globalThis;
            }
            const fn = mod[req.function];
            if (typeof fn !== "function") throw new Error("not a function: " + req.function);
            let captured = "";
            const origLog = console.log;
            console.log = (...args) => { captured += args.join(" ") + "\n"; };
            let result;
            try {
                result = fn(...(req.args || []));
            } finally {
                console.log = origLog;
            }
            const val = captured.trim() || (result === undefined ? "" : String(result));
            process.stdout.write(JSON.stringify({ value: val }) + "\n");
        } else {
            process.stdout.write(JSON.stringify({ error: "unknown op: " + req.op }) + "\n");
        }
    } catch (e) {
        process.stdout.write(JSON.stringify({ error: e.message }) + "\n");
    }
});

rl.on("close", () => process.exit(0));
' "$@"
