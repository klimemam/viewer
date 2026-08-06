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

# main.cpp AND every fragment it #includes (core/ui/*.inc since S2, and the
# rest of the split as it lands). When this script read only main.cpp, the S2
# carve would have moved 16 of its 18 tables out from under it and it would
# have kept printing the remaining two without a word about the silence - a
# checker that scans nothing looks exactly like a codebase with nothing to
# report. The glob is recursive on purpose: S3-S5 fragments join the scan the
# day they exist, with no edit here.
files = [root / "core" / "main.cpp"] + sorted((root / "core").glob("**/*.inc"))

for f in files:
    src = f.read_text(encoding="utf-8").splitlines()
    rel = f.relative_to(root).as_posix()
    i = 0
    while i < len(src):
        # `[,)]` and not `,`: BeginTable("sc", 2) - count as the LAST argument,
        # no flags - is a table like any other, and the old pattern's demand
        # for a trailing comma silently exempted it from this report.
        m = re.search(r'BeginTable\("([^"]+)",\s*([A-Za-z0-9_ ()?:+*.\-]+?)\s*[,)]', src[i])
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
            print("%-14s %-26s line %-6d declared=%-24s setups=%d%s"
                  % (name, rel, i + 1, cols, setups, note))
        i += 1
