#!/bin/sh
# Fetches the pinned GUI.Forms (and later the games render engine) into
# .build/deps/, the default CMAC_GUI_FORMS_SOURCE_DIR. Idempotent.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
pin() {
    sed -n "s/^set($1 \"\([0-9a-f]*\)\")$/\1/p" "$root/cmake/pins.cmake"
}
fetch() {
    name=$1 url=$2 revision=$3
    destination="$root/.build/deps/$name"
    if [ ! -d "$destination/.git" ]; then
        mkdir -p "$root/.build/deps"
        git clone --quiet --filter=blob:none --no-checkout "$url" "$destination"
    fi
    if ! git -C "$destination" cat-file -e "$revision^{commit}" 2>/dev/null; then
        git -C "$destination" fetch --quiet origin "$revision"
    fi
    git -C "$destination" checkout --quiet --detach "$revision"
    echo "$name at $(git -C "$destination" rev-parse HEAD)"
}
fetch gui_forms https://github.com/falseywinchnet/gui_forms.git "$(pin CMAC_GUI_FORMS_REVISION)"
