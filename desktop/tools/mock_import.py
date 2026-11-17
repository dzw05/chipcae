#!/usr/bin/env python3
"""Mock chipcae-import. Test-only input contents select a mode (success by default).

Modes: success, b, c, missing, corrupt, invalid, hang. STEP/XML suffixes are
passed through; this fixture does not parse ECAD. No confirmation CLI is defined.
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
    args = parser.parse_args()
    mode = pathlib.Path(args.input).read_text().strip() or "success"
    print("mock import: " + mode, file=sys.stderr, flush=True)
    print("staged-output: " + args.o, file=sys.stderr, flush=True)
    pathlib.Path(args.o).write_text(json.dumps({} if mode == "invalid" else STACK))
    if args.polyset:
        pathlib.Path(args.polyset).write_text("mock polyset fixture\n")
    if mode == "hang":
        while True:
            time.sleep(1)
    grade = "B" if mode == "b" else "C" if mode == "c" else "A"
    report = {"grade": grade, "defects": []}
    if grade != "A":
        report["defects"] = [{"code": "IMPORT_REJECTED" if grade == "C" else "MOCK_GAP",
                              "reason": "MULTI_MATERIAL_CUT_UNSUPPORTED" if grade == "C"
                              else "Review fixture gap"}]
    if args.report and mode != "missing":
        pathlib.Path(args.report).write_text("{" if mode == "corrupt" else json.dumps(report))
    return 2 if grade == "B" else 3 if grade == "C" else 0


if __name__ == "__main__":
    sys.exit(main())
