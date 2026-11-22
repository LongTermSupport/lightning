#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=./scripts/lib.sh
source "$SCRIPT_DIR/lib.sh"
[[ $# -eq 1 ]] || die "usage: write-build-info.sh <deb|rpm>"
FORMAT="$1"
[[ "$FORMAT" =~ ^(deb|rpm)$ ]] || die "unsupported package format: $FORMAT"
ROOT="$(project_dir)"
load_versions
load_host_arch

case "$FORMAT" in
    deb) PACKAGE_VERSION="$DEB_VERSION"; PACKAGE_ARCHITECTURE="$DEB_ARCH" ;;
    rpm) PACKAGE_VERSION="${RPM_VERSION}-${RPM_RELEASE}"; PACKAGE_ARCHITECTURE="$RPM_ARCH" ;;
esac

mapfile -t artifacts < <(find "$ROOT/dist" -maxdepth 1 -type f \
    \( -name '*.deb' -o -name '*.rpm' \) -printf '%f\n' | sort)
(( ${#artifacts[@]} > 0 )) || die "no distributable artifact found"
FILES_JSON="$(printf '%s\n' "${artifacts[@]}" | jq -R . | jq -s .)"
CHECKSUMS_JSON="$(cd "$ROOT/dist" && for file in "${artifacts[@]}"; do
    sha256sum "$file" | jq -R 'split("  ") | {filename:.[1], sha256:.[0]}'
done | jq -s .)"

SOURCE_TIME="$(json_value "$ROOT/dist/source-info.json" commit_time)"
EXACT_TAG="$(json_value "$ROOT/dist/source-info.json" exact_tag)"
QT_VERSION="$(pkg-config --modversion Qt6Core 2>/dev/null || true)"
RUST_VERSION="$(rustc --version 2>/dev/null || true)"
CMAKE_VERSION="$(cmake --version 2>/dev/null | head -1 || true)"
BUILD_TIMESTAMP="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

# Component inventory, .deb only so far: every ELF file the package installs,
# mapped to a component ("unknown" until someone names it), and the Rust lock
# file, a superset of what is linked (it also lists other platforms' crates).
INV="$(mktemp -d)"
trap 'rm -rf "$INV"' EXIT
printf 'null\n' >"$INV/components.json"
deb_payload_json() {
    mkdir "$INV/x"
    dpkg-deb -x "$ROOT/dist/$1" "$INV/x"
    (cd "$INV/x" && find . -type f -print0 | sort -z | while IFS= read -r -d '' f; do
        [[ "$(head -c4 "$f" | od -An -tx1 | tr -d ' \n')" == 7f454c46 ]] || continue
        case "$f" in
            ./usr/bin/lightning-matrix|./usr/bin/lightning-updater) component=lightning ;;
            *) component=unknown ;;
        esac
        jq -n --arg path "${f#.}" --arg sha256 "$(sha256sum "$f" | cut -d' ' -f1)" \
            --arg component "$component" '{path:$path, sha256:$sha256, component:$component}'
    done) | jq -s .
    rm -rf "$INV/x"
}
if [[ "$FORMAT" == deb ]]; then
    CARGO_LOCK="$ROOT/work/lightning/rust/Cargo.lock"
    [[ -f "$CARGO_LOCK" ]] || die "no Cargo.lock at work/lightning/rust"
    awk '
        /^\[\[package\]\]/ { if (n != "") print n "\t" v "\t" s "\t" c; n = v = s = c = "" }
        /^name = /     { n = $3 }
        /^version = /  { v = $3 }
        /^source = /   { s = $3 }
        /^checksum = / { c = $3 }
        END { if (n != "") print n "\t" v "\t" s "\t" c }' "$CARGO_LOCK" | tr -d '"' |
        jq -R 'split("\t") | {name: .[0], version: .[1],
            source: (if .[2] == "" then null else .[2] end),
            checksum: (if .[3] == "" then null else .[3] end)}' | jq -s . >"$INV/crates.json"
    [[ "$(jq length "$INV/crates.json")" == "$(grep -c '^\[\[package\]\]' "$CARGO_LOCK")" ]] \
        || die "Cargo.lock inventory count differs from its [[package]] count"
    cargo_hash="$(sha256sum "$CARGO_LOCK" | cut -d' ' -f1)"
    flake_hash="$(sha256sum "$ROOT/work/lightning/flake.lock" | cut -d' ' -f1)"
    for file in "${artifacts[@]}"; do
        deb_payload_json "$file" >"$INV/binaries.json"
        [[ "$(jq length "$INV/binaries.json")" -gt 0 ]] || die "$file installs no ELF binary to inventory"
        jq -n --arg artifact "$file" --arg sha256 "$(cd "$ROOT/dist" && sha256sum "$file" | cut -d' ' -f1)" \
            --arg depends "$(dpkg-deb -f "$ROOT/dist/$file" Depends)" \
            --arg cargo_hash "$cargo_hash" --arg flake_hash "$flake_hash" \
            --slurpfile binaries "$INV/binaries.json" --slurpfile crates "$INV/crates.json" \
            '{artifact:$artifact, sha256:$sha256, bundled_binaries:$binaries[0],
              lock_sha256:{cargo:$cargo_hash, flake:$flake_hash},
              system_dependencies:($depends | split(", ") | map(select(. != ""))),
              rust_lock_packages:$crates[0]}'
    done | jq -s . >"$INV/components.json"
fi

# The repository recorded here is the PUBLIC home (GitHub). The pipeline's own
# clone URL and its CI job/project URLs are deliberately not written: they name
# a host that may be private, and nothing reads them back.
# PROVENANCE: lightning_repository names the PUBLIC mirror, not the clone the
# pipeline actually fetched from. What binds a package to its source is
# resolved_source_sha (a full commit SHA, identical on the mirror), not this URL.
jq -n \
    --arg project_name "lightning-deploy" \
    --arg pipeline_id "${CI_PIPELINE_ID:-local}" \
    --arg job_id "${CI_JOB_ID:-local}" \
    --arg package_format "$FORMAT" \
    --arg repository "${LIGHTNING_PUBLIC_REPOSITORY:-https://github.com/lightning-matrix/lightning.git}" \
    --arg requested_ref "$SOURCE_REF" \
    --arg source_sha "$SOURCE_SHA" \
    --arg source_commit_time "$SOURCE_TIME" \
    --arg source_tag "$EXACT_TAG" \
    --arg logical_version "$LOGICAL_VERSION" \
    --arg package_version "$PACKAGE_VERSION" \
    --arg architecture "$PACKAGE_ARCHITECTURE" \
    --arg build_type "${BUILD_TYPE:-Release}" \
    --arg runner_description "${CI_RUNNER_DESCRIPTION:-local}" \
    --arg runner_id "${CI_RUNNER_ID:-local}" \
    --arg runner_tags "${CI_RUNNER_TAGS:-local}" \
    --arg base_image "${BASE_IMAGE:-local}" \
    --arg qt_version "$QT_VERSION" \
    --arg cmake_version "$CMAKE_VERSION" \
    --arg rust_version "$RUST_VERSION" \
    --arg build_timestamp "$BUILD_TIMESTAMP" \
    --argjson files "$FILES_JSON" \
    --argjson checksums "$CHECKSUMS_JSON" \
    --slurpfile components "$INV/components.json" \
    '{project_name:$project_name,
      pipeline_id:$pipeline_id, job_id:$job_id,
      package_format:$package_format, lightning_repository:$repository,
      requested_lightning_ref:$requested_ref, resolved_source_sha:$source_sha,
      source_commit_time:$source_commit_time, source_tag:(if $source_tag == "" then null else $source_tag end),
      logical_version:$logical_version, package_version:$package_version,
      architecture:$architecture, build_type:$build_type,
      rust_sdk_backend:true, e2ee_enabled:true,
      runner:{description:$runner_description,id:$runner_id,tags:$runner_tags},
      base_image:$base_image,
      toolchain:{qt:$qt_version,cmake:$cmake_version,rust:$rust_version},
      build_timestamp_utc:$build_timestamp, artifacts:$files, checksums:$checksums,
      component_inventory:$components[0]}' \
    >"$ROOT/dist/build-info.json"
