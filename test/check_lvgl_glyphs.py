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

That "guaranteed" used to be a manual check: the battery charging indicator added
LV_SYMBOL_CHARGE and its codepoint was verified by hand against
lv_font_montserrat_12's compiled glyph set, which means nothing caught it if that
verification had been wrong. This script also resolves every LV_SYMBOL_* identifier
used under components/ui_graphics to its real codepoint and checks that codepoint is
actually baked into the compiled font, so a symbol name that exists as a macro but
was dropped from (or never had) a glyph fails the same way a raw non-ASCII literal
does.
"""

import re
import sys
from pathlib import Path

UI_DIR = Path(__file__).resolve().parent.parent / "components" / "ui_graphics"
REPO_ROOT = UI_DIR.parent.parent

# Comments and log strings are never drawn, so they may hold any character. Blank
# them before scanning, preserving newlines so reported line numbers stay true.
COMMENTS = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
LOG_CALLS = re.compile(r"ESP_LOG[A-Z]\s*\((?:[^()]|\([^()]*\))*\)", re.S)
STRINGS = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
HEX_ESCAPE = re.compile(r"\\x([0-9a-fA-F]{2})")
SYMBOL_USE = re.compile(r"\bLV_SYMBOL_[A-Z0-9_]+\b")

# --- LV_SYMBOL_* -> codepoint, and which codepoints the compiled fonts carry ---
#
# Ground truth lives in the vendored LVGL sources under managed_components/, which
# is gitignored and only exists once ESP-IDF's component manager has fetched it (a
# board build, not a plain `bash test/run_all_tests.sh`). The tables below are read
# from those files live when present (_load_symbol_table/_load_font_codepoints), so
# a local dev box or a CI job that also builds firmware gets checked against the
# real, current vendor sources. Everywhere else (in particular the host-tests-only
# CI path, which deliberately never fetches managed_components, see docs/ci.md)
# falls back to the frozen snapshot below.
#
# Snapshotted 2026-08-01 from managed_components/lvgl__lvgl at lvgl 9.2.2:
# lv_symbol_def.h's macro bodies decoded as UTF-8 (not its trailing decimal/hex
# comment, which disagrees with the real bytes for at least one symbol,
# LV_SYMBOL_FILE: comment says 0xF158, the actual encoded bytes are 0xF15B,
# which is also what the font's glyph table uses), and lv_font_montserrat_12.c's
# compiled glyph range (confirmed identical to the 14 and 16 variants also
# enabled by this build). Regenerate by rerunning this script's loaders against
# a freshly fetched managed_components/ after any lvgl version bump, and paste
# the printed literals back in here, updating _SNAPSHOT_LVGL_VERSION below to
# match.
#
# components/ui_graphics/idf_component.yml pins lvgl/lvgl to the FLOATING range
# "~9.2" (any 9.2.x), not an exact version: dependencies.lock is what actually
# pins a build to one resolved version (9.2.2 as of this snapshot), and that
# lockfile can change (a dependency bump elsewhere regenerates it) without this
# file's author noticing. quality.yml's host-tests job never fetches
# managed_components/, so it always takes the fallback path below; if the lock
# ever drifted to a newer 9.2.x with different glyph data, that job would keep
# silently passing against stale data forever. _snapshot_is_confirmed_compatible()
# below is the guard.
#
# dependencies.lock is GITIGNORED (not tracked): it only exists in a checkout
# where an idf.py build has already run at least once, so a genuinely clean
# checkout (a fresh `git clone` or `git archive` export, which is exactly what
# CI's host-tests job runs from) has no lockfile to read at all. The guard
# therefore has two tiers: when the lockfile IS present, it hard-fails on any
# version disagreement (the strongest evidence available). When it is absent,
# it falls back to confirming _SNAPSHOT_LVGL_VERSION still satisfies the
# RANGE declared in the tracked idf_component.yml manifest. That fallback is
# honestly weaker: it cannot detect a same-range float (dependencies.lock
# silently moving from 9.2.2 to 9.2.3, both inside "~9.2") from tracked files
# alone, because nothing tracked records the exact resolved version in that
# case. It only catches the manifest's declared range itself moving away from
# what the snapshot covers (e.g. a bump to "~9.3"). The live path above
# remains the only fully authoritative check, and stays preferred whenever
# managed_components/ has actually been fetched (a real board build, or a
# dev box that has run one).
_SNAPSHOT_LVGL_VERSION = "9.2.2"

_FALLBACK_SYMBOL_CODEPOINTS = {
    "LV_SYMBOL_AUDIO": 0xF001,
    "LV_SYMBOL_BACKSPACE": 0xF55A,
    "LV_SYMBOL_BARS": 0xF0C9,
    "LV_SYMBOL_BATTERY_1": 0xF243,
    "LV_SYMBOL_BATTERY_2": 0xF242,
    "LV_SYMBOL_BATTERY_3": 0xF241,
    "LV_SYMBOL_BATTERY_EMPTY": 0xF244,
    "LV_SYMBOL_BATTERY_FULL": 0xF240,
    "LV_SYMBOL_BELL": 0xF0F3,
    "LV_SYMBOL_BLUETOOTH": 0xF293,
    "LV_SYMBOL_BULLET": 0x2022,
    "LV_SYMBOL_CALL": 0xF095,
    "LV_SYMBOL_CHARGE": 0xF0E7,
    "LV_SYMBOL_CLOSE": 0xF00D,
    "LV_SYMBOL_COPY": 0xF0C5,
    "LV_SYMBOL_CUT": 0xF0C4,
    "LV_SYMBOL_DIRECTORY": 0xF07B,
    "LV_SYMBOL_DOWN": 0xF078,
    "LV_SYMBOL_DOWNLOAD": 0xF019,
    "LV_SYMBOL_DRIVE": 0xF01C,
    "LV_SYMBOL_DUMMY": 0xF8FF,
    "LV_SYMBOL_EDIT": 0xF304,
    "LV_SYMBOL_EJECT": 0xF052,
    "LV_SYMBOL_ENVELOPE": 0xF0E0,
    "LV_SYMBOL_EYE_CLOSE": 0xF070,
    "LV_SYMBOL_EYE_OPEN": 0xF06E,
    "LV_SYMBOL_FILE": 0xF15B,
    "LV_SYMBOL_GPS": 0xF124,
    "LV_SYMBOL_HOME": 0xF015,
    "LV_SYMBOL_IMAGE": 0xF03E,
    "LV_SYMBOL_KEYBOARD": 0xF11C,
    "LV_SYMBOL_LEFT": 0xF053,
    "LV_SYMBOL_LIST": 0xF00B,
    "LV_SYMBOL_LOOP": 0xF079,
    "LV_SYMBOL_MINUS": 0xF068,
    "LV_SYMBOL_MUTE": 0xF026,
    "LV_SYMBOL_NEW_LINE": 0xF8A2,
    "LV_SYMBOL_NEXT": 0xF051,
    "LV_SYMBOL_OK": 0xF00C,
    "LV_SYMBOL_PASTE": 0xF0EA,
    "LV_SYMBOL_PAUSE": 0xF04C,
    "LV_SYMBOL_PLAY": 0xF04B,
    "LV_SYMBOL_PLUS": 0xF067,
    "LV_SYMBOL_POWER": 0xF011,
    "LV_SYMBOL_PREV": 0xF048,
    "LV_SYMBOL_REFRESH": 0xF021,
    "LV_SYMBOL_RIGHT": 0xF054,
    "LV_SYMBOL_SAVE": 0xF0C7,
    "LV_SYMBOL_SD_CARD": 0xF7C2,
    "LV_SYMBOL_SETTINGS": 0xF013,
    "LV_SYMBOL_SHUFFLE": 0xF074,
    "LV_SYMBOL_STOP": 0xF04D,
    "LV_SYMBOL_TINT": 0xF043,
    "LV_SYMBOL_TRASH": 0xF2ED,
    "LV_SYMBOL_UP": 0xF077,
    "LV_SYMBOL_UPLOAD": 0xF093,
    "LV_SYMBOL_USB": 0xF287,
    "LV_SYMBOL_VIDEO": 0xF008,
    "LV_SYMBOL_VOLUME_MAX": 0xF028,
    "LV_SYMBOL_VOLUME_MID": 0xF027,
    "LV_SYMBOL_WARNING": 0xF071,
    "LV_SYMBOL_WIFI": 0xF1EB,
}

# LV_SYMBOL_DUMMY (0xF8FF) is deliberately excluded: it is LVGL's internal
# placeholder codepoint, never backed by a real glyph in any font, and no
# screen should ever display it.
_FALLBACK_FONT_CODEPOINTS = frozenset(
    {0xB0, 0x2022}
    | {
        0xF001, 0xF008, 0xF00B, 0xF00C, 0xF00D, 0xF011, 0xF013, 0xF015, 0xF019,
        0xF01C, 0xF021, 0xF026, 0xF027, 0xF028, 0xF03E, 0xF043, 0xF048, 0xF04B,
        0xF04C, 0xF04D, 0xF051, 0xF052, 0xF053, 0xF054, 0xF067, 0xF068, 0xF06E,
        0xF070, 0xF071, 0xF074, 0xF077, 0xF078, 0xF079, 0xF07B, 0xF093, 0xF095,
        0xF0C4, 0xF0C5, 0xF0C7, 0xF0C9, 0xF0E0, 0xF0E7, 0xF0EA, 0xF0F3, 0xF11C,
        0xF124, 0xF15B, 0xF1EB, 0xF240, 0xF241, 0xF242, 0xF243, 0xF244, 0xF287,
        0xF293, 0xF2ED, 0xF304, 0xF55A, 0xF7C2, 0xF8A2,
    }
)

_SYMBOL_DEF_DEFINE = re.compile(r'#define\s+(LV_SYMBOL_\w+)\s+"((?:\\x[0-9A-Fa-f]{2})+)"')
_FONT_UNICODE_LIST = re.compile(
    r"static const uint16_t unicode_list_1\[\] = \{([^}]*)\};", re.S
)
# [^{}]* stays inside a single cmap struct entry, so this cannot pair
# unicode_list_1 with a different entry's range_start: the first cmap entry
# (plain ASCII) has its own range_start = 32 earlier in the file, and a lazy
# dot-matches-all span would happily skip over the entry boundary to reach
# unicode_list_1 in the second entry instead of stopping at its own.
_FONT_SPARSE_RANGE = re.compile(
    r"\{[^{}]*\.range_start\s*=\s*(\d+)[^{}]*\.unicode_list\s*=\s*unicode_list_1[^{}]*\}"
)


# Each repeated line must start with whitespace followed by a non-whitespace
# character, so the split between the indent and the rest of the line is
# unambiguous: an all-whitespace line cannot be carved up two ways, which is
# what made the previous [ \t]+.* form exponential under backtracking.
_LOCKFILE_LVGL_VERSION = re.compile(
    r"lvgl/lvgl:\n(?:[ \t]+\S[^\n]*\n)*?[ \t]+version:[ \t]*([0-9][0-9.]*)"
)
_MANIFEST_LVGL_VERSION = re.compile(r"lvgl/lvgl:\s*\n\s*version:\s*[\"']?([^\"'\n]+)")


def _locked_lvgl_version():
    """Return the lvgl/lvgl version pinned in the root dependencies.lock: the
    exact version ESP-IDF's component manager resolved to the last time a
    build ran in this checkout. dependencies.lock is gitignored, so this is
    None (not just "missing", genuinely absent) in a checkout where no
    idf.py build has ever run, e.g. a fresh clone or a CI job that only
    exercises the host-tests-only path. None also on an unparseable lvgl
    entry.
    """
    lock_path = REPO_ROOT / "dependencies.lock"
    try:
        text = lock_path.read_text(encoding="utf-8")
    except OSError:
        return None
    match = _LOCKFILE_LVGL_VERSION.search(text)
    return match.group(1) if match else None


def _manifest_lvgl_range():
    """Return the lvgl/lvgl version range declared in
    components/ui_graphics/idf_component.yml: TRACKED, unlike
    dependencies.lock, so it is always readable even in a checkout that has
    never run a build. None if it cannot be parsed."""
    manifest_path = REPO_ROOT / "components/ui_graphics/idf_component.yml"
    try:
        text = manifest_path.read_text(encoding="utf-8")
    except OSError:
        return None
    match = _MANIFEST_LVGL_VERSION.search(text)
    return match.group(1).strip() if match else None


def _version_tuple(version_str):
    return tuple(int(p) for p in version_str.split("."))


def _snapshot_satisfies_manifest_range(snapshot_version, range_str):
    """True if snapshot_version satisfies range_str. Handles exactly the two
    syntaxes actually used in this repo's idf_component.yml files: a bare
    exact version ("9.2.2") or a tilde range ("~9.2", meaning >=9.2.0
    <9.3.0, or "~9.2.2", meaning >=9.2.2 <9.3.0). This is NOT a general
    semver range parser: an unrecognized syntax returns False (fail loudly)
    rather than silently assuming the snapshot is still compatible."""
    range_str = range_str.strip()
    try:
        if range_str.startswith("~"):
            base_parts = _version_tuple(range_str[1:].strip())
            if len(base_parts) not in (2, 3):
                return False
            major, minor = base_parts[0], base_parts[1]
            snap_parts = _version_tuple(snapshot_version)
            if len(snap_parts) != 3 or snap_parts[0] != major or snap_parts[1] != minor:
                return False
            return len(base_parts) == 2 or snap_parts >= base_parts
        return _version_tuple(snapshot_version) == _version_tuple(range_str)
    except ValueError:
        return False


def _snapshot_is_confirmed_compatible():
    """(ok, reason) for whether the frozen fallback tables above can be
    trusted for this run. Only called when managed_components/ is absent
    (the live path is always preferred when it is present, see main()).

    Two tiers, strongest evidence first:

    1. dependencies.lock present (a build has run in this checkout at least
       once): hard match against the exact resolved version. Any
       disagreement is a hard failure, not a warning.
    2. dependencies.lock absent (a genuinely clean checkout: see its
       docstring): fall back to confirming the snapshot's version still
       satisfies the tracked manifest's declared range. Weaker by
       construction (see the comment above _SNAPSHOT_LVGL_VERSION for the
       honest limit), but still catches the range itself moving away from
       what the snapshot covers, and never silently passes when it cannot
       even parse that much.
    """
    locked = _locked_lvgl_version()
    if locked is not None:
        if locked != _SNAPSHOT_LVGL_VERSION:
            return False, (
                f"dependencies.lock pins lvgl {locked}, but the frozen glyph "
                f"snapshot above was captured from lvgl {_SNAPSHOT_LVGL_VERSION}"
            )
        return True, None

    manifest_range = _manifest_lvgl_range()
    if manifest_range is None:
        return False, (
            "could not read lvgl/lvgl's version range from "
            f"{REPO_ROOT / 'components/ui_graphics/idf_component.yml'} (and there is no "
            f"{REPO_ROOT / 'dependencies.lock'} in this checkout to check against instead)"
        )
    if not _snapshot_satisfies_manifest_range(_SNAPSHOT_LVGL_VERSION, manifest_range):
        return False, (
            f"components/ui_graphics/idf_component.yml declares lvgl {manifest_range}, "
            f"which the frozen glyph snapshot's lvgl {_SNAPSHOT_LVGL_VERSION} does not satisfy"
        )
    return True, None


def _load_symbol_table():
    """Return {LV_SYMBOL_NAME: codepoint}, live from managed_components if
    fetched, else the frozen snapshot above."""
    path = REPO_ROOT / "managed_components/lvgl__lvgl/src/font/lv_symbol_def.h"
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return dict(_FALLBACK_SYMBOL_CODEPOINTS)

    table = {}
    for name, escaped in _SYMBOL_DEF_DEFINE.findall(text):
        raw = bytes(int(b, 16) for b in re.findall(r"\\x([0-9A-Fa-f]{2})", escaped))
        try:
            decoded = raw.decode("utf-8")
        except UnicodeDecodeError:
            continue
        if len(decoded) == 1:
            table[name] = ord(decoded)
    return table if table else dict(_FALLBACK_SYMBOL_CODEPOINTS)


def _load_font_codepoints():
    """Return the set of non-ASCII codepoints compiled into
    lv_font_montserrat_12 (identical to the 14/16 variants, see the snapshot
    comment above), live if managed_components is fetched, else frozen."""
    path = REPO_ROOT / "managed_components/lvgl__lvgl/src/font/lv_font_montserrat_12.c"
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return _FALLBACK_FONT_CODEPOINTS

    list_match = _FONT_UNICODE_LIST.search(text)
    range_match = _FONT_SPARSE_RANGE.search(text)
    if not list_match or not range_match:
        return _FALLBACK_FONT_CODEPOINTS

    range_start = int(range_match.group(1))
    values = [
        int(tok.strip(), 16) for tok in list_match.group(1).split(",") if tok.strip()
    ]
    codepoints = frozenset(range_start + v for v in values)
    return codepoints if codepoints else _FALLBACK_FONT_CODEPOINTS


def _blank_source(source: str) -> str:
    """Erase comments and ESP_LOG* calls, keeping newlines so line numbers hold."""
    return LOG_CALLS.sub(_blank, COMMENTS.sub(_blank, source))


def _blank(match: "re.Match") -> str:
    """Erase matched text but keep its newlines, so line numbers do not shift."""
    return re.sub(r"[^\n]", " ", match.group(0))


def offending_literals(source: str):
    """Yield (line, literal, reason) for each string literal holding a non-ASCII byte."""
    blanked = _blank_source(source)

    for match in STRINGS.finditer(blanked):
        literal = match.group(1)
        line = blanked.count("\n", 0, match.start()) + 1

        if any(ord(ch) > 0x7F for ch in literal):
            yield line, literal, "literal non-ASCII character"
            continue

        # "\xe2\x86\x97" is the same tofu, spelled defensively.
        if any(int(b, 16) > 0x7F for b in HEX_ESCAPE.findall(literal)):
            yield line, literal, r"non-ASCII byte via \x escape"


def offending_symbols(source: str, symbol_table: dict, font_codepoints: frozenset):
    """Yield (line, name, reason) for each LV_SYMBOL_* use that will not draw."""
    blanked = _blank_source(source)

    for match in SYMBOL_USE.finditer(blanked):
        name = match.group(0)
        line = blanked.count("\n", 0, match.start()) + 1

        codepoint = symbol_table.get(name)
        if codepoint is None:
            yield line, name, "not a known LV_SYMBOL_* macro"
            continue
        if codepoint not in font_codepoints:
            yield line, name, f"U+{codepoint:04X} has no glyph in the compiled font"


def main() -> int:
    managed_components_present = (REPO_ROOT / "managed_components/lvgl__lvgl").exists()

    # The live path (reading managed_components/ directly) is always
    # preferred and needs no version confirmation: it IS the current
    # vendor source. The frozen fallback below it is a snapshot of a past
    # run of that live path, so before trusting it, confirm the version it
    # was captured from still matches what a real build would actually
    # fetch (see _snapshot_is_confirmed_compatible's docstring for why this
    # cannot be skipped: it is exactly the path host-tests-only CI always
    # takes).
    if not managed_components_present:
        ok, reason = _snapshot_is_confirmed_compatible()
        if not ok:
            print("FAILED: cannot confirm the frozen LVGL glyph snapshot is still valid")
            print(f"  {reason}")
            print("\nThis check has no live managed_components/ to verify against here (the")
            print("host-tests-only CI path never fetches it), so it refuses to trust a")
            print("snapshot it cannot confirm is still compatible instead of silently passing")
            print("against data that may no longer be accurate. Regenerate the snapshot (see")
            print("the comment above _SNAPSHOT_LVGL_VERSION) against the currently resolved")
            print("lvgl version and update that constant.")
            return 1

    symbol_table = _load_symbol_table()
    font_codepoints = _load_font_codepoints()

    findings = []
    for path in sorted(UI_DIR.rglob("*.[ch]")):
        source = path.read_text(encoding="utf-8", errors="replace")
        rel = path.relative_to(REPO_ROOT)

        for line, literal, reason in offending_literals(source):
            findings.append(f"  {rel}:{line}: {reason}: \"{literal}\"")

        for line, name, reason in offending_symbols(source, symbol_table, font_codepoints):
            findings.append(f"  {rel}:{line}: {reason}: {name}")

    if findings:
        print("FAILED: an LVGL display string will not render as intended")
        print("\n".join(findings))
        print("\nString literals under components/ui_graphics must be ASCII; any")
        print("other codepoint goes through an LV_SYMBOL_* macro. Every LV_SYMBOL_*")
        print("used must both exist and have a glyph in the compiled Montserrat")
        print("font, or it draws an empty box on the device and nothing warns you.")
        return 1

    print("LVGL glyph check: all display strings are ASCII, and every LV_SYMBOL_* "
          "used resolves to a glyph the compiled font actually carries ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
