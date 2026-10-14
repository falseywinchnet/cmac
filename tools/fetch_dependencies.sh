#!/bin/sh
# Fetches the pinned GUI.Forms and the games repository's render engine into
# .build/deps/ (the defaults of CMAC_GUI_FORMS_SOURCE_DIR and
# CMAC_GAMES_SOURCE_DIR). Idempotent. GUI.Forms' text and Skia dependencies
# are fetched by its own scripts (docs/BUILDING.md).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
pin() {
    sed -n "s/^set($1 \"\([0-9a-f]*\)\")$/\1/p" "$root/cmake/pins.cmake"
}
# fetch <name> <url> <revision> [sparse directory]
fetch() {
    name=$1 url=$2 revision=$3 sparse=${4:-}
    destination="$root/.build/deps/$name"
    if [ ! -d "$destination/.git" ]; then
        mkdir -p "$root/.build/deps"
        git clone --quiet --filter=blob:none --no-checkout "$url" "$destination"
        if [ -n "$sparse" ]; then
            git -C "$destination" sparse-checkout set --no-cone "/$sparse/"
        fi
    fi
    if ! git -C "$destination" cat-file -e "$revision^{commit}" 2>/dev/null; then
        git -C "$destination" fetch --quiet origin "$revision"
    fi
    git -C "$destination" checkout --quiet --detach "$revision"
    echo "$name at $(git -C "$destination" rev-parse HEAD)"
}
fetch gui_forms https://github.com/falseywinchnet/gui_forms.git "$(pin CMAC_GUI_FORMS_REVISION)"
# Only the render engine is needed from the games repository.
fetch games https://github.com/falseywinchnet/games.git "$(pin CMAC_GAMES_REVISION)" shared/render
