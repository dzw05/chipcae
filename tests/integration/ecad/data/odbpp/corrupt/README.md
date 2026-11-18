Corrupt acceptance inputs are generated at test time by `../gen_archives.py` into
`<temporary-root>/corrupt/`. No generated binary is checked in.

Cases: truncated gzip, bad zip CRC, missing matrix/profile, parent traversal,
absolute/drive paths, bad gzip CRC/ISIZE, bad tar checksum, and an otherwise valid
job with an unknown feature record at `steps/pcb/layers/f.cu/features:3`.

The generated ratio-padded-before/after.tgz cases place incompressible data on
either side of a highly compressed entry; both must reject at the default 200:1
per-entry ratio limit.
