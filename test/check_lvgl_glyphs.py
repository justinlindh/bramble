#!/usr/bin/env python3
"""Fail the build if an LVGL screen tries to draw a character the font lacks.

The built-in Montserrat fonts we ship carry ASCII plus the LV_SYMBOL_* block and
almost nothing else. Any other codepoint in a display string renders as an empty
tofu box: no compiler error, no crash, no log line. It just quietly looks broken.

That shipped. The delivery badge appended a raw U+2197 arrow to the hop count and
drew a blank box next to every relayed message.

So: string literals under components/ui_graphics may contain ASCII only. Non-ASCII
goes through an LV_SYMBOL_* macro, which is guaranteed to have a glyph. Comments are
exempt (they are never drawn).
"""

import re
import sys
from pathlib import Path

UI_DIR = Path(__file__).resolve().parent.parent / "components" / "ui_graphics"

# Comments and log strings are never drawn, so they may hold any character. Blank
# them before scanning, preserving newlines so reported line numbers stay true.
COMMENTS = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
LOG_CALLS = re.compile(r"ESP_LOG[A-Z]\s*\((?:[^()]|\([^()]*\))*\)", re.S)
STRINGS = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
HEX_ESCAPE = re.compile(r"\\x([0-9a-fA-F]{2})")


def _blank(match: "re.Match") -> str:
    """Erase matched text but keep its newlines, so line numbers do not shift."""
    return re.sub(r"[^\n]", " ", match.group(0))


def offending_literals(source: str):
    """Yield (literal, reason) for each string literal holding a non-ASCII byte."""
    blanked = LOG_CALLS.sub(_blank, COMMENTS.sub(_blank, source))

    for match in STRINGS.finditer(blanked):
        literal = match.group(1)
        line = blanked.count("\n", 0, match.start()) + 1

        if any(ord(ch) > 0x7F for ch in literal):
            yield line, literal, "literal non-ASCII character"
            continue

        # "\xe2\x86\x97" is the same tofu, spelled defensively.
        if any(int(b, 16) > 0x7F for b in HEX_ESCAPE.findall(literal)):
            yield line, literal, r"non-ASCII byte via \x escape"


def main() -> int:
    findings = []
    for path in sorted(UI_DIR.rglob("*.[ch]")):
        source = path.read_text(encoding="utf-8", errors="replace")
        for line, literal, reason in offending_literals(source):
            rel = path.relative_to(UI_DIR.parent.parent)
            findings.append(f"  {rel}:{line}: {reason}: \"{literal}\"")

    if findings:
        print("FAILED: non-ASCII in an LVGL display string (renders as a tofu box)")
        print("\n".join(findings))
        print("\nUse an LV_SYMBOL_* macro instead. The built-in Montserrat fonts")
        print("contain ASCII and the LV_SYMBOL_* block only; every other codepoint")
        print("draws an empty box on the device and nothing warns you.")
        return 1

    print("LVGL glyph check: all display strings are ASCII or LV_SYMBOL_* ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
