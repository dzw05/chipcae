These minimal fixtures are synthetic and contain no real export data. Values and
layer order are defined in gen_kicad_like.py. Regenerate from the repository root:

```sh
python3 tests/integration/ecad/data/kicad_like/gen_kicad_like.py
```

Tests read the committed XML directly and do not require Python or KiCad.
The late fixture deliberately puts Stackup before Layer and Spec definitions.

`pcb/` contains independently authored minimal two/four copper layer boards for
optional manual KiCad comparisons. Run `pcb/regen.sh` with `kicad-cli` installed
to export them locally. Those exports are not automated fixtures and must not be
committed; the script-defined XML remains the acceptance input.
