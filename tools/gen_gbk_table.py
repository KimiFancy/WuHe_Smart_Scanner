#!/usr/bin/env python3
"""
gen_gbk_table.py — Generate a dense GBK→Unicode lookup table as a C source.

Produces main/gbk_table.c containing:
    const uint16_t gbk_to_unicode[126][190];

Indexing:
    lead byte  0x81..0xFE  ->  lead - 0x81              (0..125)
    trail byte 0x40..0x7E  ->  trail - 0x40             (0..62)
    trail byte 0x80..0xFE  ->  trail - 0x80 + 63        (63..189)

Undefined cells are 0. The table is built from Python's 'gbk' codec, which
implements the GBK standard (a superset of GB2312). Run:

    python3 tools/gen_gbk_table.py
"""

import os
import sys

LEAD_LO, LEAD_HI = 0x81, 0xFE          # 126 rows
TRAIL_SPLIT = 0x7F                      # gap: trail 0x7F is unused by GBK
# trail columns: 0x40-0x7E (63) + 0x80-0xFE (127) = 190 cols
COLS = 190
ROWS = LEAD_HI - LEAD_LO + 1            # 126


def trail_index(t: int) -> int:
    """Map a GBK trail byte to a column index, or -1 if invalid."""
    if 0x40 <= t <= 0x7E:
        return t - 0x40
    if 0x80 <= t <= 0xFE:
        return t - 0x80 + 63
    return -1


def build_table():
    table = [[0] * COLS for _ in range(ROWS)]
    defined = 0
    for lead in range(LEAD_LO, LEAD_HI + 1):
        for trail in range(0x00, 0x100):
            ti = trail_index(trail)
            if ti < 0:
                continue
            try:
                ch = bytes([lead, trail]).decode("gbk")
            except (UnicodeDecodeError, ValueError):
                continue
            cp = ord(ch)
            # Don't store ASCII-mapped codepoints as 0; keep them.
            table[lead - LEAD_LO][ti] = cp
            defined += 1
    return table, defined


def emit(table, defined, out_path):
    total = ROWS * COLS
    lines = []
    lines.append("/*")
    lines.append(" * gbk_table.c — GENERATED FILE. Do not edit by hand.")
    lines.append(" *")
    lines.append(" * Dense GBK→Unicode codepoint lookup table.")
    lines.append(" * Regenerate with:  python3 tools/gen_gbk_table.py")
    lines.append(" *")
    lines.append(" * Coverage: %d defined GBK double-byte sequences" % defined)
    lines.append(" * out of a %dx%d = %d-cell space." % (ROWS, COLS, total))
    lines.append(" *")
    lines.append(" * Indexing (see tools/gen_gbk_table.py):")
    lines.append(" *   lead byte  0x81..0xFE  ->  lead - 0x81")
    lines.append(" *   trail byte 0x40..0x7E  ->  trail - 0x40")
    lines.append(" *   trail byte 0x80..0xFE  ->  trail - 0x80 + 63")
    lines.append(" *")
    lines.append(" * Undefined cells are 0 (gbk_to_utf8 emits U+FFFD for those).")
    lines.append(" */")
    lines.append("")
    lines.append('#include "gbk_utf8.h"')
    lines.append("")
    lines.append("/* Flash-resident const table. The linker places this in .rodata.")
    lines.append(" * Size: %d bytes (%d KiB). */" % (total * 2, total * 2 // 1024))
    lines.append("const uint16_t gbk_to_unicode[%d][%d] = {" % (ROWS, COLS))

    for r in range(ROWS):
        row = table[r]
        cells = ["0x{:04X}".format(row[c]) for c in range(COLS)]
        chunks = [", ".join(cells[i:i + 10]) for i in range(0, COLS, 10)]
        lead_val = LEAD_LO + r
        lines.append("    /* lead 0x{:02X} */".format(lead_val))
        lines.append("    {")
        for ci, chunk in enumerate(chunks):
            suffix = "," if ci < len(chunks) - 1 else ""
            lines.append("    " + chunk + suffix)
        row_suffix = "," if r < ROWS - 1 else ""
        lines.append("    }" + row_suffix)
    lines.append("};")
    lines.append("")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    return len("\n".join(lines))


def self_test():
    """Verify a few known mappings match the scanner's observed bytes."""
    # (gbk_bytes, expected_char, expected_codepoint) — derive gbk from char
    # to avoid manual byte mistakes.
    checks = [
        (b"\xcb\xae", "水", 0x6C34),  # scanner sample
        (b"\xc0\xb6", "蓝", 0x84DD),
        (b"\xc9\xab", "色", 0x8272),
        (b"\xd6\xd0", "中", 0x4E2D),
        ("仓".encode("gbk"), "仓", 0x4ED3),
        ("库".encode("gbk"), "库", 0x5E93),
    ]
    ok = True
    for gb, ch, cp in checks:
        got = ord(gb.decode("gbk"))
        status = "OK" if got == cp == ord(ch) else "FAIL"
        if status == "FAIL":
            ok = False
        print("  self-test [{}] {:4s} U+{:04X} gbk={}".format(
            status, ch, got, gb.hex()))
    return ok


def main():
    print("Building GBK→Unicode table...")
    if not self_test():
        print("ERROR: self-test failed, aborting", file=sys.stderr)
        sys.exit(1)

    table, defined = build_table()
    total = ROWS * COLS
    print("Defined: {}/{} cells ({:.1%})".format(defined, total, defined / total))

    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "main", "gbk_table.c")
    out = os.path.normpath(out)
    size = emit(table, defined, out)
    print("Wrote {} ({} bytes of C source)".format(out, size))
    print("Flash footprint: {} bytes ({:.0f} KiB)".format(total * 2, total * 2 / 1024))


if __name__ == "__main__":
    main()
