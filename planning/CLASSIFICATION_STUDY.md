# Classification study: yellow, red and the ownership lines

Recorded 2026-10-14. This proposes what cmac colours and why. The owner
chooses the red tiers (decision D4 in `PLAN.md`); nothing here is final.

cmac is diagnostic. A red rectangle says "cmac believes this folder holds
temporary data". It never says "delete this", and cmac offers no way to.

## 1. Sources studied

| Source | What it offers | What it does not |
|---|---|---|
| Apple's own conventions | The meaning of `Caches`, `tmp` and the per-user `T`/`C` folders; kernel flags for purgeable, sparse, clone-sharing and dataless files | A complete list of caches |
| [Pearcleaner](https://github.com/alienator88/Pearcleaner) (Apache 2.0 with Commons Clause) | App leftover locations (`Logic/Locations.swift`) and a developer-storage list of about 30 tools (`Views/DevelopmentView.swift`) | A temporary/permanent distinction: its developer list mixes regenerable caches with user data |
| [BleachBit](https://github.com/bleachbit/bleachbit) (GPL-3.0) | Per-application cleaner definitions with warnings for risky options (`cleaners/*.xml`) | Much macOS coverage: 15 definitions are tagged for macOS, mostly browser caches under `~/Library/Caches` |

Neither licence admits copying code or data into cmac. The tiers below are
written from Apple's conventions and named locations; the studies are cited
for what they taught.

**Lesson from Pearcleaner:** "developer storage" is not "temporary".
Among its entries are editor settings (`Application Support/Code/User`),
installed extensions (`~/.vscode/extensions`) and whole Conda installations
(`~/anaconda3`). Those are restorable at best and personal at worst. cmac
admits a path to red only with a stated reason it is regenerated without the
user.

**Lesson from BleachBit:** classify per kind (cache, log, history), and mark
the kinds where a belief could be wrong. cmac's version of a warning is a
weaker tier, not a dialog.

## 2. The kernel's own signals (best evidence)

Requested during the scan at no measured cost (`SCANNER_STUDY.md` §3.1):

| Flag | Meaning | cmac use |
|---|---|---|
| `EF_IS_PURGEABLE` | the system may delete this file under space pressure | red, in every option: the OS itself says so |
| `EF_IS_SPARSE` | the file has holes | sized by allocation, never by length |
| `EF_MAY_SHARE_BLOCKS` | the file may be an APFS clone | private size measured in a second pass |
| `SF_DATALESS` | the contents live in iCloud, not on disk | shown at its local size (near zero) and marked |

## 3. Red tiers (proposed)

| Tier | Contents | Reason | Risk of a wrong belief |
|---|---|---|---|
| R1 system temporary | `/private/tmp`, `/private/var/tmp`, each user's temporary folder (`confstr(_CS_DARWIN_USER_TEMP_DIR)`, under `/private/var/folders/…/T`), purgeable files anywhere | the system clears these itself | lowest |
| R2 caches | `~/Library/Caches`, `/Library/Caches`, `/System/Library/Caches`, each user's cache folder (`_CS_DARWIN_USER_CACHE_DIR`, `…/C`), sandboxed apps' `~/Library/Containers/*/Data/Library/Caches`, `~/.cache` | Apple defines `Caches` as data an app can regenerate | low; a few apps misuse `Caches` for downloads |
| R3 logs and diagnostics | `~/Library/Logs`, `/Library/Logs`, `/private/var/log`, `DiagnosticReports` folders | written for diagnosis, rotated by the system | low, but a log can be the only record of a fault |
| R4 build output and package caches | Xcode `DerivedData`, `~/Library/Developer/CoreSimulator/Caches`, Go's build cache, `~/.npm/_cacache`, `~/.cargo/registry/cache`, `~/.gradle/caches` | rebuilt by the tool on demand | moderate: rebuilding can take hours or need a network |

**Never red** (examples): `Application Support`, `Containers` data outside
`Caches`, `Preferences`, `Downloads`, `Documents`, Xcode `Archives` (user
artifacts), simulator devices, `node_modules`, virtual environments, Conda
installations, mail, photos libraries, and the Trash (emptying it is the
user's decision; it may get its own mark, decision D4).

## 4. Yellow

Space that belongs to the system rather than to the user's files:

- **Swap:** `/System/Volumes/VM/swapfile*` (6 × 1 GiB on the study machine),
  and any `sleepimage`.
- **Free space:** the APFS container's free space, shared by all its
  volumes.
- **Virtual mapping:** to be fixed by decision D2. Candidates: space held by
  local snapshots, the container's other volumes (Preboot, Recovery, VM,
  Update), and the difference between a volume's used space and the sum of
  its files.

## 5. Ownership lines

Lines drawn over the cushions mark whose region a rectangle is in, so the
colours above still show. Proposed regions (decision D3 sets the look and
the boundaries):

- **User:** every home folder under `/Users` (each user distinguishable).
- **System:** the sealed System volume (`/System`, `/usr`, `/bin`, `/sbin`),
  `/Library`, `/private`.
- **Unmarked:** `/Applications`, `/Users/Shared`, `/opt`, `/usr/local`, and
  other volumes.

A new look is shown to the owner as labelled candidates on a real scan
before it is adopted (the PlaySuite rule for materials, applied here).
