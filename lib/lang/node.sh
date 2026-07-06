#!/bin/sh
# lib/lang/node.sh — Glyph language adapter for Node.js / V8 / JavaScript.
#
# Protocol: see lib/lang/python.sh

exec node -e '
const readline = require("readline");
const rl = readline.createInterface({ input: process.stdin });

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
            // Wrap in a return-ing function so the last expression value comes back.
            // Try eval first (works for expressions), fall back to Function.
            let result;
            try {
                // Wrap in parens — if it is an expression, eval returns its value
                result = eval("(" + code + ")");
            } catch (e) {
                // Statement form: run as-is, value is undefined
                result = eval(code);
            }
            process.stdout.write(JSON.stringify({
                value: result === undefined ? "" : String(result)
            }) + "\n");
        } else if (req.op === "call") {
            let mod;
            if (req.module) {
                mod = require(req.module);
            } else {
                mod = globalThis;
            }
            const fn = mod[req.function];
            if (typeof fn !== "function") throw new Error("not a function: " + req.function);
            const result = fn(...(req.args || []));
            process.stdout.write(JSON.stringify({
                value: result === undefined ? "" : String(result)
            }) + "\n");
        } else {
            process.stdout.write(JSON.stringify({ error: "unknown op: " + req.op }) + "\n");
        }
    } catch (e) {
        process.stdout.write(JSON.stringify({ error: e.message }) + "\n");
    }
});

rl.on("close", () => process.exit(0));
' "$@"
