#include <sys/attr.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
struct volcaps { uint32_t len; vol_capabilities_attr_t caps; } __attribute__((packed));
int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        struct attrlist al; memset(&al, 0, sizeof al);
        al.bitmapcount = ATTR_BIT_MAP_COUNT; al.volattr = ATTR_VOL_INFO | ATTR_VOL_CAPABILITIES;
        struct volcaps vc;
        if (getattrlist(argv[i], &al, &vc, sizeof vc, 0) != 0) { perror(argv[i]); continue; }
        uint32_t f = vc.caps.capabilities[VOL_CAPABILITIES_FORMAT] & vc.caps.valid[VOL_CAPABILITIES_FORMAT];
        uint32_t n = vc.caps.capabilities[VOL_CAPABILITIES_INTERFACES] & vc.caps.valid[VOL_CAPABILITIES_INTERFACES];
        struct statfs s; statfs(argv[i], &s);
        printf("%-28s fs=%-6s dev=%-14s searchfs=%d bulk=%d clone=%d clone_mapping=%d dir_hardlinks=%d sparse=%d\n",
               argv[i], s.f_fstypename, s.f_mntfromname, !!(n & VOL_CAP_INT_SEARCHFS), !!(n & VOL_CAP_INT_READDIRATTR),
               !!(n & VOL_CAP_INT_CLONE), !!(f & VOL_CAP_FMT_CLONE_MAPPING), !!(f & VOL_CAP_FMT_DIR_HARDLINKS), !!(f & VOL_CAP_FMT_SPARSE_FILES));
    }
    return 0;
}
