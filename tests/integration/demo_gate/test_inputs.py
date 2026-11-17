"""Frozen DEMO-GATE inputs: schema validity and permitted template edits."""
import copy
import json
import math
from pathlib import Path
import sys

try:
    import jsonschema
except ImportError:
    print("SKIP: jsonschema is required")
    sys.exit(77)

root = Path(sys.argv[1]).resolve()
example = root / "examples/demo_gate"


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


polygon = [[.002, 0], [.018, 0], [.02, .002], [.02, .018],
           [.018, .02], [.002, .02], [0, .018], [0, .002]]
conductivity = {"organic_eq": 3.1, "sac305_ball_eq": 58,
                "underfill": .5, "silicon": 148}
schemas = {
    "case": "schema/case-1.0.schema.json",
    "stack": "schema/stack-0.2.schema.json",
    "profile": "schema/run-profile-1.0.schema.json",
    "materials": "material/schema/material-library.v1.schema.json",
}
try:
    from referencing import Registry, Resource
except ImportError:
    Registry = None

registry = Registry() if Registry is not None else None
schema_store = {}
for path in [*schemas.values(), "material/schema/material.v1.schema.json",
             "schema/parameters.schema.json"]:
    schema = read(root / path)
    if registry is None:
        # RefResolver uses urllib's URL joining, which cannot resolve fragment
        # references against the custom chipcae scheme. Map only schema URIs
        # to an HTTPS namespace; all documents still resolve from the local store.
        schema = json.loads(json.dumps(schema).replace("chipcae://", "https://chipcae.invalid/"))
    schema_store[schema["$id"]] = schema
    if registry is not None:
        registry = registry.with_resource(schema["$id"], Resource.from_contents(schema))
inputs = {}
for name, path in schemas.items():
    inputs[name] = read(example / (name + ".json"))
    schema = read(root / path)
    if registry is not None:
        resolution = {"registry": registry}
    else:
        schema = json.loads(json.dumps(schema).replace("chipcae://", "https://chipcae.invalid/"))
        # Deprecated RefResolver is only a fallback for older jsonschema installations.
        resolution = {"resolver": jsonschema.RefResolver.from_schema(schema, store=schema_store)}
    jsonschema.validators.validator_for(schema)(schema, **resolution).validate(inputs[name])

expected_stack = read(root / "schema/examples/fcbga_demo.stack.json")
expected_stack["id"] = "demo_gate"
expected_stack["outline"]["polygon"] = polygon
assert inputs["stack"] == expected_stack, "stack differs beyond the frozen edits"
actual_polygon = inputs["stack"]["outline"]["polygon"]
assert len(actual_polygon) == 8
for actual, expected in zip(actual_polygon, polygon):
    assert len(actual) == 2
    assert all(a == b for a, b in zip(actual, expected))
area = sum(a[0] * b[1] - b[0] * a[1]
           for a, b in zip(actual_polygon, actual_polygon[1:] + actual_polygon[:1])) / 2
assert math.isclose(area, 3.92e-4, rel_tol=1e-12, abs_tol=0), area

# Freeze the complete source text, including citations. Only conductivity and
# x-conductivity-source may be added to each template card.
sources = {
    "organic_eq": (
        "含过孔的高密度 FC-PBGA 有机基板，取等效厚度方向值，见 ECTC 2005 [doi:10.1109/ectc.2005.1442026](https://doi.org/10.1109/ectc.2005.1442026)。"
    ),
    "sac305_ball_eq": (
        "SAC305 体材料的导热系数，见 [TME/Cynel SAC305 TDS](https://www.tme.com/Document/be9dc599936f5899f2f2308663aa8b3c/SAC305_EN.pdf)、[Solder Connection 合金表](https://www.solderconnection.com/wp-content/uploads/2023/03/Solder-Alloy-Technical-Specifications.pdf)。"
    ),
    "underfill": (
        "硅微粉填充的环氧底填，取典型值，见 [CoolTherm ME-531 TDS](https://exdron.com/Exdron-Pdf/COOLTHERM-ME-531-SYR-30CC-EFD-DATASHEET.pdf)（0.5 W/mK）。"
    ),
    "silicon": (
        "单晶硅 300 K 下的导热系数，见 Glassbrenner & Slack, *Phys. Rev.* 134, A1058 (1964)。"
    ),
}
materials = copy.deepcopy(inputs["materials"])
for card in materials["materials"]:
    ident = card["id"]
    assert card.pop("conductivity") == conductivity[ident], ident
    source = card.pop("x-conductivity-source")
    assert source == sources[ident], (ident, source)
assert materials == read(root / "schema/examples/fcbga_demo.materials.json")

assert inputs["profile"] == {
    "schema": "chipcae.run_profile/1.0", "ranks": 1, "backend": "cpu", "gpus": 0}
assert inputs["case"] == {
    "schema": "chipcae.case/1.0",
    "stack": {"ref": "stack.json"}, "materials": {"ref": "materials.json"},
    "mesh": {"type": "aligned_cartesian_cut", "nx": 4, "ny": 4, "levels": 1,
             "max_ratio": 10, "cut": {"stabilization": "ghost_penalty",
                                     "quad_degree": 6, "gamma_g": .1}},
    "physics": [{"id": "thermal", "solver": "fem.conduction", "mode": "steady"},
                {"id": "mech", "solver": "fem.solid", "temp_from": "thermal",
                 "T_ref": 298.15, "nonlinear": {"geometric": False}}],
    "loads": [{"type": "fixed_temperature", "target": "face:zmin", "T": 298.15},
              {"type": "heat_source", "target": "layer:die_layer", "power": 10},
              {"type": "convection", "target": "face:zmax", "h": 10, "T_inf": 298.15}],
    "constraints": {"rigid": "3-2-1"},
    "solver": {"linear": {"preconditioner": "direct"}},
    "output": {"format": "vtkhdf",
               "warpage": {"standard": "JESD22-B112", "remove_rigid": "best_fit_plane"},
               "fields": ["T", "U", "layer_id", "material_id", "cell_kind", "elem_gid"]},
}
print("DEMO-GATE schemas and frozen inputs OK")
