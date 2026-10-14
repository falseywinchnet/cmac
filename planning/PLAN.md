# cmac programme

Started 2026-10-14. Each stage has a gate: a measurement or demonstration
recorded here before the stage counts as done. A stage starts only once the
gates it depends on are recorded. Open decisions are the owner's; do not
assume them.

## Pins

| Dependency | Repository | Revision | Use |
|---|---|---|---|
| GUI.Forms | `falseywinchnet/gui_forms` | `a0739e4c86a19afb1493b8e31e3b1392edfddd0b` (main, 2026-10-14) | window, controls, threading, live surfaces; LLVM 22.1.8 contract |
| Render engine | `falseywinchnet/games`, `shared/render` | `7e2a21e8d948d58ec30be7a952e6ee051c0936f4` (main, 2026-10-14) | `render_core` (`Target`, `Rect`, `r2d`) and `ui/surface` |

Validate a new pin before adopting it (build, tests, one scan, one frame).

## Stages

### S0. Foundation — done 2026-10-14

Guardrails (`AGENTS.md`), the house style copy, the scanner study and the
classification study.

Gate (recorded): `SCANNER_STUDY.md`. Parallel `getattrlistbulk` with
`openat` from held parent descriptors, mount points pruned by
`ATTR_DIR_MOUNTSTATUS`, walked the whole volume group (1.53 M entries) in
9.4 s warm with 6 threads on the owner's MacBook, against about 26 s for
`searchfs` and 20 s for APFS's in-kernel directory statistics. A raw b-tree
read is closed (root-only devices, FileVault).

### S1. Scanner

The scan engine and a command-line `cmac-scan` that prints totals, counts
and timings. No window yet.

- Directory queue drained by `AtomicThreadPool` tasks inside one `Worker`;
  per-task attribute buffer, node arena and name pool; parent descriptors
  reference-counted and closed after their last child is opened.
- Nodes: typed `NodeIndex`, parent, first child and child count (children
  contiguous after the merge), name offset and length in a pool, allocated
  size, logical size, file id, link count, kernel flags, kind. Target size
  about 48 bytes per node plus names; state the measured figure.
- Hard links counted once (file id, when the link count exceeds 1); clones
  flagged for the private-size pass; sparse files sized by allocation;
  symbolic links never followed; refused folders kept as protected nodes
  with unknown size; dataless files marked.
- Entry-count stop with the stale-count rule in `SCANNER_STUDY.md` §3.1.
- Cancellation checked between directories. Progress as atomic counters.
- The finished tree published as an immutable snapshot.

Gate record so far (2026-10-14, `SCANNER_STUDY.md` §6a):
- done: built with LLVM 22.1.8 on the M4; fixture tests pass (hard links
  once, a clone, a sparse file, a symbolic link loop, a refused folder, a
  5,000-entry folder, a 300-deep chain, one against many threads, read to
  the end, cancellation, the firmlink count);
- done: warm wall time and kernel CPU at parity with the probe on both
  machines, with exactly the probe's object counts;
- open: cold runs (needs the owner's `sudo purge`); peak memory, measured at
  about 230 bytes per object, which is too much (target: release the
  workspaces as the snapshot is built); cancellation latency; a mounted disk
  image fixture; a directory changing during its read.

Gate: on the M4 and the MacBook, warm and cold (the owner runs
`sudo purge` before each cold run), scanning `/`:
- entry counts within 0.1% of the probe's on the same machine and minute;
- wall time no worse than the probe's plus 10%, with kernel and user CPU;
- peak memory recorded;
- cancellation honoured within 100 ms;
- fixture tests pass: hard links once, clones, sparse files, a symbolic
  link loop, a refused folder, a mounted disk image pruned, and a directory
  changing during its read.
Also record the measurements still open in `SCANNER_STUDY.md` §6.

### S2. Snapshot and rescan

Depends on S1.

- A compact snapshot file of the last scan, read at launch in well under a
  second (state the measured time and size).
- Rescan from the FSEvents journal since the stored event id, plus the
  user's open writers (libproc); revisit only changed folders.
- Skip a subtree whose recursive generation count has not moved (only
  folders the system marked itself; cmac never marks, D6).
- Keep the sealed System volume's part of the snapshot against the sealed
  snapshot's identity and reuse it until the OS changes: it costs 47 µs per
  object against Data's 13 µs, 40% of an M4 walk (`SCANNER_STUDY.md` §6a).

Gate: time from launch to a complete, current picture on the second launch,
and zero misses against a full walk after a scripted churn fixture.

### S3. Space accounting (the yellow)

Depends on S1.

The container map: each volume's use, free space, and virtual mapping
(swap files and the sleep image, D2), in yellow; snapshots, the other
volumes and the residual shown in neutral tiles. Free space from `statfs`; purgeable space through
Foundation's volume capacity keys (in `src/platform/macos/`); a signed,
labelled residual for space no file accounts for. The clone accounting of
D5: a second pass reads `ATTR_CMNEXT_PRIVATESIZE` for the files flagged
`may_share_blocks` only (27 k of 906 k on the study machine), each file is
sized by its private bytes, and the difference appears once as a "shared
by clones" tile.

Gate: the tiles sum to the container's capacity, with the residual shown
and explained on the study machine and the M4.

### S4. Classification (the red and the lines)

Depends on S1 and decisions D3 and D4.

The tier table as data, the kernel flags, and the user and system regions.

Gate: the owner reviews the table against a real scan; fixture tests.

### S5. Treemap

Depends on S1.

- Squarified layout (Bruls, Huizing and van Wijk, 2000) and cushion shading
  (van Wijk and van de Wetering, 1999), written from the papers.
- The shading kernel writes `render::Target` rows in the window's byte
  order; bands split on an `AtomicThreadPool` owned by the UI thread, each
  writing disjoint rows.
- Rectangles below one device pixel are merged into their parent's cushion,
  not drawn.
- Picking from a leaf index buffer; a selection change repaints only the
  old and new outlines through `render::Surface` damage.
- A settled picture schedules no frames.

Gate: full repaint time at 2560 × 1600 device pixels for the S1 snapshot of
`/`, and the repaint area of a selection change, measured on the M4.

### S6. Application

Depends on S3, S4, S5 and the GUI.Forms ribbon (D1).

The GUI.Forms window: ribbon, folder list synchronized with the treemap,
status line, "Show in Finder" for the selection, and a protected-folder
notice that explains Full Disk Access.

Gate: an M4 smoke run (launch, scan, select, show in Finder, close), and
layouts checked at the window minimum and at scales 1 and 2.

### S7. Packaging and CI

Copy the Plan Paint and PlaySuite recipe exactly: pinned GUI.Forms, LLVM
22.1.8, its published compiler cache. macOS only.

Gate: CI green on the pinned revisions; an installable package.

## Decided

Answered by the owner on 2026-10-14:

- **D1 Ribbon:** a reusable ribbon control in GUI.Forms, shared with Plan
  Paint, not a cmac composition. S6 waits on it
  (`TOOLKIT_REQUESTS.md`).
- **D2 Virtual mapping** means memory: swap files and the sleep image.
  Snapshots, the container's other volumes and the residual are shown but
  not yellow.
- **D6 Marking folders:** no. cmac never marks folders maintain-dir-stats.
  It reads the generation counts of folders the system marked itself.
- **D7 Snapshot file:** yes. cmac keeps its last scan in its own cache
  folder and rescans only what changed. It writes nothing else.
- **D8 Repository:** public, MIT.

Answered on 2026-10-14, second round:

- **D4 Red tiers:** R1 system temporary, R2 caches and R3 logs are red,
  with purgeable files anywhere. R4 (build output and package caches) is
  not red.
- **D5 Clones:** each file is sized by its unique (private) bytes, and the
  blocks clones share appear once, as a separate tile. This needs the
  private-size pass over the files that may share blocks (added to S3).
- **D9 Floor:** Apple silicon, macOS 14 or later (GUI.Forms' contract).
- **Ribbon (D1, who and when):** this programme's author builds the
  reusable ribbon in GUI.Forms next, under GUI.Forms' own programme, while
  cmac's later stages proceed.

## Open decisions (the owner's)

- **D3 Ownership lines.** Boundaries of the user and system regions, and
  the line style (shown as labelled candidates on a real scan first).
- **D10 Trash.** Whether the Trash gets its own mark (it is never red).
