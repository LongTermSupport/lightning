#!/usr/bin/env python3
"""Export the reviewed Rust bucket from a local Trivy v2 database using the bbolt CLI."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys


BUCKET = "cargo::GitHub Security Advisory Rust"


def export(path, expected):
    metadata = json.loads(path.with_name("metadata.json").read_bytes())
    with path.open("rb") as stream:
        checksum = hashlib.file_digest(stream, "sha256").hexdigest()
    if checksum != expected or metadata["Version"] != 2:
        raise ValueError("unexpected Trivy database digest or schema")

    def read(command, *keys):
        return subprocess.check_output(["bbolt", command, path, BUCKET, *keys], text=True)

    advisories = []
    for crate in sorted(read("keys").splitlines()):
        for advisory_id in sorted(read("keys", crate).splitlines()):
            record = json.loads(read("get", crate, advisory_id))
            upstream = next((key for key in [advisory_id, *record.get("VendorIDs", [])]
                             if key.startswith("GHSA-")), advisory_id)
            row = dict(id=advisory_id, component="cargo:" + crate, affected=record["VulnerableVersions"],
                       fixed=record.get("PatchedVersions", []), url="https://github.com/advisories/" + upstream)
            advisories.append(json.dumps(row, separators=(",", ":")))
    if not advisories:
        raise ValueError("empty Cargo advisory database")
    header = dict(schema=1, source="GitHub Advisory Database, reviewed Rust ecosystem via aquasecurity/trivy-db v2",
                  source_url="https://github.com/github/advisory-database", bucket=BUCKET,
                  database_sha256=checksum, updated=metadata["UpdatedAt"][:10])
    print(json.dumps(header, indent=2)[:-2] + ',\n  "advisories": [')
    print(",\n".join("    " + row for row in advisories))
    print("  ]\n}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path, help="local trivy.db alongside metadata.json")
    parser.add_argument("sha256", help="reviewed SHA-256 of the database bytes")
    args = parser.parse_args()
    try:
        export(args.database, args.sha256)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as exc:
        sys.exit(f"cannot export advisory snapshot: {exc}")
