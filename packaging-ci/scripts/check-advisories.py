#!/usr/bin/env python3
"""Offline advisory triage for the source locks and, with --build-info, the actual .deb payload."""
import argparse
from collections import Counter, defaultdict
from datetime import date, datetime, timezone
import hashlib
import json
import operator
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tomllib

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "packaging-ci/advisories"
REGISTRY = "registry+https://github.com/rust-lang/crates.io-index"
OPS = {"=": operator.eq, ">=": operator.ge, "<=": operator.le, ">": operator.gt, "<": operator.lt}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def semver(value):
    match = re.fullmatch(r"(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?", value)
    require(match, "unsupported version in Cargo advisory comparison")
    major, minor, patch, pre = match.groups()
    identifiers = tuple((0, int(i)) if i.isdigit() else (1, i) for i in (pre or "").split("."))
    return int(major), int(minor or 0), int(patch or 0), pre is None, identifiers


def cargo_range(expression):
    bounds = []
    for clause in expression.split(","):
        match = re.fullmatch(r"(>=|<=|=|>|<)\s*(\S+)", clause.strip())
        require(match, "unsupported Cargo advisory range")
        op, bound = match.groups()
        bounds.append((OPS[op], semver(bound)))
    return bounds


def rows(items, fields):
    return Counter(tuple(row.get(k) for k in fields) for row in items)


def package_payload(path):
    binaries = {}
    with subprocess.Popen(["dpkg-deb", "--fsys-tarfile", path], stdout=subprocess.PIPE) as process:
        with process.stdout, tarfile.open(fileobj=process.stdout, mode="r|") as archive:
            for member in archive:
                if not member.isfile():
                    continue
                stream = archive.extractfile(member)
                magic = stream.read(4)
                if magic != b"\x7fELF":
                    continue
                checksum = hashlib.sha256(magic)
                while chunk := stream.read(1024 * 1024):
                    checksum.update(chunk)
                name = "/" + member.name.removeprefix("./")
                require(name not in binaries, "duplicate ELF path in deb")
                binaries[name] = checksum.hexdigest()
        require(process.wait() == 0, "cannot inspect deb payload")
    return binaries


def inspect_artifacts(info, directory, packages, locks):
    require(re.fullmatch("[0-9a-f]{40}", info["resolved_source_sha"]), "invalid source SHA")
    inventories = info["component_inventory"]
    require(info["package_format"] == "deb" and inventories, "no deb component inventory")
    names = [entry["artifact"] for entry in inventories]
    require(len(set(names)) == len(names) and sorted(names) == sorted(info["artifacts"]),
            "artifact list differs from inventory")
    require(rows(info["checksums"], ("filename", "sha256")) ==
            Counter((e["artifact"], e["sha256"]) for e in inventories), "artifact checksums differ")
    fields = ("name", "version", "source", "checksum")
    for entry in inventories:
        name = entry["artifact"]
        require(re.fullmatch(r"[\w.+~-]+\.deb", name), "invalid deb filename")
        path = directory / name
        require(digest(path) == entry["sha256"], "deb SHA-256 differs from inventory")
        require(entry.get("lock_sha256") == locks, "source lock hashes differ or are missing")
        require(rows(entry["rust_lock_packages"], fields) == rows(packages, fields),
                "Cargo inventory differs from lock")
        binaries = entry["bundled_binaries"]
        require(binaries and rows(binaries, ("path", "sha256")) ==
                Counter(package_payload(path).items()), "bundled ELF inventory differs from deb")
        yield entry


def decisions_from(triage):
    decisions = {}
    for row in triage["triage"]:
        key = row["id"], row["component"], row["version"]
        require(all(isinstance(v, str) and v.strip() for v in (*key, row["reason"])),
                "empty triage identity or reason")
        require(row["decision"] in {"affected", "not_affected", "waived", "unknown"},
                "invalid triage decision")
        require(type(row["expiry"]) is date and key not in decisions, "invalid expiry or duplicate triage")
        decisions[key] = dict(row, expiry=row["expiry"].isoformat())
    return decisions


def scan(args):
    today = args.as_of
    snapshot = json.loads(args.snapshot.read_bytes())
    require(snapshot["schema"] == 1 and snapshot["advisories"], "empty or unsupported snapshot")
    updated = date.fromisoformat(snapshot["updated"])
    locks = {"cargo": digest(args.cargo_lock), "flake": digest(args.flake_lock)}
    report = dict(tool="lightning-advisory-gate/1", python=sys.version.split()[0], as_of=today.isoformat(),
                  source=snapshot["source"], database_date=updated.isoformat(),
                  snapshot_sha256=digest(args.snapshot), triage_sha256=digest(args.triage),
                  lock_sha256=locks, findings=[], errors=[])
    if not 0 <= (today - updated).days <= 30:
        report["errors"].append("advisory snapshot is future-dated or older than 30 days")

    packages = tomllib.loads(args.cargo_lock.read_text())["package"]
    require(packages, "empty Cargo.lock")
    components = defaultdict(set)
    for package in packages:
        key = "cargo:" + package["name"]
        version = package["version"]
        semver(version)
        source = package.get("source")
        if source == REGISTRY:
            require(re.fullmatch("[0-9a-f]{64}", package["checksum"]), "invalid crate checksum")
        components[key].add((version, source == REGISTRY))
    report["rust_lock_packages"] = len(packages)
    flake = json.loads(args.flake_lock.read_bytes())
    require(flake["version"] == 7 and flake["root"] in flake["nodes"], "unsupported flake.lock")
    report["flake_inputs"] = {}
    for node, data in flake["nodes"].items():
        if node == flake["root"]:
            continue
        pin = data["locked"]
        require(pin["type"] == "github" and re.fullmatch("[0-9a-f]{40}", pin["rev"])
                and re.fullmatch(r"sha256-[a-zA-Z0-9+/]{43}=", pin["narHash"]), "unsupported flake pin")
        component = "flake:" + pin["owner"] + "/" + pin["repo"]
        components[component].add((pin["rev"], True))
        report["flake_inputs"][node] = dict(component=component, revision=pin["rev"], narHash=pin["narHash"])

    scopes = {}
    if args.build_info:
        info = json.loads(args.build_info.read_bytes())
        report["resolved_source_sha"] = info["resolved_source_sha"]
        report["artifacts"] = {}
        for item in inspect_artifacts(info, args.build_info.parent, packages, locks):
            scope = {key: set(values) for key, values in components.items()}
            name = item["artifact"]
            scopes[name] = scope
            for binary in item["bundled_binaries"]:
                component = binary["component"]
                if component == "unknown":
                    report["errors"].append(f"{name}: unidentified ELF {binary['path']}")
                scope.setdefault("binary:" + component, set()).add((binary["sha256"], True))
            for group in item["system_dependencies"]:
                for alternative in group.split("|"):
                    match = re.match(r"^([a-z0-9][a-z0-9+.-]*)\b", alternative.strip())
                    require(match, "invalid system dependency")
                    scope.setdefault("deb:" + match[1], set()).add(("unresolved", False))
            report["artifacts"][name] = {k: item[k] for k in ("sha256", "bundled_binaries", "system_dependencies")}
    else:
        scopes["locks"] = components

    decisions = decisions_from(tomllib.loads(args.triage.read_text()))
    seen = set()
    for adv in snapshot["advisories"]:
        key = adv["component"]
        kind = key.split(":")[0]
        require(kind in {"cargo", "flake", "binary", "deb"} and ":" in key, "invalid advisory component")
        identity = adv["id"], key
        require(identity not in seen and adv["affected"], "duplicate or empty advisory")
        seen.add(identity)
        ranges = [cargo_range(r) for r in adv["affected"]] if kind == "cargo" else []
        for artifact, scope in scopes.items():
            for version, known in sorted(scope.get(key, ())):
                if known and kind == "cargo":
                    actual = semver(version)
                    if not any(all(compare(actual, bound) for compare, bound in bounds) for bounds in ranges):
                        continue
                elif known and version not in adv["affected"]:
                    continue
                review = decisions.get((*identity, version))
                status = "affected" if known else "unknown"
                result = dict(artifact=artifact, component=key, version=version, id=adv["id"], match=status,
                              fixed=adv["fixed"], url=adv["url"], decision=status)
                if review:
                    result.update(decision=review["decision"], reason=review["reason"], expiry=review["expiry"])
                    if today.isoformat() >= review["expiry"]:
                        result["decision"] = "expired"
                report["findings"].append(result)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cargo-lock", type=Path, default=ROOT / "rust/Cargo.lock")
    parser.add_argument("--flake-lock", type=Path, default=ROOT / "flake.lock")
    parser.add_argument("--build-info", type=Path, help="build-info.json alongside its inventoried .deb files")
    parser.add_argument("--snapshot", type=Path, default=DATA / "snapshot.json")
    parser.add_argument("--triage", type=Path, default=DATA / "triage.toml")
    parser.add_argument("--as-of", type=date.fromisoformat, default=datetime.now(timezone.utc).date(),
                        help="UTC review date for replay (default: today)")
    try:
        report = scan(parser.parse_args())
    except (OSError, ValueError, KeyError, TypeError, tarfile.TarError) as exc:
        print(f"invalid advisory gate input: {exc}", file=sys.stderr)
        return 2
    print(json.dumps(report, indent=2, sort_keys=True))
    return int(bool(report["errors"]) or any(f["decision"] not in {"waived", "not_affected"}
                                             for f in report["findings"]))


if __name__ == "__main__":
    sys.exit(main())
