# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
from pathlib import Path
import tempfile
import unittest
from partitions_fixtures import fixture, save, invalid, two_steps


class Types(unittest.TestCase):
    def test_partitions_must_be_an_object_including_explicit_null(self):
        for value in ([], 'x', None, True, 2):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                _, manifest = fixture(run)
                manifest['partitions'] = value
                save(run, manifest)
                invalid(self, run, 'partitions: ', 'expected object')

    def test_per_step_must_be_present_with_exact_case_and_an_array(self):
        cases = [{}, {'solver_fixed_per_run': True},
                 {'Per_step': [2], 'solver_fixed_per_run': True}]
        cases += [{'per_step': value, 'solver_fixed_per_run': True}
                  for value in ({}, None, '2', True, 2)]
        for partitions in cases:
            with self.subTest(partitions=partitions), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                _, manifest = fixture(run)
                manifest['partitions'] = partitions
                save(run, manifest)
                invalid(self, run, 'partitions.per_step: ', 'expected array')

    def test_per_step_length_must_equal_number_of_manifest_steps(self):
        for counts in ([], [2, 2]):
            with self.subTest(counts=counts), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                _, manifest = fixture(run)
                manifest['partitions'] = {'per_step': counts, 'solver_fixed_per_run': True}
                save(run, manifest)
                invalid(self, run, 'partitions.per_step: ', 'expected length 1')

    def test_per_step_elements_must_be_positive_nonboolean_integers(self):
        for value in (True, False, 2.0, '2', None, [], {}, 0, -1):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                _, manifest = fixture(run)
                manifest['partitions'] = {'per_step': [value], 'solver_fixed_per_run': True}
                save(run, manifest)
                invalid(self, run, 'partitions.per_step[0]: ', 'expected integer >= 1')

    def test_invalid_second_element_reports_its_array_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            _, manifest = two_steps(run)
            manifest['partitions'] = {'per_step': [2, False], 'solver_fixed_per_run': False}
            save(run, manifest)
            invalid(self, run, 'partitions.per_step[1]: ', 'expected integer >= 1')

    def test_fixed_flag_must_be_present_and_boolean(self):
        cases = [{'per_step': [2]}]
        cases += [{'per_step': [2], 'solver_fixed_per_run': value}
                  for value in (1, 'true', None, [], {})]
        for partitions in cases:
            with self.subTest(partitions=partitions), tempfile.TemporaryDirectory() as tmp:
                run = Path(tmp) / 'run'
                _, manifest = fixture(run)
                manifest['partitions'] = partitions
                save(run, manifest)
                invalid(self, run, 'partitions.solver_fixed_per_run: ', 'expected boolean')

    def test_fixed_run_requires_identical_counts_for_otherwise_valid_steps(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            _, manifest = two_steps(run)
            manifest['partitions'] = {'per_step': [2, 1], 'solver_fixed_per_run': True}
            save(run, manifest)
            invalid(self, run, 'partitions.per_step: ', 'expected identical values')


if __name__ == '__main__':
    unittest.main(argv=[__file__])
