#!/usr/bin/python3

from pathlib import Path
import re
import sys
import subprocess

if len(sys.argv) < 3:
    print(f"Syntax: {sys.argv[0]} <path-to-libdcp-source-tree> <ERROR|BV21_ERROR|WARNING>")
    sys.exit(1)

libdcp = Path(sys.argv[1])
type = sys.argv[2]
header = libdcp / "src" / "verify.h"

types = ("BV21_ERROR", "ERROR", "WARNING")

def find_type(name):
    """
    Search source code to find where a given code is used and hence find out whether
    it represents an error, Bv2.1 "error" or warning.
    """
    previous = ''
    for source in ["verify_j2k.cc", "dcp.cc", "verify.cc"]:
        path = libdcp / "src" / source
        with open(path) as s:
            for line in s:
                if line.find(name) != -1:
                    line_with_previous = previous + line
                    for t in types:
                        if line_with_previous.find(t) != -1:
                            return t
                    assert False
                previous = line


print('<itemizedlist>')

active = False
with open(header) as h:
    for line in h:
        strip = line.strip()
        if strip == "enum class Code {":
            active = True
        elif strip == "};":
            active = False
        elif active:
            if strip.startswith('/**'):
                text = strip.replace('/**', '').replace('*/', '').strip()
            elif not strip.startswith('/*') and not strip.startswith('*') and strip.endswith(','):
                this_type = find_type(strip[:-1])
                if this_type == type:
                    text = re.sub(r"\[.*?\]", lambda m: f'(Bv2.1 {m[0][7:-1]})', text)
                    text = text.replace('<', '&lt;')
                    text = text.replace('>', '&gt;')
                    text = re.sub(r"_(.*?)_", r"<code>\1</code>", text)
                    print(f'<listitem>{text}.</listitem>')

print('</itemizedlist>')
