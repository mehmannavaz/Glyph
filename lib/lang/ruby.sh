#!/bin/sh
# lib/lang/ruby.sh — Glyph language adapter for Ruby.
# Protocol: see lib/lang/python.sh

exec ruby -e '
require "json"

STDIN.each_line do |line|
  line = line.strip
  next if line.empty?
  begin
    req = JSON.parse(line)
  rescue => e
    STDOUT.write({ error: "bad JSON: #{e.message}" }.to_json + "\n")
    STDOUT.flush
    next
  end

  begin
    if req["op"] == "eval"
      result = eval(req["code"] || "")
      STDOUT.write({ value: result.nil? ? "" : result.inspect }.to_json + "\n")
      STDOUT.flush
    elsif req["op"] == "call"
      mod_name = req["module"] || ""
      mod = mod_name.empty? ? Object : Object.const_get(mod_name)
      fn = mod.method(req["function"])
      result = fn.call(*req["args"])
      STDOUT.write({ value: result.nil? ? "" : result.inspect }.to_json + "\n")
      STDOUT.flush
    else
      STDOUT.write({ error: "unknown op: #{req["op"]}" }.to_json + "\n")
      STDOUT.flush
    end
  rescue => e
    STDOUT.write({ error: e.message }.to_json + "\n")
    STDOUT.flush
  end
end
'
