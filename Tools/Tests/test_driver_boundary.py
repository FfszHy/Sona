import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "check_driver_boundary", Path(__file__).resolve().parents[1] / "check_driver_boundary.py"
)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class DriverBoundaryTests(unittest.TestCase):
    def test_checks_both_architectures_and_deduplicates(self):
        output = """driver (for architecture arm64):
_AudioObjectGetPropertyData
_CFRelease
driver (for architecture x86_64):
_AudioDeviceStart
_AudioObjectGetPropertyData
_AudioStreamGetProperty
"""
        self.assertEqual(checker.forbidden_imports(output), [
            "AudioDeviceStart", "AudioObjectGetPropertyData", "AudioStreamGetProperty"
        ])

    def test_does_not_reject_plugin_types_or_host_interface(self):
        self.assertEqual(checker.forbidden_imports(
            "_CFRelease\n_AudioServerPlugInDriverInterface\n_mach_absolute_time\n"
        ), [])

    def test_cannot_pass_when_inspection_fails(self):
        with patch.object(checker.subprocess, "run", side_effect=OSError("missing nm")):
            self.assertEqual(checker.main(["check", "driver"]), 2)


if __name__ == "__main__":
    unittest.main()
