"""Report every ImGui table's declared column count next to the number of
TableSetupColumn calls that follow it.

A table whose two numbers disagree hits "TableSetupColumn() called too many
times" and then draws wrong. That is how the Projection panel shipped broken:
a column was added to the setup list and the literal count beside BeginTable
was not touched. The two live tens of lines apart, so nothing in the source
makes the mismatch visible - you find out from an assert at runtime.

This does NOT fail the build. Setups written in a loop count as one line, so
some rows disagree legitimately. It is a thing to READ after touching a table,
not a gate.

    python tools/check_table_columns.py
"""
import re
import pathlib

root = pathlib.Path(__file__).resolve().parent.parent
src = (root / "core" / "main.cpp").read_text(encoding="utf-8").splitlines()

i = 0
while i < len(src):
    m = re.search(r'BeginTable\("([^"]+)",\s*([A-Za-z0-9_ ()?:+*.\-]+?),', src[i])
    if m:
        name, cols = m.group(1), m.group(2).strip()
        setups = 0
        j = i + 1
        while j < len(src) and j < i + 90:
            if "TableSetupColumn" in src[j]:
                setups += 1
            if "TableHeadersRow" in src[j] or "TableNextRow" in src[j]:
                break
            if "BeginTable(" in src[j]:
                break
            j += 1
        loop = any("for (" in src[k] for k in range(i, min(j, i + 90)))
        note = "  (setups in a loop - count by hand)" if loop else ""
        print("%-14s line %-6d declared=%-24s setups=%d%s" % (name, i + 1, cols, setups, note))
    i += 1
