#!/usr/bin/env bash
# Regenerate the bundled flag font and its test fixture from Mozilla's
# Twemoji COLRv0 release (graphics CC-BY 4.0, font code Apache-2.0).
#
# data/fonts/TwemojiCountryFlags.ttf: the same subset talkjs's
# country-flag-emoji-polyfill ships (regional indicators, U+1F3F4 and the tag
# letters, so England/Scotland/Wales too), kept as TTF because DirectWrite
# does not load WOFF2, and renamed so it cannot collide with an installed
# Twemoji. tests/data/NoFlagColorEmoji.ttf: a colour face named like Windows'
# emoji font that has the regional-indicator letters but no flag ligatures.
# Requires fonttools (nix-shell -p python3Packages.fonttools).
set -euo pipefail
cd "$(dirname "$0")/.."
url=https://github.com/mozilla/twemoji-colr/releases/download/v0.7.0/Twemoji.Mozilla.ttf
sha256=6d90152ee0d29e82fe2a87793af5aa4b7ad13e6538360889e141e81ed299ee8e

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
curl -sfL -o "$work/twemoji.ttf" "$url"
echo "$sha256  $work/twemoji.ttf" | sha256sum -c -

pyftsubset "$work/twemoji.ttf" --drop-tables+=FFTM \
    --unicodes=U+1F1E6-1F1FF,U+1F3F4,U+E0061-E007A,U+E007F \
    --output-file="$work/flags.ttf"
pyftsubset "$work/twemoji.ttf" --drop-tables+=FFTM --layout-features='' \
    --unicodes=U+1F600,U+1F1E6-1F1FF,U+1F3F4 \
    --output-file="$work/noflags.ttf"

python3 -P - "$work" <<'EOF'
import sys
from fontTools.ttLib import TTFont
work = sys.argv[1]
for src, dst, family in (
        ("flags.ttf", "data/fonts/TwemojiCountryFlags.ttf", "Twemoji Country Flags"),
        ("noflags.ttf", "tests/data/NoFlagColorEmoji.ttf", "Segoe UI Emoji")):
    font = TTFont(f"{work}/{src}")
    name = font["name"]
    postscript = family.replace(" ", "")
    for record in list(name.names):
        if record.nameID in (1, 4):
            record.string = family
        elif record.nameID == 3:
            record.string = f"{postscript}-Regular"
        elif record.nameID == 6:
            record.string = postscript
    font.save(dst)
EOF
ls -la data/fonts/TwemojiCountryFlags.ttf tests/data/NoFlagColorEmoji.ttf
