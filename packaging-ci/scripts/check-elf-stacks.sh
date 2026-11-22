#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
(( $# > 0 )) || { echo "usage: check-elf-stacks.sh <required ELF or payload directory>..." >&2; exit 2; }

check_stack() {
    if ! readelf -lW -- "$1" | awk '
        $1 == "GNU_STACK" {
            count++
            flags = ""
            for (i = 7; i < NF; i++) flags = flags $i
            if (flags !~ /^RW?$/ || $NF !~ /^0x[0-9a-fA-F]+$/) bad = 1
        }
        END { exit !(count == 1 && !bad) }
    '; then
        printf 'missing, executable or unreadable GNU_STACK: %s\n' "$1" >&2
        exit 1
    fi
}

for path in "$@"; do
    if [[ -d "$path" ]]; then
        find "$path" -type f -print0 | while IFS= read -r -d '' file; do
            [[ -r "$file" ]] || { printf 'unreadable payload file: %s\n' "$file" >&2; exit 1; }
            IFS= read -r -n 4 -d '' magic < "$file" || true
            if [[ "$magic" == $'\177ELF' ]]; then
                check_stack "$file"
            fi
        done
    else
        check_stack "$path"
    fi
done
