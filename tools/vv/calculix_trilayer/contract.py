# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Shared WP-2 artifact schema and cross-field validation."""
import copy
import json
from pathlib import Path

ARTIFACT_VERSION = 1

CONTRACT = json.loads(Path(__file__).with_name("reference.schema.json").read_text(encoding="utf-8"))


def validate_reference(data, contract=CONTRACT):
    """Add the validator-only selected-value check with const/contains.

    JSON Schema cannot reference another instance field directly, so normalize
    the contract with the artifact's top-level value before validating it.
    """
    import jsonschema

    normalized = copy.deepcopy(contract)
    normalized["properties"]["artifact_version"] = {
        "type": "integer", "const": ARTIFACT_VERSION,
    }
    normalized["properties"]["meshes"]["allOf"].append({
        "contains": {
            "properties": {
                "name": {"const": data.get("selected_mesh")},
                "W_bfp_m": {"const": data.get("W_bfp_m")},
            },
            "required": ["name", "W_bfp_m"],
        },
        "minContains": 1,
        "maxContains": 1,
    })
    jsonschema.Draft202012Validator(normalized).validate(data)

