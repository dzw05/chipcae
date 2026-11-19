"""Rounded hat and rotated holed ring are v1.0 Tier C import limitations."""
from parts_support import *


def acceptance(d):
    prepare(d)
    rounded_parts(d)
    import yaml
    spec = yaml.safe_load((ROOT / 'protocol/errors.yaml').read_text())
    code = next(e['code'] for e in spec['errors'] if e['name'] == 'IMPORT_REJECTED')
    for ident, reason in (('hat', 'multiple distinct cut sections'),
                          ('ring', 'holed non-rectilinear section')):
        # Separate invocations prove each part is rejected for its own limitation.
        dump(d / 'assembly.json', assembly([part(ident, 'underfill')]))
        for name in ('report.json', 'import_report.json', 'stack.json'):
            (d / name).unlink(missing_ok=True)
        run('import', ['--assembly', d / 'assembly.json', '-o', d / 'stack.json',
                       '--report', d / 'report.json', '--accept-b-tier'], expected=code)
        report = json.loads((d / 'report.json').read_text())
        assert report['code'] == 'IMPORT_REJECTED', report
        report = json.loads((d / 'import_report.json').read_text())
        defects = [v for v in report['defects']
                   if v['defect_id'].startswith(ident + '/cut_unsupported#')]
        assert defects, report
        for defect in defects:
            assert defect['tier'] == 'C', defect
            # Frozen DefectReason ordinal: assembly v1.0.2 continuation 7-7.
            assert defect['reason'] == 3, defect  # PART_NOT_PRISMATIC
            assert defect['details'].startswith('cut_unsupported: ' + reason), defect
        assert not (d / 'stack.json').exists()


if __name__ == '__main__':
    main(acceptance, dependency='SP3/SP4')
