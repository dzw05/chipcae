import copy
import tempfile
import unittest

from assembly_test_support import Assembly, check, require, require_assembly_geometry

require('JSONCONS')
require('ECAD')
require_assembly_geometry()


def _invalid_assembly(root, case):
    a = Assembly(root)
    a.boxes()
    parts = a.document['parts']
    targets = {'missing_layer': 'layer:Nope@top',
               'missing_part': 'part:ghost@top',
               'cycle': 'part:second@top',
               'self_reference': 'part:lid@top'}
    if case == 'duplicate_id':
        parts.append(copy.deepcopy(parts[0]))
    else:
        parts[0]['placement'] = {'attach': targets[case], 'gap': 0}
        if case == 'cycle':
            second = copy.deepcopy(parts[0])
            second['id'] = 'second'
            second['placement'] = {'attach': 'part:lid@top', 'gap': 0}
            parts.append(second)
    a.save()
    return a


class AssemblyRejectMatrixTest(unittest.TestCase):
    def _assert_rejected(self, a, result, attach_defect=True):
        check(result, 7)
        self.assertEqual(a.defects()['code'], 'IMPORT_REJECTED')
        self.assertFalse(a.output.exists())
        if attach_defect:
            projections = [{key: defect[key] for key in ('defect_id', 'tier', 'reason')}
                           for defect in a.import_report()['defects']]
            self.assertIn({'defect_id': 'lid/attach_invalid', 'tier': 'C', 'reason': 7},
                          projections)

    def test_reject_matrix(self):
        messages = {
            'missing_layer': 'IMPORT_REJECTED: missing attach target layer:Nope',
            'missing_part': 'IMPORT_REJECTED: missing attach target part:ghost',
            'cycle': 'IMPORT_REJECTED: cyclic part attachments',
            'self_reference': 'IMPORT_REJECTED: cyclic part attachments',
            'duplicate_id': 'IMPORT_REJECTED: duplicate part id lid',
        }
        for case, message in messages.items():
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                a = _invalid_assembly(tmp, case)
                self._assert_rejected(a, a.run('--accept-b-tier'),
                                      attach_defect=case != 'duplicate_id')
                self.assertEqual(a.defects()['message'], message)

    def test_c_tier_cannot_be_accepted(self):
        for case in ('missing_part', 'cycle'):
            for mode in ('explicit', 'default', 'all_b'):
                with self.subTest(case=case, mode=mode), tempfile.TemporaryDirectory() as tmp:
                    a = _invalid_assembly(tmp, case)
                    if mode == 'explicit':
                        ids = [part['id'] + '/attach_invalid' for part in a.document['parts']]
                        result = a.run('--accept', a.accept(ids))
                    elif mode == 'default':
                        a.accept(['lid/attach_invalid']).rename(a.root / 'import.accept.json')
                        result = a.run()
                    else:
                        result = a.run('--accept-b-tier')
                    self._assert_rejected(a, result)


if __name__ == '__main__':
    unittest.main(argv=[__file__], verbosity=2)
