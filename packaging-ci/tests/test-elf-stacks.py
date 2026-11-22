#!/usr/bin/env python3
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
CHECK = SCRIPTS / "check-elf-stacks.sh"


def elf(flags, kind=0x6474E551):
    ident = b"\x7fELF" + bytes([2, 1, 1]) + bytes(9)
    header = struct.pack("<16sHHIQQQIHHHHHH", ident, 3, 62, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    return header + struct.pack("<IIQQQQQQ", kind, flags, 0, 0, 0, 0, 0, 16)


class StackGate(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.main = self.root / "lightning-matrix"
        self.main.write_bytes(elf(6))
        (self.root / "desktop file").write_text("text")

    def check(self, *paths, env=None):
        return subprocess.run(["bash", CHECK, *paths], text=True, capture_output=True, env=env)

    def test_shipped_elf_and_all_bundled_libraries(self):
        plugin = self.root / "nested plugin.so"
        plugin.write_bytes(elf(6))
        result = self.check(self.main, self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        plugin.write_bytes(elf(7))
        result = self.check(self.main, self.root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(plugin.name, result.stderr)

    def test_missing_or_executable_header(self):
        for data in (elf(7), elf(5), elf(6, kind=1), b"shell wrapper"):
            with self.subTest(data=data):
                self.main.write_bytes(data)
                result = self.check(self.main)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("GNU_STACK", result.stderr)

    def test_readelf_failure_after_printing_safe_header(self):
        tool = self.root / "readelf"
        tool.write_text("#!/bin/sh\necho 'GNU_STACK 0 0 0 0 0 RW 0x10'\nexit 3\n")
        tool.chmod(0o755)
        result = self.check(self.main, env={**os.environ, "PATH": str(self.root) + ":" + os.environ["PATH"]})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("GNU_STACK", result.stderr)

    def test_linux_validators_check_the_payload(self):
        for route, expected in {
            "validate-deb": ['"$binary" "$updater" "$audit_root"'],
            "validate-rpm": ['"$binary" "$updater" "$audit_root"'],
            "validate-appimage": ['"$app"', '"$tree/usr/bin/lightning-matrix" "$tree/usr/bin/lightning-updater" "$tree"'],
            "validate-flatpak": ['"$appdir/bin/lightning-matrix" "$appdir/bin/lightning-updater" "$appdir"'],
            "validate-snap": ['"$audit/prime/usr/bin/lightning-matrix" "$audit/prime/usr/bin/lightning-updater" "$audit/prime"'],
            "flathub-presubmission-lint": ['"$appdir/bin/lightning-matrix" "$appdir/bin/lightning-updater" "$appdir"'],
        }.items():
            with self.subTest(route=route):
                script = (SCRIPTS / (route + ".sh")).read_text()
                for args in expected:
                    self.assertIn('"$SCRIPT_DIR/check-elf-stacks.sh" ' + args, script)


if __name__ == "__main__":
    unittest.main(verbosity=2)
