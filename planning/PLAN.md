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

Depends on S1 and decision D7.

- A compact snapshot file of the last scan, read at launch in well under a
  second (state the measured time and size).
- Rescan from the FSEvents journal since the stored event id, plus the
  user's open writers (libproc); revisit only changed folders.
- Skip a subtree whose recursive generation count has not moved (folders
  the system already marks; decision D6 for folders cmac would mark).

Gate: time from launch to a complete, current picture on the second launch,
and zero misses against a full walk after a scripted churn fixture.

### S3. Space accounting (the yellow)

Depends on S1 and decision D2.

The container map: each volume's use, free space, swap files, and the
system's virtual mapping. Free space from `statfs`; purgeable space through
Foundation's volume capacity keys (in `src/platform/macos/`); a signed,
labelled residual for space no file accounts for.

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

Depends on S3, S4, S5 and decision D1.

The GUI.Forms window: ribbon, folder list synchronized with the treemap,
status line, "Show in Finder" for the selection, and a protected-folder
notice that explains Full Disk Access.

Gate: an M4 smoke run (launch, scan, select, show in Finder, close), and
layouts checked at the window minimum and at scales 1 and 2.

### S7. Packaging and CI

Copy the Plan Paint and PlaySuite recipe exactly: pinned GUI.Forms, LLVM
22.1.8, its published compiler cache. macOS only.

Gate: CI green on the pinned revisions; an installable package.

## Open decisions (the owner's)

- **D1 Ribbon.** GUI.Forms has no ribbon control; Plan Paint builds its own
  (`src/forms/ribbon.cpp`). Either a reusable GUI.Forms ribbon (two
  consumers now want one), or a cmac composition of `TabControl` and
  `CommandBar` for now.
- **D2 Virtual mapping.** Which space is yellow besides swap and free
  space: local snapshots, the container's other volumes, the residual, or
  something else meant by the term.
- **D3 Ownership lines.** Boundaries of the user and system regions, and
  the line style (shown as labelled candidates on a real scan first).
- **D4 Red tiers.** Which of R1 to R4 in `CLASSIFICATION_STUDY.md` are red,
  and whether the Trash gets its own mark.
- **D5 Clones.** Size each file by allocation (clones counted per copy, as
  Finder does) or by private size, with shared blocks shown as one
  separate tile.
- **D6 Marking folders.** Whether cmac may, with the user's consent, mark
  folders maintain-dir-stats through the private interface for faster
  rescans. It writes persistent file-system metadata, contrary to the
  read-only rule.
- **D7 Snapshot file.** Whether cmac may keep its last scan (every name and
  size) in its own cache folder. Needed for S2.
- **D8 Repository.** Public or private; licence (GUI.Forms is MIT).
- **D9 Floor.** macOS 14 or later on Apple silicon only (the GUI.Forms
  runtime contract), or Intel too.
