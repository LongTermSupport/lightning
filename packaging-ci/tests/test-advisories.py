#!/usr/bin/env python3
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "packaging-ci/scripts/check-advisories.py"
REGISTRY = "registry+https://github.com/rust-lang/crates.io-index"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def advisory(component, affected):
    return dict(id="TEST-1", component=component, affected=affected, fixed=[], url="https://example.invalid/1")


class AdvisoryGate(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.cargo = self.root / "Cargo.lock"
        self.cargo.write_text(f'[[package]]\nname = "demo"\nversion = "1.10.0-rc.12+local"\n'
                              f'source = "{REGISTRY}"\nchecksum = "{"a" * 64}"\n')
        self.flake = self.root / "flake.lock"
        pin = dict(type="github", owner="owner", repo="lib", rev="b" * 40, narHash="sha256-" + "A" * 43 + "=")
        self.flake.write_text(json.dumps(dict(version=7, root="root", nodes={"root": {}, "lib": {"locked": pin}})))
        self.snapshot = dict(schema=1, source="fixture", updated="2026-11-19",
                             advisories=[advisory("cargo:demo", [">=1.10.0-rc.9, <1.10.0"])])
        self.triage = self.root / "triage.toml"
        self.triage.write_text("triage = []")
        self.elf = b"\x7fELFstand-in"
        self.package = self.root / "lightning.deb"
        with tarfile.open(self.package, "w") as tar:
            for name, data in [("./usr/bin/lightning-matrix", self.elf), ("./usr/share/readme", b"text")]:
                member = tarfile.TarInfo(name)
                member.size = len(data)
                tar.addfile(member, io.BytesIO(data))
        dpkg = self.root / "dpkg-deb"
        dpkg.write_text('#!/bin/sh\n[ "$1" = "--fsys-tarfile" ] && exec cat "$2"\n')
        dpkg.chmod(0o755)
        self.entry = dict(artifact=self.package.name, sha256=sha(self.package.read_bytes()),
                          lock_sha256=dict(cargo=sha(self.cargo.read_bytes()), flake=sha(self.flake.read_bytes())),
                          bundled_binaries=[dict(component="lightning", path="/usr/bin/lightning-matrix",
                                                 sha256=sha(self.elf))],
                          system_dependencies=["libqt6core6t64 (>= 6.8)"],
                          rust_lock_packages=[dict(name="demo", version="1.10.0-rc.12+local", source=REGISTRY,
                                                   checksum="a" * 64)])
        self.info = dict(package_format="deb", resolved_source_sha="c" * 40, artifacts=[self.package.name],
                         checksums=[dict(filename=self.package.name, sha256=self.entry["sha256"])],
                         component_inventory=[self.entry])

    def review(self, decision, expiry="2026-11-23", version="1.10.0-rc.12+local"):
        self.triage.write_text(f'[[triage]]\nid="TEST-1"\ncomponent="cargo:demo"\nversion="{version}"\n'
                               f'decision="{decision}"\nreason="documented fixture evidence"\nexpiry={expiry}\n')

    def gate(self, expected, date="2026-11-22", error=None):
        snapshot = self.root / "snapshot.json"
        snapshot.write_text(json.dumps(self.snapshot))
        inventory = self.root / "build-info.json"
        inventory.write_text(json.dumps(self.info))
        run = subprocess.run([sys.executable, "-I", SCRIPT, "--snapshot", snapshot, "--triage", self.triage,
                              "--cargo-lock", self.cargo, "--flake-lock", self.flake, "--build-info", inventory,
                              "--as-of", date], capture_output=True, text=True,
                             env={**os.environ, "PATH": str(self.root) + os.pathsep + os.environ["PATH"]})
        self.assertEqual(run.returncode, expected, run.stderr or run.stdout)
        if error:
            self.assertIn(error, run.stderr)
            return
        report = json.loads(run.stdout)
        self.assertNotIn(str(self.root), run.stdout)
        return report

    def test_unhandled_range_and_fixed_version(self):
        self.snapshot["advisories"][0]["fixed"] = ["1.10.0"]
        result, = self.gate(1)["findings"]
        self.assertEqual((result["decision"], result["artifact"], result["fixed"]),
                         ("affected", self.package.name, ["1.10.0"]))
        self.snapshot["advisories"][0]["affected"] = ["<1.9.0", ">1.10.0-rc.12"]
        self.assertEqual(self.gate(0)["findings"], [])

    def test_review_evidence_and_exclusive_expiry(self):
        for decision in ("not_affected", "waived", "affected", "unknown"):
            with self.subTest(decision=decision):
                self.review(decision)
                finding, = self.gate(int(decision in {"affected", "unknown"}))["findings"]
                self.assertEqual((finding["decision"], finding["match"], finding["reason"]),
                                 (decision, "affected", "documented fixture evidence"))
                self.assertEqual(self.gate(1, date="2026-11-23")["findings"][0]["decision"], "expired")
        self.review("waived", version="1.10.0")
        self.assertEqual(self.gate(1)["findings"][0]["decision"], "affected")

    def test_flake_binary_and_unresolved_system_dependency(self):
        self.snapshot["advisories"] = [advisory("flake:owner/lib", ["b" * 40]),
                                       advisory("binary:lightning", [sha(self.elf)]),
                                       advisory("deb:libqt6core6t64", ["6.8.1"])]
        report = self.gate(1)
        self.assertEqual([f["match"] for f in report["findings"]], ["affected", "affected", "unknown"])
        self.assertEqual(report["lock_sha256"], self.entry["lock_sha256"])
        self.assertEqual(report["resolved_source_sha"], self.info["resolved_source_sha"])
        self.assertEqual(report["artifacts"][self.package.name]["sha256"], self.entry["sha256"])
        self.entry["bundled_binaries"][0]["component"] = "unknown"
        self.assertIn("unidentified ELF", self.gate(1)["errors"][0])

    def test_expired_snapshot(self):
        self.review("waived", expiry="2027-01-01")
        self.assertIn("older than 30 days", self.gate(1, date="2026-12-20")["errors"][0])

    def test_reconcile_all_payload_and_lock_evidence(self):
        self.entry["rust_lock_packages"][0]["checksum"] = "d" * 64
        self.gate(2, error="Cargo inventory differs")
        self.entry["rust_lock_packages"][0]["checksum"] = "a" * 64
        self.flake.write_text(self.flake.read_text() + "\n")
        self.gate(2, error="source lock hashes differ")
        self.entry["lock_sha256"]["flake"] = sha(self.flake.read_bytes())
        self.entry["bundled_binaries"].clear()
        self.gate(2, error="bundled ELF inventory differs")
        self.package.write_bytes(b"replaced")
        self.gate(2, error="deb SHA-256 differs")
        self.info["component_inventory"] = None
        self.gate(2, error="no deb component inventory")

    def test_incomplete_or_duplicate_review(self):
        self.review("waived")
        row = self.triage.read_text()
        self.triage.write_text(row + row)
        self.gate(2, error="duplicate triage")
        self.triage.write_text(row.replace("documented fixture evidence", " "))
        self.gate(2, error="empty triage")


if __name__ == "__main__":
    unittest.main(verbosity=2)
