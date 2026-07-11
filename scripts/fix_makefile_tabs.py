#!/usr/bin/env python3
# fix_makefile_tabs.py — restore tab indentation on Makefile recipe lines.
#
# The Edit/Write tools convert literal tabs to spaces, which breaks GNU make
# (recipes MUST start with a tab). This script rewrites the Makefile so that
# any line whose stripped content begins with a recipe token ($(CXX), ./, rm,
# or a make function call) gets a single leading tab.
#
# Run: python3 scripts/fix_makefile_tabs.py
import re, sys, pathlib

MF = pathlib.Path("/home/z/my-project/work/esp-glow/Makefile")
text = MF.read_text()

# Recipe tokens that must live on a tab-indented line.
recipe_re = re.compile(r'^[ \t]+(\$\(CXX\)|\./|rm |\$\(CXXFLAGS\) -c)')

out = []
for line in text.splitlines(keepends=True):
    bare = line.rstrip('\n')
    if recipe_re.match(bare):
        # Strip ALL leading whitespace and replace with a single tab.
        stripped = bare.lstrip(' \t')
        out.append('\t' + stripped + '\n')
    else:
        out.append(line)

MF.write_text(''.join(out))
print("Makefile recipe tabs restored.")

# Verify: no recipe line should start with a space.
bad = []
for i, line in enumerate(MF.read_text().splitlines(), 1):
    if recipe_re.match(line) and not line.startswith('\t'):
        bad.append((i, line))
if bad:
    print("STILL BAD:", bad, file=sys.stderr); sys.exit(1)
print("OK: all recipe lines start with a tab.")
