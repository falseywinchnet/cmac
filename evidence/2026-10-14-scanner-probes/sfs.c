// Throwaway probe: whole-volume catalog walk with searchfs, matching every FILEID.
#include <sys/attr.h>
#include <sys/vnode.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
struct p64 { uint32_t len; uint64_t v; } __attribute__((packed));
int main(int argc, char** argv) {
    const char* vol = argv[1]; int want_returned = argc > 2 ? atoi(argv[2]) : 1;
    struct attrlist ret; memset(&ret, 0, sizeof ret); ret.bitmapcount = ATTR_BIT_MAP_COUNT;
    ret.commonattr = (want_returned ? ATTR_CMN_RETURNED_ATTRS : 0) | ATTR_CMN_NAME | ATTR_CMN_OBJTYPE | ATTR_CMN_FILEID | ATTR_CMN_PARENTID;
    ret.fileattr = ATTR_FILE_ALLOCSIZE | ATTR_FILE_DATALENGTH;
    struct p64 lo = { sizeof lo, argc > 3 ? strtoull(argv[3],0,0) : 0 }, hi = { sizeof hi, argc > 4 ? strtoull(argv[4],0,0) : UINT64_MAX };
    size_t bufsize = 4 << 20; char* buf = malloc(bufsize);
    struct fssearchblock sb; memset(&sb, 0, sizeof sb);
    sb.returnattrs = &ret; sb.returnbuffer = buf; sb.returnbuffersize = bufsize; sb.maxmatches = 1u << 20;
    sb.timelimit.tv_sec = argc > 5 ? atoi(argv[5]) : 2; sb.searchparams1 = &lo; sb.sizeofsearchparams1 = sizeof lo; sb.searchparams2 = &hi; sb.sizeofsearchparams2 = sizeof hi;
    sb.searchattrs.bitmapcount = ATTR_BIT_MAP_COUNT; sb.searchattrs.commonattr = ATTR_CMN_FILEID;
    struct searchstate st; unsigned long matches = 0; unsigned int opts = SRCHFS_START | SRCHFS_MATCHFILES | SRCHFS_MATCHDIRS;
    unsigned long long files = 0, dirs = 0, ab = 0, lb = 0, calls = 0, busy = 0;
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    for (;;) {
        int r = searchfs(vol, &sb, &matches, 0x08000103, opts, &st); int e = errno; calls++;
        if (r != 0 && e != EAGAIN) { if (e == EBUSY) { busy++; if (busy > 20) { printf("gave up after %llu EBUSY restarts\n", busy); break; } opts |= SRCHFS_START; files = dirs = ab = lb = 0; continue; } printf("searchfs error %d (%s) after %llu calls\n", e, strerror(e), calls); break; }
        char* p0 = buf;
        for (unsigned long i = 0; i < matches; i++) {
            uint32_t len = *(uint32_t*)p0; char* p = p0 + 4;
            attribute_set_t rs = { .commonattr = ret.commonattr, .fileattr = ret.fileattr };
            if (want_returned) { rs = *(attribute_set_t*)p; p += sizeof(attribute_set_t); }
            p += sizeof(attrreference_t);
            fsobj_type_t t = *(fsobj_type_t*)p; p += 4; p += 8; p += 8;
            if (t == VDIR) dirs++; else { files++; if (rs.fileattr & ATTR_FILE_ALLOCSIZE) { ab += *(off_t*)p; p += 8; } if (rs.fileattr & ATTR_FILE_DATALENGTH) { lb += *(off_t*)p; p += 8; } }
            p0 += len;
        }
        if (r == 0) break;
        opts &= ~SRCHFS_START;
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    printf("searchfs %s returned_attrs=%d %.3f s (cpu sys %.2f)  files=%llu dirs=%llu calls=%llu ebusy_restarts=%llu alloc=%.2f GiB logical=%.2f GiB  %.0f entries/s\n",
        vol, want_returned, s, ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6, files, dirs, calls, busy, ab / 1073741824.0, lb / 1073741824.0, (files + dirs) / s);
    return 0;
}
