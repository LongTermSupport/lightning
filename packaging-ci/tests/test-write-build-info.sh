#!/usr/bin/env bash
set -Eeuo pipefail

# write-build-info.sh against a stand-in .deb: its component inventory must
# name every ELF file the package installs and every Cargo.lock package, and
# be tied to the artifact's own hash. Needs dpkg-deb and jq.

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd -- "$ROOT/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail=0
note() { printf '  ok: %s\n' "$1"; }
bad() { printf '  FAIL: %s\n' "$1" >&2; fail=1; }

project() { # dir, then name=content-kind pairs for the payload
    local dir="$1"; shift
    mkdir -p "$dir/dist" "$dir/work/lightning/rust" "$dir/pkg/DEBIAN"
    cp "$REPO/flake.lock" "$dir/work/lightning/"
    cp "$REPO/rust/Cargo.lock" "$dir/work/lightning/rust/"
    cat >"$dir/dist/version.env" <<'EOF'
BASE_VERSION=0.11.0
LOGICAL_VERSION=0.11.0
DEB_VERSION=0.11.0
RPM_VERSION=0.11.0
RPM_RELEASE=1
SOURCE_SHA=0123456789abcdef0123456789abcdef01234567
SOURCE_REF=main
IS_EXACT_TAG=false
PUBLISHING=false
EOF
    printf '{"commit_time": "2026-11-22T00:00:00Z", "exact_tag": ""}\n' >"$dir/dist/source-info.json"
    printf 'Package: lightning\nVersion: 0.11.0\nArchitecture: amd64\nMaintainer: t <t@example.invalid>\nDepends: libc6 (>= 2.36), libqt6core6t64\nDescription: t\n' \
        >"$dir/pkg/DEBIAN/control"
    local entry path kind
    for entry in "$@"; do
        path="${entry%%=*}" kind="${entry#*=}"
        mkdir -p "$dir/pkg/$(dirname "$path")"
        if [[ "$kind" == elf ]]; then printf '\177ELF%s' "$path" >"$dir/pkg/$path"; else printf 'text\n' >"$dir/pkg/$path"; fi
    done
    dpkg-deb --root-owner-group -b "$dir/pkg" "$dir/dist/lightning_0.11.0_amd64.deb" >/dev/null
}
run() { # dir format
    env -u CI_PIPELINE_ID -u CI_JOB_ID CI_PROJECT_DIR="$1" "$ROOT/scripts/write-build-info.sh" "$2" >"$1/out.log" 2>&1
}

printf '== a .deb inventory ==\n'
P="$WORK/deb"
project "$P" usr/bin/lightning-matrix=elf usr/lib/x86_64-linux-gnu/libextra.so.1=elf usr/share/doc/lightning/README=text
if run "$P" deb; then note "write-build-info.sh deb succeeds"; else bad "deb run failed: $(tail -3 "$P/out.log")"; fi
J="$P/dist/build-info.json"
inv() { jq -c "$1" "$J" 2>/dev/null; }
[[ "$(inv '.component_inventory | length')" == 1 ]] && note "one inventory per .deb" || bad "inventory count"
[[ "$(inv '.component_inventory[0].sha256')" == "\"$(sha256sum "$P/dist/lightning_0.11.0_amd64.deb" | cut -d' ' -f1)\"" ]] \
    && note "the inventory names the artifact's own sha256" || bad "artifact hash"
[[ "$(inv '[.component_inventory[0].bundled_binaries[] | [.path, .component]]')" \
   == '[["/usr/bin/lightning-matrix","lightning"],["/usr/lib/x86_64-linux-gnu/libextra.so.1","unknown"]]' ]] \
    && note "every ELF file is listed; one not built here is 'unknown'" || bad "bundled binaries: $(inv '.component_inventory[0].bundled_binaries')"
[[ "$(inv '.component_inventory[0].bundled_binaries[0].sha256')" == "\"$(printf '\177ELF%s' usr/bin/lightning-matrix | sha256sum | cut -d' ' -f1)\"" ]] \
    && note "each binary carries its own sha256" || bad "binary hash"
[[ "$(inv '.component_inventory[0].system_dependencies')" == '["libc6 (>= 2.36)","libqt6core6t64"]' ]] \
    && note "the declared system dependencies are recorded" || bad "system dependencies"
[[ "$(inv '.component_inventory[0].rust_lock_packages | length')" == "$(grep -c '^\[\[package\]\]' "$REPO/rust/Cargo.lock")" ]] \
    && note "every Cargo.lock package is listed" || bad "Cargo.lock package count"
[[ "$(inv '[.component_inventory[0].rust_lock_packages[] | select(.name == "matrix-sdk-base")][0] | [.version, (.source | startswith("registry+")), (.checksum | length)]')" \
   == "[\"$(awk '/^name = "matrix-sdk-base"$/{getline; gsub(/"/, "", $3); print $3; exit}' "$REPO/rust/Cargo.lock")\",true,64]" ]] \
    && note "a crate carries its version, source and checksum" || bad "matrix-sdk-base entry"
! grep -qF "$WORK" "$J" && note "no builder path in build-info.json" || bad "a builder path leaked into build-info.json"

printf '== a .deb with no binary ==\n'
P="$WORK/empty"
project "$P" usr/share/doc/lightning/README=text
if run "$P" deb; then bad "a .deb without an ELF file was inventoried"; else
    grep -q "installs no ELF binary" "$P/out.log" && note "a .deb with nothing to inventory is refused" || bad "refused for the wrong reason: $(tail -1 "$P/out.log")"
fi

printf '== rpm is not inventoried yet ==\n'
P="$WORK/rpm"
project "$P" usr/bin/lightning-matrix=elf
rm "$P/dist/"*.deb; printf 'rpm\n' >"$P/dist/lightning-0.11.0-1.x86_64.rpm"
if run "$P" rpm; then
    [[ "$(jq -c '.component_inventory' "$P/dist/build-info.json")" == null ]] && note "an rpm records no inventory, explicitly" || bad "rpm inventory"
else bad "rpm run failed: $(tail -3 "$P/out.log")"; fi

if (( fail )); then
    printf 'write-build-info tests FAILED\n' >&2
    exit 1
fi
printf 'write-build-info tests passed\n'
