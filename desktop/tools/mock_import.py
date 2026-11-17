#!/usr/bin/env python3
"""Mock chipcae-import. Test-only input contents select a mode (success by default).

The first input line selects protocol outcomes; this fixture does not parse ECAD.
"""
import argparse
import json
import pathlib
import sys
import time

STACK = json.loads(r'''
{
  "id": "mock_import_result",
  "unit": "m",
  "outline": {
    "box": [
      0,
      0,
      0.02,
      0.02
    ]
  },
  "symmetry": "none",
  "package_type": "FCBGA",
  "layers": [
    {
      "name": "substrate",
      "thickness": 0.001,
      "kind": "Homogenized",
      "material": "organic_eq",
      "n_elem_min": 2,
      "grading": 1
    },
    {
      "name": "bump_eq",
      "thickness": 0.0001,
      "kind": "Homogenized",
      "material": "sac305_ball_eq",
      "n_elem_min": 2,
      "grading": 1
    },
    {
      "name": "die_layer",
      "thickness": 0.0005,
      "kind": "Explicit",
      "material": "underfill",
      "n_elem_min": 2,
      "grading": 1,
      "regions": [
        {
          "name": "die",
          "material": "silicon",
          "footprint": {
            "box": [
              0.005,
              0.005,
              0.015,
              0.015
            ]
          }
        }
      ]
    }
  ],
  "activation_groups": [
    {
      "name": "assembly",
      "members": [
        "layer:substrate",
        "body:die"
      ]
    }
  ]
}
''')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("-o", required=True)
    parser.add_argument("--polyset")
    parser.add_argument("--report")
    parser.add_argument("--material-map")
    parser.add_argument("--accept-b-tier", action="store_true")
    args = parser.parse_args()
    mode = (pathlib.Path(args.input).read_text().strip().splitlines() or ["success"])[0]
    print("mock import: " + mode, file=sys.stderr, flush=True)
    print("staged-output: " + args.o, file=sys.stderr, flush=True)
    pathlib.Path(args.o).write_text(json.dumps({} if mode == "invalid" else STACK))
    if args.polyset:
        pathlib.Path(args.polyset).write_text("mock polyset fixture\n")
    if mode == "hang":
        while True:
            time.sleep(1)
    code, exit_code = {
        "confirm": ("IMPORT_CONFIRMATION_REQUIRED", 6),
        "b": ("IMPORT_CONFIRMATION_REQUIRED", 6),
        "reject": ("IMPORT_REJECTED", 7),
        "c": ("IMPORT_REJECTED", 7),
        "map-missing": ("MATERIAL_MAPPING_MISSING", 9),
        "code-mismatch": ("IMPORT_REJECTED", 0),
        "invalid": ("IMPORT_REJECTED", 7),
        "missing": ("OK", 1),
        "corrupt": ("OK", 1),
    }.get(mode, ("OK", 0))
    # --accept-b-tier is accepted for argv compatibility; confirm/b modes always
    # request confirmation. Accepted reruns use the independent controller fixture.
    report = {"code": code, "message": "Mock import: " + mode, "defects": []}
    if code in ("IMPORT_CONFIRMATION_REQUIRED", "IMPORT_REJECTED"):
        report["defects"] = [{"code": "MOCK_GAP", "reason": "Review fixture gap"}]
    report_path = (pathlib.Path(args.report) if args.report
                   else pathlib.Path(args.o).parent / "defects.json")
    if mode != "missing":
        report_path.write_text("{" if mode == "corrupt" else json.dumps(report))
    return exit_code



if __name__ == "__main__":
    sys.exit(main())
