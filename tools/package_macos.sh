#!/bin/sh
# Stages a self-contained cmac.app: the build's bundle with GUI.Forms'
# shared libraries and the LLVM 22.1.8 macOS 14 runtime in
# Contents/Frameworks, every absolute rpath replaced by
# @executable_path/../Frameworks, signed ad hoc (libraries first, then the
# bundle), as GUI.Forms' "Runtime files to ship" requires.
#
# usage: tools/package_macos.sh <build directory> <runtime prefix> <output directory>
# The output directory must not already hold cmac.app.
set -eu
build=$1
runtime=$2
output=$3
bundle="$output/cmac.app"
if [ -e "$bundle" ]; then
    echo "package_macos: $bundle exists; remove it first" >&2
    exit 1
fi
mkdir -p "$output"
cp -R "$build/cmac.app" "$bundle"
frameworks="$bundle/Contents/Frameworks"
mkdir -p "$frameworks"
for library in libgui_forms_application.0.dylib libgui_forms_threading.0.dylib; do
    cp -L "$build/gui_forms/$library" "$frameworks/$library"
done
for library in libc++.1.dylib libc++abi.1.dylib libunwind.1.dylib; do
    cp -L "$runtime/lib/$library" "$frameworks/$library"
done

# Removes every LC_RPATH of a Mach-O file and adds the given one.
replace_rpaths() {
    file=$1
    replacement=$2
    for path in $(otool -l "$file" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }'); do
        install_name_tool -delete_rpath "$path" "$file" 2>/dev/null
    done
    install_name_tool -add_rpath "$replacement" "$file" 2>/dev/null
}

executable="$bundle/Contents/MacOS/cmac"
replace_rpaths "$executable" "@executable_path/../Frameworks"
for library in "$frameworks"/*.dylib; do
    replace_rpaths "$library" "@loader_path"
    codesign --force --sign - "$library"
done
codesign --force --sign - "$bundle"
echo "staged $bundle"
