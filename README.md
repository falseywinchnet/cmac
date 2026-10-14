# cmac

A disk-space diagnostic for the Mac, in the line of KDirStat: scan a volume very
fast, show every file as a cushion-shaded rectangle sized by the space it
takes, and let you select a file and open its folder in Finder.

cmac is **diagnostic only**. It reads metadata and shows it. It never deletes,
moves, cleans, compresses or rewrites anything, and it never reads file
contents. The "c" is for that restraint: it is not a cleaner.

## What it shows

- One treemap of the whole volume group (Macintosh HD as Finder shows it),
  plus the container space that no file accounts for.
- **Yellow:** free space and virtual mapping (the swap files and the sleep
  image).
- **Red:** files in folders cmac believes are temporary (the tier table is in
  `planning/CLASSIFICATION_STUDY.md`). Red is a belief, not a recommendation.
- **Added lines** shade the regions that belong to user folders and to system
  folders, so ownership shows through the colours.
- A ribbon for commands, and a folder list synchronized with the treemap.

## Built on

- [GUI.Forms](https://github.com/falseywinchnet/gui_forms): window, controls,
  threading (`Worker`, `AtomicThreadPool`, `CancellationFlag`), live surfaces.
- The PlaySuite render engine (`shared/render` in
  [games](https://github.com/falseywinchnet/games)): the pixel targets and
  damage-limited surface the treemap draws into.

Both are pinned by revision; see `planning/PLAN.md`.

## Status

Planning. Stage S0 (guardrails and measurements) is recorded; the scanner
(S1) is next. `planning/PLAN.md` is the programme.
