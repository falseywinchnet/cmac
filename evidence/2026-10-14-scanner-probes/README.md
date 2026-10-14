# Scanner probes, 2026-10-14

Throwaway C used for `planning/SCANNER_STUDY.md`. Kept unchanged as the
record; not product code and not in the house style.

| File | What it measures |
|---|---|
| `caps.c` | volume capabilities (`searchfs`, clones, sparse files) |
| `bulk.c` | parallel `getattrlistbulk` walk; arguments in the study, §7 |
| `sfs.c` | whole-volume `searchfs` |
| `dirstats.c` | survey of folders carrying APFS maintain-dir-stats (recursive generation count) |

The APFS directory statistics timings used the read-only probe
`explorations/change-sources/catalog/src/dirstat_fsctl.c` from
[`jlevy/fdu`](https://github.com/jlevy/fdu) (MIT), built unchanged:

```sh
clang -O2 -o dirstat_fsctl dirstat_fsctl.c
/usr/bin/time -p ./dirstat_fsctl /System/Volumes/Data
```

Machine and conditions are in the study, §1.
