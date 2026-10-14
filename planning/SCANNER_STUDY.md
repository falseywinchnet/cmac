# Scanner study: how fast can a Mac volume be read?

Recorded 2026-10-14. The probes are in
`evidence/2026-10-14-scanner-probes/` (throwaway C, not product code); every
number below came from them on the machine described in §1. Repeat with the
commands in §7 before quoting any of it elsewhere.

## 1. Machine and conditions

| | |
|---|---|
| Machine | owner's MacBook, Apple A18 Pro, 6 CPUs (2 performance, 4 efficiency) |
| System | macOS 26.6.1 |
| Storage | internal APFS container `disk3`; FileVault on |
| Data volume | `disk3s5`, 194 GiB used of 228 GiB, 1.1 M inodes |
| System volume | `disk3s1s1` (sealed snapshot), 12 GiB, 459 k inodes |
| Load | Chrome, WindowServer and others running; load average about 6.4 |
| Cache | warm (repeated runs). `purge` needs root, so no controlled cold run exists; the first single-thread run (49.2 s) was partly cold and is not a cold measurement. |
| Privacy | the shell had no Full Disk Access: 210 folders refused (`EPERM`) on the Data volume, 267 on the whole group |

Wall times on this loaded laptop vary by up to a factor of two between
identical runs. Kernel CPU time (`sys`) is steadier and is quoted with them.
User CPU was 0.13 to 0.54 s in every walk: **the scan is entirely kernel
work**; nothing cmac does in user space matters next to the system calls it
makes.

## 2. What was compared

1. **Parallel `getattrlistbulk`** (`bulk.c`): N threads drain one LIFO
   directory queue. Each directory is opened with
   `openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW)` from the
   parent's still-open descriptor (reference-counted, closed when its last
   child has been opened), then read with `getattrlistbulk` into a 256 KiB
   per-thread buffer. Requested: name, error, type, file id, link count,
   allocated size, logical length; variants add device id, directory entry
   count and mount status, BSD flags, extended flags and private size.
2. **`searchfs`** (`sfs.c`): one whole-volume catalog search matching every
   object, 4 MiB return buffer, resumed until done. This is the closest
   public analogue of WizTree's direct read of the NTFS Master File Table: the
   kernel walks the file system's own tree instead of the program walking
   directories.
3. **Raw b-tree read**, as WizTree does on NTFS: examined, not run (§4).

## 3. Results

### 3.1 Parallel `getattrlistbulk`, Data volume (1.05 M entries)

| Threads | Wall (s) | Note |
|---|---|---|
| 1 | 49.2 | first run, partly cold |
| 2 | 15.3 | |
| 4 | 8.1 | |
| 6 | 6.3 | best; equals the CPU count |
| 8 | 7.9 | |
| 12 | 7.7 | |

Repeated 6-thread runs: 5.9 to 11.3 s wall, 11 to 14 s kernel CPU. More
threads than CPUs adds kernel CPU (10 threads: 14 to 16 s) without lowering
wall time.

| Variant (6 threads) | Calls | Wall (s) | Kernel CPU (s) | Verdict |
|---|---|---|---|---|
| base (device id for mounts) | 257,566 | 7.1 to 11.3 | 11.7 to 13.9 | baseline |
| stop at the parent-reported entry count | 142,359 | 6.5 to 8.7 | 12.6 to 13.6 | keep: 45% fewer calls, free |
| + BSD flags and extended flags | 142,397 | 5.9 to 8.3 | 11.7 to 12.4 | keep: no measurable cost |
| + private size for every file | 142,406 | 14.0 to 14.3 | 27.1 to 28.8 | defer: doubles kernel work |

*Entry-count stop.* Every directory otherwise costs a final call that only
says "no more entries". The parent's listing already returns
`ATTR_DIR_ENTRYCOUNT`, so the child read stops when that many have been
seen. A directory that changes during the scan can make the count stale; the
scanner must treat a short or long read as ordinary (read on until zero when
the count was not reached, and accept extra entries when it was exceeded).

### 3.2 The whole volume group from `/`

Finder's "Macintosh HD" is two volumes, System and Data, joined by
firmlinks. **`stat` reports one device (16777234) for `/`, `/Users` and
`/System/Volumes/Data`,** and `getattrlistbulk` reports a mount point's
covered directory, so comparing device ids does not find mounts:

| Walk from `/`, 6 threads | Files | Folders | Allocated | Wall (s) |
|---|---|---|---|---|
| prune by device id | 2,127,554 | 455,024 | 424.5 GiB | 15.7 to 18.5 |
| prune by `ATTR_DIR_MOUNTSTATUS` mount points | 1,221,288 | 312,734 | 214.9 GiB | 9.4 |

The first counts the Data volume twice (once through the firmlinks, once
through `/System/Volumes/Data`). Pruning at `DIR_MNTSTATUS_MNTPOINT` walks
each object once and gives Finder's view: System (12.4 GiB) plus Data
(202.5 GiB). **1.53 M entries in 9.4 s, about 163,000 entries per second,
warm.**

### 3.3 `searchfs`

| Volume | Wall (s) | Kernel CPU (s) | Entries | Calls |
|---|---|---|---|---|
| Data | 23.0 to 24.3 | 3.8 to 4.2 | 1.05 M | 24 to 25 |
| System (sealed) | 2.9 | 1.5 | 486 k | 7 |

- It is **single-threaded and cannot be split**: on APFS the search criteria
  are ignored. File-id ranges `[0, 1000]` and `[0, 2]` both returned every
  object, so ranges cannot be handed to parallel searches.
- On the Data volume it spends 80% of its time waiting, not computing;
  longer time slices (2 s or 30 s) changed nothing.
- Requesting `ATTR_CMN_RETURNED_ATTRS` fails with `EINVAL`, so the layout of
  each record has to be inferred from the type.
- A resumed search fails with `EBUSY` if the catalog changes, and must then
  start again. The Data volume changes constantly; these runs were lucky.
- It reached about 2,700 more files than the walk, which fits the walk's 210
  refused folders. Which folders those files are in was not checked.

**Verdict:** parallel `getattrlistbulk` is about 2.8 times faster for the
whole group (9.4 s against 2.9 + 23.3 s) and is the scanner. `searchfs` is
not used (§5).

### 3.4 What the kernel says about sizes

From the Data volume walk with extended flags and private size:

| Measure | Value |
|---|---|
| logical length, summed | 665.5 GiB |
| allocated size, summed | 202.5 GiB |
| private size (blocks shared with no clone), summed | 164.1 GiB |
| files that may share blocks (`EF_MAY_SHARE_BLOCKS`) | 27,115 |
| sparse files (`EF_IS_SPARSE`) | 227 |
| purgeable files (`EF_IS_PURGEABLE`) | 14 |
| dataless (iCloud-evicted, `SF_DATALESS`) | 555 |

Logical length overstates use more than threefold here (sparse disk images),
and allocated size overstates it by about 38 GiB (APFS clones counted once
per clone). The treemap must size by allocation and must say how it counts
clones; see `PLAN.md` decision D5.

## 4. Why not read the b-tree directly (the WizTree way)

WizTree is fast on Windows because the NTFS Master File Table is one file
listing every object, readable sequentially with administrator rights. On
this Mac the equivalent is closed:

- The raw devices (`/dev/rdisk3`, `/dev/rdisk3s5`) are `root:operator`,
  mode 0640. cmac never escalates.
- With FileVault on, the Data volume's file-system tree is encrypted with a
  key only the kernel holds. A raw read returns ciphertext.
- APFS is copy-on-write: the current tree is found through the object map
  and checkpoints, not at a fixed place, and it changes while it is read.

A raw reader could work for an unencrypted external APFS or HFS+ disk read
as root. That is outside cmac's no-escalation rule and is not planned.

## 5. Privacy

`searchfs` returning names under folders that privacy protection refused to
the walk is a reason **not** to use it, not a feature. cmac shows a refused
folder as protected, with an unknown size, and tells the user how to grant
Full Disk Access if they want it counted.

## 5a. Looking for a faster path Apple already provides

The owner asked (2026-10-14) whether Apple exposes the file-system tree
through some canonical but little-known route, even one that needs the user's
permission. Five candidates were examined.

| Route | What it gives | Measured here | Verdict |
|---|---|---|---|
| `searchfs` (the documented catalog search) | every object, in tree order | 23 s for the Data volume, single-threaded, 80% waiting (§3.3) | slower |
| APFS directory statistics, read through `APFSIOC_GET_DIR_STATS_EXT` (private `fsctl` `0xC1104A71`, 272-byte structure, read-only; the call `apfs.util -S` makes) | a subtree's descendant count and physical size, no per-file data | Data volume root: 20.1 s wall, 2.3 s kernel CPU, 984,617 descendants; `~`: 13.1 s; `~/Developer`: 0.76 s. On a folder already marked maintain-dir-stats: 0.1 ms | single-threaded in-kernel walk; slower than the parallel walk for a whole volume, and totals only |
| `ATTR_DIR_ALLOCSIZE`, `ATTR_DIR_DATALENGTH` | documented as the directory's size | 0 and the directory record's own length, even on marked folders | not the recursive size |
| `ATTR_CMNEXT_RECURSIVE_GENCOUNT` | a generation count that changes when anything below a marked folder changes | 1,342 folders already marked by the system: sandboxed apps' `Caches` and `tmp`, Spotlight's indexes, `Library/Caches/com.apple.helpd` | a free skip signal for rescans, on folders Apple marked |
| Spotlight (`kMDItemFSSize`) | a prebuilt index | not run | incomplete by design (hidden and system folders excluded); not a scanner |

And the cost inside the walk itself: a names-only walk (no sizes) used the
same kernel CPU as the full walk (9.6 to 11.8 s against 10.7 to 13.0 s). The
cost is about 10 µs of kernel time per directory entry, whatever is asked
for. No directory API avoids it; the parallel walk only spreads it over the
cores and overlaps the waits that make `searchfs` and the in-kernel walk
slow.

Prior art: the `jlevy/fdu` research
([PR 142](https://github.com/jlevy/fdu/pull/142),
`docs/project/research/research-2026-09-27-disk-growth-change-sources.md`,
MIT) found the same `fsctl`, reports that Apple's DiskSpaceDiagnostics uses
it (`APFSIOC_DIR_STATS_OP`) with `getattrlistbulk` for its space
attribution, that `apfs.util -M` can mark a folder maintain-dir-stats
without privileges, and that marks used as a skip signal refreshed a
225,653-entry tree in 1.04 s against a 7.9 to 9.7 s walk with no misses,
while the totals themselves drifted and were not authoritative.

### What this means for speed

1. **The first scan** is bounded by the kernel's per-entry cost. The levers
   are cores (the M4 has 10), a quiet machine, and showing the picture while
   the scan runs: the container and volume totals are known at once from
   `statfs`, and the top-level folders fill in as their subtrees finish. On
   this loaded laptop the best Data-volume walk was 5.5 s and the whole
   group 9.4 s.
2. **Every later scan** need not be a scan. cmac keeps the last snapshot,
   shows it at once, and revisits only folders that changed:
   - the FSEvents journal since the stored event id names changed folders
     (it misses files still held open by a writer, fdu found; those are
     found with libproc's open-file list for the user's own processes);
   - a recursive generation count that has not moved proves a marked
     subtree unchanged, without reading it.
3. **Optional, with the user's consent:** cmac could mark its top-level
   folders maintain-dir-stats itself, so that every subtree gets a skip
   signal. That writes a persistent flag into the user's file system through
   a private interface Apple documents only for empty folders, so it breaks
   the read-only rule and needs re-verification on every macOS release.
   The owner declined it (D6, 2026-10-14); cmac only reads the counts of
   folders the system marked.

## 6. Still to measure (gate of stage S1)

- The M4 (10 CPUs): thread count, and whether efficiency cores help or
  hinder.
- Thread QoS class (user-initiated against default).
- Buffer size: 64 KiB, 256 KiB, 1 MiB.
- Queue order: LIFO (depth-first, current) against FIFO, for descriptor
  count and cache locality.
- A cold cache, with the owner running `sudo purge` before each run.
- `ATTR_CMNEXT_RECURSIVE_GENCOUNT`: if APFS keeps it for directories, a
  rescan could skip unchanged subtrees. Find out which directories carry it.
- FSEvents (journal since a stored event id) for a rescan that revisits only
  the folders that changed. This, not a faster first scan, is where repeat
  scans become nearly instant.
- Private size as a second pass over only the `EF_MAY_SHARE_BLOCKS` files
  (27 k of 906 k here), with `getattrlist` per file.

## 6a. cmac-scan, first measurements (2026-10-14)

The S1 scanner (`src/scan/`), built with LLVM 22.1.8 and GUI.Forms' runtime,
one `gui_forms::Worker` per thread. Warm cache, `cmac-scan /`.

**Mac mini M4** (10 CPUs: 4 performance, 6 efficiency; macOS 26.5; load
average about 2):

| | cmac-scan, 10 threads | probe, 10 threads |
|---|---|---|
| walk | 9.58, 9.63, 9.73 s | 9.52, 9.63, 9.64 s |
| kernel CPU | 59 s | 59 s |
| objects | 2,008,806 | the same, counted as the probe counts |
| snapshot build | 0.06 s | — |
| peak resident | 450 to 482 MiB | — |

| Threads | 4 | 6 | 8 | 10 | 12 | 16 | 20 |
|---|---|---|---|---|---|---|---|
| walk (s) | 11.1 | 9.0 | 9.1 | 9.7 | 9.9 | 10.2 | 10.0 |
| kernel CPU (s) | 15 | 25 | 42 | 60 | 71 | 83 | 84 |

Beyond 6 threads the kernel burns more CPU for no gain: something inside
it is serialized. It is not per volume. Data alone took 5.7 s, System alone
3.9 s, and both at once 8.8 s, about their sum.

The sealed System volume is the expensive one: **47 µs of kernel CPU per
object against 13 µs on Data** (498 k objects, 23.6 s CPU; 1.52 M objects,
19.8 s). It is a quarter of the objects and 40% of the walk. It changes only
with an OS update, so its part of the snapshot can be kept against the
sealed snapshot's identity and reused (S2), leaving about 5.7 s for a whole
M4 scan.

**Owner's MacBook** (as §1; load average 6.3): cmac-scan 10.4, 10.8 and 16.3
s; the probe 12.0, 12.2 and 12.6 s, interleaved. Parity within this
machine's noise. 1.54 M objects, peak resident 275 to 284 MiB.

**Totals now differ from the probe's, correctly.** The probe summed every
hard link. One 223 GiB sparse disk image here has two links; cmac counts it
once, so its logical total is 460 GiB against the probe's 685 GiB.

**The bug the M4 found.** The first M4 run walked only the System volume
(486 k objects). The firmlinked folders (`/Users`, `/Applications`,
`/Library`, `/private`, …) are empty stubs on the System volume, so the
parent's entry count is 0, and the reader stopped before its first call.
Fixed: a firmlink's count is treated as unknown, and at least one call is
always made. `test_firmlink_count` guards it.

## 6b. The saved snapshot and the journal (stage S2, 2026-10-14)

cmac keeps its last scan as a record (`src/scan/snapshot_file.hpp`: header,
56-byte node records, name pool) and, at launch, shows it, replays the
FSEvents journal since the position stored with it, and rereads only the
folders that changed; everything else is copied from the record.

**FSEvents replay cost**, owner's MacBook, history of the whole volume
group from `/`:

| Event ids behind | Events delivered | Replay |
|---|---|---|
| 20,000 | 580 | 0.10 s |
| 200,000 | 7,908 | 0.66 s |
| 2,000,000 | 15,382 | 8.05 s |

About 4 µs per event id behind the cursor, spent in `fseventsd` (fdu found
the same: the cost belongs to the volume's journal, not to the root). A
replay that would take longer than 3 s is skipped for a full scan
(`RefreshPolicy`). Paths arrive canonical (`/Users/...`), matching a
snapshot of `/`. Live events that arrive during a long replay come flagged
dropped; the replay therefore stops at the position taken when it began,
and anything later is replayed next time from the journal on disk.

**Measured on the M4**, `cmac-scan / --cache FILE`, warm:

| | First run (no record) | Next runs (seconds to minutes later) |
|---|---|---|
| read the record (149 MB, 2.01 M objects) | — | 0.05 to 0.08 s |
| plan (journal replay, reuse marks) | — | 0.02 to 0.12 s (15 to 180 changes) |
| scan | 9.75 s full | 0.26 to 0.67 s (9 to 181 folders reread, about 386,000 copied) |
| save the record | 0.05 s | 0.03 to 0.05 s |

The app, launched twice in a row on the M4 with `--capture`: **10.3 s** from
process start to the first complete picture with no record, **0.71 s** with
one (window, load, replay, incremental scan, layout, paint and capture).

**How exact.** `--verify` scans in full right after the refresh and compares
node by node. The refreshed root matched the full one within 1 MiB of
160 GiB; 101 nodes differed (a dozen files and their ancestors). Two plain
full scans a few seconds apart differ by 11 to 12 nodes, so most of the rest
is a live system changing between the two scans: build outputs, a stats file
`systemstats` holds open, Chrome's and Google Updater's databases.

**A change FSEvents never reports.** The first `--verify` found 628 files
whose copied size was 1,044,480 bytes too large each: files written 38
minutes earlier still held APFS's speculative preallocation at the first
scan, and APFS released it lazily a minute later, with no write and no
event. The reuse plan now rereads any folder holding a file whose
allocation exceeds its rounded length by 64 KiB or more (not sparse,
compressed or dataless) until it settles.

**Known limits.** A file held open by a writer is reported only when it is
closed (fdu, 2026-09-27); its size is the last one read. The record holds
two snapshots in memory during a refresh (peak resident 632 MiB on the M4).
No cold-cache or after-reboot launch has been measured; the journal and the
record both survive a reboot.

## 7. How to repeat

```sh
cd evidence/2026-10-14-scanner-probes
clang -O2 -o caps caps.c && ./caps / /System/Volumes/Data /System/Volumes/VM
clang -O2 -o bulk bulk.c
# bulk <root> <threads> <mount mode: 1 device id, 2 mount status + entry count> <extended: 0, 1 flags, 2 flags + private size>
for t in 1 2 4 6 8 12; do ./bulk /System/Volumes/Data $t 2 0; done
./bulk / 6 2 1
clang -O2 -o sfs sfs.c
# sfs <volume> <request returned attrs: 0> [fileid low] [fileid high] [time slice s]
./sfs /System/Volumes/Data 0
./sfs / 0
clang -O2 -o dirstats dirstats.c
./dirstats /System/Volumes/Data   # lists folders with a nonzero recursive generation count
```

The directory statistics timings: see this folder's `README.md`.
