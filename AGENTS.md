# cmac working agreement

cmac is a native macOS disk-space diagnostic built on GUI.Forms and the
PlaySuite render engine. This file is the agreement for anyone, person or
model, changing it. Read it, then `planning/PLAN.md` and the stage you are
working on.

## What cmac is not

- **Not a cleaner.** cmac never deletes, moves, renames, trashes, compresses,
  rewrites, marks or changes permissions on anything. It writes only its own
  settings file and its scan cache (the last snapshot, in its own cache
  folder).
  No "clean", "free up" or "remove" command may be added. Revealing an item in
  Finder is the only outward action; whatever the user does there is theirs.
- **Never reads file contents.** Metadata only (names, sizes, flags,
  identities). No hashing, no content sniffing, no duplicate detection by
  content.
- **Never sends anything anywhere.** No network access, telemetry or update
  checks.
- **Never escalates.** No `sudo`, no privileged helper, no raw-device reads, no
  attempt to reach what privacy protection (TCC) withholds. A protected folder
  is shown as protected, with its size unknown, and the user may grant Full
  Disk Access themselves. (`searchfs` was measured to return names inside
  protected folders; cmac does not use it to fill those holes. See
  `planning/SCANNER_STUDY.md` §5.) Nor does it set APFS maintain-dir-stats
  on any folder (owner, D6).

## Code style (mandatory)

All first-party C++ and Objective-C++ follows
`planning/PROGRAMMING_HOUSE_STYLE.md`, a verbatim copy of the GUI.Forms copy
(itself a verbatim copy of `programming-house-style.md` in
`falseywinchnet/EpistemicGPT`). Change none of them alone; a change goes to
EpistemicGPT first and is copied here byte for byte. Read it in full before
writing code. The points that bite most often:

- Spell types: no `auto`, trailing returns or structured bindings.
- `(*pointer).member`, never `->`.
- No lambdas, anonymous callbacks or comprehensions; name every target.
- Hand-written comparisons; no `= default` comparisons or spaceship.
- Typed identities for things that can be confused: a node index is not a
  file id, a byte count is not a block count, allocated size is not logical
  size.
- Allocate before the loop and reuse; no per-entry allocation, logging or
  locking in the scan or paint kernels.
- Every datum has an owner and a lifetime; borrows stay valid through use.

Before committing, run the spelling check over everything you touched:

```text
python3 -B tools/check_house_style.py src tests tools
```

It checks spelling only. Ownership, lifetime, initialization and naming still
need review against the full document.

## Threading model (mandatory)

- Threads are GUI.Forms `Worker` and `AtomicThreadPool`; cancellation is its
  `CancellationFlag`. Never `std::thread`, `std::async`, `std::jthread`,
  `std::stop_token`, GCD or `NSThread`.
- One execution context per piece of mutable state:
  - The **UI thread** owns the window, the controls, the published tree
    snapshot it is showing, selection, and the treemap layout.
  - The **scan** runs on one `Worker` per CPU, all draining one shared
    directory queue. Each owns its own workspace (attribute buffer, entry
    arena, name pool); only the queue and the progress counters are shared,
    and they are synchronized at their boundary. The worker that drains last
    builds the snapshot. (`AtomicThreadPool::run` executes batches of up to
    64 tasks per thread on the calling thread, one after another, so it
    suits data-parallel batches, not long-running drains.)
  - The **paint** may split a frame into bands on an `AtomicThreadPool` owned
    by the UI thread; each band writes disjoint rows of the target.
- Results cross to the UI thread by publication, as PlaySuite's `Dealer`
  does: the worker fills an owned result, sets an atomic `done` with release
  order, and the UI thread collects it on its timer with acquire order.
  Progress is read from atomic counters the same way. Nothing on a worker
  touches a control.
- Declare a `Worker` after the context it borrows, so it is joined first.
  Cancellation is checked between directories, never inside the entry loop.
- A finished scan is published as an immutable snapshot. A rescan builds a
  new snapshot and replaces the old one only when complete; the UI never
  sees a tree that is being written.

## Speed

The goal is to be extremely fast, measured, never asserted. No result may be
called fast without the workload, machine, cache state and command that
produced it. `planning/SCANNER_STUDY.md` is the current record and the method
to repeat. A change to the scanner or the paint kernel states its measured
effect against that record.

## Dependencies

- GUI.Forms is pinned by full revision from `falseywinchnet/gui_forms` and
  follows its LLVM 22.1.8 contract, as Plan Paint and PlaySuite do. Do not
  modify the GUI.Forms checkout to fix a cmac build. A missing reusable
  capability (a ribbon, a reveal-in-file-manager host call) is a GUI.Forms
  request, recorded in `planning/TOOLKIT_REQUESTS.md` until it lands.
- The render engine is `shared/render` from `falseywinchnet/games`, pinned by
  full revision and included through its own `build.cmake` with `ENGINE_DIR`
  set, as a standalone game build does. cmac-specific kernels (the cushion
  shader, treemap layout) live in cmac.
- Objective-C++ is admitted only in `src/platform/macos/`. AppKit and
  Foundation types stay there.
- KDirStat, WinDirStat, Pearcleaner and BleachBit are studied, never copied:
  their licences (GPL, Apache 2.0 with Commons Clause) do not admit their
  code or data here. Implement from the published papers and from Apple's
  documentation; cite sources in the study documents.

## Where things live

- `src/scan/`: the scanner (macOS file system calls), the node arena and
  the snapshot.
- `src/space/`: container and volume accounting (free, swap, virtual mapping).
- `src/classify/`: the temporary-folder tiers and the user/system regions.
- `src/treemap/`: squarified layout, cushion coefficients, the shading kernel.
- `src/app/`: the GUI.Forms window, ribbon, folder list and selection.
- `src/platform/macos/`: Objective-C++ (Finder reveal, Foundation-only keys).
- `src/tools/`: command-line tools (`cmac-scan`, the scanner's measurement
  tool).
- `tests/`: fixture trees built in a temporary directory at test time.
- `planning/`: the programme, studies and decisions.
- `evidence/`: measurement records and the probes that produced them, kept
  unchanged. Probes are throwaway C and are not product code.

## Building

`docs/BUILDING.md`. cmac builds only with GUI.Forms' pinned LLVM 22.1.8
toolchain file and runtime; the owner's MacBook lacks them, so build and
test on the Mac mini M4 (`m4build`) and in CI, as GUI.Forms and PlaySuite
do. Copy their recipe exactly; do not improvise toolchains or caches.
