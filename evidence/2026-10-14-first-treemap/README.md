# First treemap, 2026-10-14

`m4-volume-group.png`: the lower panel's picture of the Mac mini M4's whole
volume group, written by `cmac --capture` (the treemap buffer only, not the
window's header). 1280 × 724 device pixels at scale 1.

The app's status line for this run:

```text
/: 2,010,534 objects, 160.59 GiB in 9.6 s; free 12.04 GiB, virtual memory 2.00 GiB;
839 protected folders not counted; layout 34 ms, paint 5 ms, 742221 tiles
```

- Red: tiers R1 to R3 (caches, logs, temporary folders) and purgeable files.
- Yellow, right: free space (12 GiB) above virtual memory (2 GiB).
- Grey: folders too small to subdivide, and files without an extension.

Command (from the M4 mirror, launched over ssh, so privacy protection refuses
protected folders at once instead of prompting):

```sh
.build/app/cmac.app/Contents/MacOS/cmac --capture /tmp/cmac-capture.bmp
sips -s format png /tmp/cmac-capture.bmp --out m4-volume-group.png
```

Two bugs found by the first capture and fixed before this one: only each
folder's first child was laid out (per-depth scratch moved under a held
reference; `test_deep_coverage`), and virtual memory read as 216 GiB
(`statfs` gives the container's use on APFS; the VM volume's files are now
summed).
