// Probe: which directories report APFS maintain-dir-stats (nonzero recursive gencount),
// and what ATTR_DIR_ALLOCSIZE says for them. Walks with getattrlist per directory (slow, survey only).
#include <sys/attr.h>
#include <sys/vnode.h>
#include <fts.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct out { uint32_t len; attribute_set_t rs; uint32_t dir_pad; off_t alloc; off_t datalen; uint64_t gen; } __attribute__((packed));
int main(int argc, char** argv) {
    char* paths[] = { argv[1], NULL };
    FTS* f = fts_open(paths, FTS_PHYSICAL | FTS_XDEV | FTS_NOSTAT, NULL);
    unsigned long long dirs = 0, marked = 0;
    FTSENT* e;
    while ((e = fts_read(f))) {
        if (e->fts_info != FTS_D) continue;
        dirs++;
        struct attrlist al; memset(&al, 0, sizeof al); al.bitmapcount = ATTR_BIT_MAP_COUNT;
        al.commonattr = ATTR_CMN_RETURNED_ATTRS; al.dirattr = ATTR_DIR_ALLOCSIZE | ATTR_DIR_DATALENGTH; al.forkattr = ATTR_CMNEXT_RECURSIVE_GENCOUNT;
        char buf[256]; memset(buf, 0, sizeof buf);
        if (getattrlist(e->fts_accpath, &al, buf, sizeof buf, FSOPT_NOFOLLOW | FSOPT_ATTR_CMN_EXTENDED) != 0) continue;
        char* p = buf + 4; attribute_set_t rs = *(attribute_set_t*)p; p += sizeof rs;
        off_t alloc = -1, dl = -1; uint64_t gen = 0;
        if (rs.dirattr & ATTR_DIR_ALLOCSIZE) { alloc = *(off_t*)p; p += 8; }
        if (rs.dirattr & ATTR_DIR_DATALENGTH) { dl = *(off_t*)p; p += 8; }
        if (rs.forkattr & ATTR_CMNEXT_RECURSIVE_GENCOUNT) { gen = *(uint64_t*)p; p += 8; }
        if (gen != 0 || (argc > 2 && dirs <= 3)) { marked++; printf("gen=%llu alloc=%lld datalen=%lld returned_dir=0x%x  %s\n", gen, alloc, dl, rs.dirattr, e->fts_path); if (gen != 0) fts_set(f, e, FTS_SKIP); }
    }
    printf("dirs=%llu marked=%llu\n", dirs, marked);
    return 0;
}
