# Polyset sidecar format 0.1

A polyset is a binary, little-endian file. It stores integer coordinates in the database unit (`dbu_m` meters per integer unit). The magic is the eight ASCII bytes `CCPOLY01`.

The file is encoded in this order:

1. 8-byte magic.
2. `dbu_m`: IEEE-754 binary64.
3. Layer count: unsigned 32-bit integer.
4. For each layer, in order:
   - UTF-8 layer-name byte length: unsigned 32-bit integer, followed by that many bytes.
   - Ring count: unsigned 32-bit integer.
   - `ring_count + 1` signed 64-bit offsets. Offsets index coordinate pairs; the first is zero and the final offset is the number of points.
   - `2 * final_offset` signed 64-bit coordinates, alternating x and y.

Offsets delimit each ring as a half-open point range. Rings must contain at least three points. Exterior rings are counter-clockwise and hole rings are clockwise. Interpret rings with the NonZero fill rule. Layer names identify the copper layer referenced by `copper.polygons.layer`; `copper.polygons.file` is a path relative to the stack instance and must also appear in top-level `external_refs` with its SHA-256 digest.

Writers must emit finite positive `dbu_m`, monotonic offsets, and no trailing bytes. Readers reject malformed lengths, offsets, truncated arrays, invalid magic, and trailing data.
