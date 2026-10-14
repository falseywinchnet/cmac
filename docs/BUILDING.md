# Building cmac

cmac builds with GUI.Forms' pinned toolchain, exactly as GUI.Forms and its
other consumers do: LLVM 22.1.8 through GUI.Forms' `cmake/llvm22.cmake`, with
the macOS 14 libc++ runtime built by GUI.Forms'
`tools/build_macos_runtimes.py`. Apple's system Clang is rejected by
GUI.Forms' configure check. Do not improvise another toolchain.

## On the Mac mini M4

The M4 has Homebrew `llvm@22` (clang 22.1.8), cmake, ninja and ccache, and
the runtime prefix PlaySuite uses:
`~/Developer/PlaysuiteDependencies/toolkit-4e126d6/toolchain/llvm-22.1.8-macos14`.

From the owner's MacBook, `m4build` syncs the working tree and runs a
command in the mirror (`.build/` is excluded, so dependencies and build
trees persist there):

```sh
m4build -- zsh -lc 'export PATH=/opt/homebrew/opt/llvm@22/bin:/opt/homebrew/bin:$PATH
  export GUI_FORMS_LLVM_RUNTIME=$HOME/Developer/PlaysuiteDependencies/toolkit-4e126d6/toolchain/llvm-22.1.8-macos14
  tools/fetch_dependencies.sh
  cmake -S . -B .build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=$PWD/.build/deps/gui_forms/cmake/llvm22.cmake \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  cmake --build .build/release --parallel 4
  ctest --test-dir .build/release --output-on-failure'
```

## Elsewhere

Any Apple-silicon Mac with the same three pieces: `brew install cmake ninja
ccache llvm@22`, `llvm@22`'s `bin` first on `PATH`, and
`GUI_FORMS_LLVM_RUNTIME` pointing at a runtime prefix built by
`.build/deps/gui_forms/tools/build_macos_runtimes.py` (see GUI.Forms'
`docs/user/install.md`).

## Measuring

`cmac-scan /` prints the outcome, counts, walk and build times, kernel and
user CPU, and peak memory. Record any figure you quote with the machine,
cache state and command, as `planning/SCANNER_STUDY.md` does.
