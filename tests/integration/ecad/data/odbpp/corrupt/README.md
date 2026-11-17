Corrupt acceptance inputs are generated at test time by `../gen_archives.py` into
`<temporary-root>/corrupt/`. No generated binary is checked in.

Cases: truncated gzip, bad zip CRC, missing matrix/profile, parent traversal,
absolute/drive paths, bad gzip CRC/ISIZE, bad tar checksum, and an otherwise valid
job with an unknown feature record at `steps/pcb/layers/f.cu/features:3`.
