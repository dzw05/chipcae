# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Shared WP-2 artifact schema and producer/consumer validation."""

import copy
import json

CONTRACT = json.loads(r'''
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "WP-2 trilayer CalculiX REFERENCE artifact",
  "description": "WP-2 structural checks with exactly coarse and fine meshes and fine selection. Equality of top-level W_bfp_m and the selected mesh value is a validator-only producer check, checked by the validator below. Raw schema validation does not enforce equality between instance fields.",
  "type": "object",
  "additionalProperties": false,
  "required": [
    "kind",
    "label",
    "W_bfp_m",
    "element_type",
    "host",
    "calculix_version",
    "geometry",
    "meshes",
    "selected_mesh",
    "prototype_W_bfp_m",
    "abs_delta_m",
    "rel_delta"
  ],
  "properties": {
    "kind": { "const": "REFERENCE", "type": "string" },
    "label": { "type": "string", "minLength": 1 },
    "W_bfp_m": { "type": "number" },
    "element_type": { "type": "string", "const": "C3D20" },
    "host": { "type": "string", "minLength": 1 },
    "calculix_version": { "type": "string", "minLength": 1 },
    "geometry": {
      "type": "object",
      "additionalProperties": false,
      "required": ["L_m", "thicknesses_m", "E_Pa", "nu", "alpha_1_per_K", "dT_K"],
      "properties": {
        "L_m": { "type": "number" },
        "thicknesses_m": { "type": "array", "minItems": 3, "maxItems": 3, "items": { "type": "number" } },
        "E_Pa": { "type": "array", "minItems": 3, "maxItems": 3, "items": { "type": "number" } },
        "nu": { "type": "array", "minItems": 3, "maxItems": 3, "items": { "type": "number" } },
        "alpha_1_per_K": { "type": "array", "minItems": 3, "maxItems": 3, "items": { "type": "number" } },
        "dT_K": { "type": "number" }
      }
    },
    "meshes": {
      "type": "array",
      "minItems": 2,
      "maxItems": 2,
      "allOf": [
        {
          "contains": { "properties": { "name": { "const": "coarse" } }, "required": ["name"] },
          "minContains": 1,
          "maxContains": 1
        },
        {
          "contains": { "properties": { "name": { "const": "fine" } }, "required": ["name"] },
          "minContains": 1,
          "maxContains": 1
        }
      ],
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["name", "n_elem_xyz", "n_elem_z_per_layer", "W_bfp_m"],
        "properties": {
          "name": { "type": "string", "enum": ["coarse", "fine"] },
          "n_elem_xyz": { "type": "array", "minItems": 3, "maxItems": 3, "items": { "type": "integer" } },
          "n_elem_z_per_layer": { "type": "array", "minItems": 3, "maxItems": 3, "items": { "type": "integer" } },
          "W_bfp_m": { "type": "number" }
        }
      }
    },
    "selected_mesh": { "type": "string", "const": "fine" },
    "prototype_W_bfp_m": { "type": "number" },
    "abs_delta_m": { "type": "number" },
    "rel_delta": { "type": "number" },
    "provenance": {
      "type": "object",
      "description": "Optional, non-contractual generator metadata; arbitrary keys and values are permitted.",
      "additionalProperties": true
    }
  }
}
''')


def validate_reference(data, contract=CONTRACT):
    """Add the validator-only selected-value check with const/contains.

    JSON Schema cannot reference another instance field directly, so normalize
    the contract with the artifact's top-level value before validating it.
    """
    import jsonschema

    normalized = copy.deepcopy(contract)
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

