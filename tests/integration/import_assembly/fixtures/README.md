The main test build automatically builds `sp4_step_fixtures` when OCCT is ON.
OCCT tests fail if the generator is missing. `SP4_FIXTURE_GENERATOR` may override
its path. Each test generates synthetic STEP files in its temporary directory;
no vendor geometry is stored here.

The C++ tests use private orchestration helpers `map_step_defects`,
`assembly_error_exit`, and `inline_assembly_outline` in the importer namespace.
The tests include the production implementation directly; missing helpers fail
compilation. No fallback replaces production behavior. The writer tests cover
external_refs hashes and missing sidecars; CLI tests cover split/rewrite reports.
