#!/usr/bin/env python3
"""Generate the JLCPCB assembly BOM and CPL from pager.kicad_pcb.

Codifies the previously ad-hoc production-file process:

- Iterate board footprints in file order.
- Skip refs starting with H, FID, or TP, and parts whose LCSC field is
  empty or "DNP".
- bom.csv: header "Comment,Designator,Footprint,LCSC Part #", one row per
  (value, footprint name, LCSC) group, designators comma-joined and sorted,
  groups ordered by LCSC part number (stable: first-encounter order breaks
  ties).
- positions.csv: header "Designator,Mid X,Mid Y,Layer,Rotation", sorted by
  designator, Y negated (KiCad to JLC), layer T, rotation offset by footprint
  name substring (SOT-23* / SOT-25 +180, D_SMA +180, SOD-123 +180,
  ESOP-8 +270), offsets applied mod 360.

Run with kicad's python:  python3 gen_production.py [board] [outdir]
"""
import csv
import os
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "pager.kicad_pcb")
OUTDIR = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "production")

SKIP_PREFIXES = ("H", "FID", "TP")

# (substring of footprint name, rotation offset KiCad -> JLC)
ROT_OFFSETS = (
    ("SOT-23", 180),
    ("SOT-25", 180),
    ("D_SMA", 180),
    ("SOD-123", 180),
    ("ESOP-8", 270),
)


def main():
    board = pcbnew.LoadBoard(BOARD)
    parts = []
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        if ref.startswith(SKIP_PREFIXES):
            continue
        lcsc = fp.GetFieldText("LCSC") if fp.HasField("LCSC") else ""
        if not lcsc or lcsc == "DNP":
            continue
        fpname = fp.GetFPIDAsString().split(":")[-1]
        pos = fp.GetPosition()
        rot = fp.GetOrientationDegrees()
        for sub, off in ROT_OFFSETS:
            if sub in fpname:
                rot = (rot + off) % 360
                break
        parts.append(dict(
            ref=ref, value=fp.GetValue(), fpname=fpname, lcsc=lcsc,
            x=pcbnew.ToMM(pos.x), y=pcbnew.ToMM(pos.y), rot=rot))

    # BOM: group by (value, footprint, LCSC), stable-sort groups by LCSC
    groups = {}
    for p in parts:
        groups.setdefault((p["value"], p["fpname"], p["lcsc"]), []).append(p["ref"])
    with open(os.path.join(OUTDIR, "bom.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
        for (value, fpname, lcsc), refs in sorted(groups.items(), key=lambda kv: kv[0][2]):
            w.writerow([value, ",".join(sorted(refs)), fpname, lcsc])

    # CPL: one row per part, sorted by designator, Y negated for JLC
    with open(os.path.join(OUTDIR, "positions.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for p in sorted(parts, key=lambda p: p["ref"]):
            w.writerow([p["ref"], f"{p['x']:.4f}", f"{-p['y']:.4f}", "T", f"{p['rot']:.1f}"])

    print(f"wrote {OUTDIR}/bom.csv ({len(groups)} lines) and "
          f"{OUTDIR}/positions.csv ({len(parts)} parts)", flush=True)


if __name__ == "__main__":
    main()
    os._exit(0)
