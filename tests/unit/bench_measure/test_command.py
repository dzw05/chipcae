"""Synthetic argv acceptance; does not invoke or replace system CTest."""
import sys
import unittest

from test_contract import Contract, SyntheticRunner, TOOL


class Command(Contract):
    def test_cli_file_exists(self):
        self.assertTrue(TOOL.is_file(), f"missing benchmark CLI: {TOOL}")

    def test_exact_ctest_command_and_inherited_output(self):
        for test in ("solve_mesh", "solve_pipeline"):
            with self.subTest(test=test):
                self.out.unlink(missing_ok=True)
                runner = SyntheticRunner()
                rc, _ = self.invoke(runner, extra=("--test", test))
                self.assertEqual(rc, 0)
                self.assertEqual(len(runner.calls), 2)
                discovery, execution = runner.calls
                for argv, kwargs in runner.calls:
                    self.assertEqual(argv[0], "ctest")
                    self.assertEqual(argv[argv.index("--test-dir") + 1], str(self.build))
                    self.assertFalse(kwargs.get("shell", False))
                self.assertIn("--show-only=json-v1", discovery[0])
                argv, kwargs = execution
                self.assertIn("-j1", argv)
                self.assertEqual(argv[argv.index("-R") + 1], f"^{test}$")
                self.assertFalse(kwargs.get("capture_output", False))
                self.assertIsNone(kwargs.get("stdout"))
                self.assertIsNone(kwargs.get("stderr"))
                self.assertEqual(runner.events, ["discovery", "clock", "run", "clock"])


if __name__ == "__main__":
    # Load only command tests; shared fixtures remain in test_contract.py.
    suite = unittest.TestSuite(Command(name) for name in (
        "test_cli_file_exists", "test_exact_ctest_command_and_inherited_output"))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
