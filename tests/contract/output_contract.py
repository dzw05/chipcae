"""Shared producer/desktop results contract, read independently with h5py."""
import json


def validate_manifest(root, manifest):
    import jsonschema
    schema = json.loads((root / 'schema/manifest-1.schema.json').read_text())
    jsonschema.Draft202012Validator(schema).validate(manifest)
    assert len(manifest['partitions']['per_step']) == len(manifest['steps'])


def validate_output(root, directory, *, uncut=False):
    import h5py
    manifest = json.loads((directory / 'manifest.json').read_text())
    validate_manifest(root, manifest)
    assert {'warpage_pv', 'U_mag_max', 'T_max'} <= {s['name'] for s in manifest['summary']}
    fields = {f['name']: f for f in manifest['fields'] if not f.get('reserved', False)}
    assert len(fields) == sum(not f.get('reserved', False) for f in manifest['fields'])
    for name, unit, components in [('T', 'K', 1), ('U', 'm', 3)]:
        assert fields[name]['unit'] == unit
        assert fields[name]['location'] == 'point'
        assert fields[name]['components'] == components
    for step in manifest['steps']:
        with h5py.File(directory / step['file']) as file:
            vtk = file['VTKHDF']
            assert tuple(vtk.attrs['Version']) == (2, 8)
            assert vtk['hex/Points'].size > 0
            assert vtk['hex/Connectivity'].size > 0
            actual = set()
            for name in ('hex', 'cut', 'eb_surface', 'particles'):
                block = vtk[name]  # Empty blocks must exist, too.
                assert tuple(block.attrs['Version']) == (2, 8)
                if uncut and name != 'hex':
                    assert block['Points'].size == 0, name
                    assert not block['NumberOfPoints'][:].any(), name
                    if 'NumberOfCells' in block:
                        assert not block['NumberOfCells'][:].any(), name
                        assert block['Connectivity'].size == 0, name
                assert 'layer_names' in block['FieldData'], name
                assert 'material_names' in block['FieldData'], name
                for location, group in [('point', 'PointData'), ('cell', 'CellData')]:
                    for key, data in block[group].items():
                        assert key in fields, (name, group, key)
                        field = fields[key]
                        assert field['location'] == location
                        assert str(data.dtype) == field['dtype'], key
                        assert (data.shape[1] if data.ndim > 1 else 1) == field['components'], key
                        actual.add(key)
            assert actual == set(fields), (actual, set(fields))
    return manifest
