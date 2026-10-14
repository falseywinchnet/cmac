// Throwaway probe: parallel getattrlistbulk over a tree, openat from a held parent fd.
#include <sys/attr.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#include <time.h>
#include <sys/resource.h>

typedef struct Dir { int parent_fd; struct Dir* parent; atomic_int refs; char name[256]; int fd; uint32_t expect; } Dir;
static Dir** stack; static size_t top, cap; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER; static int busy; static int nthreads;
static atomic_ullong files, dirs, alloc_bytes, logical_bytes, errs, eperm, calls; static dev_t root_dev; static int use_devid; static int ext; static atomic_ullong purgeable_bytes, sparse_files, shared_files, private_bytes, purgeable_files, dataless;

static void release(Dir* d) { while (d && atomic_fetch_sub(&d->refs, 1) == 1) { Dir* p = d->parent; if (d->fd >= 0) close(d->fd); free(d); d = p; } }
static void push(Dir* d) { pthread_mutex_lock(&mu); if (top == cap) { cap = cap ? cap * 2 : 4096; stack = realloc(stack, cap * sizeof *stack); } stack[top++] = d; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu); }
static Dir* pop(void) { pthread_mutex_lock(&mu); while (top == 0) { if (busy == 0) { pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu); return NULL; } pthread_cond_wait(&cv, &mu); } Dir* d = stack[--top]; busy++; pthread_mutex_unlock(&mu); return d; }
static void done(void) { pthread_mutex_lock(&mu); busy--; if (busy == 0 && top == 0) pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu); }

static void* run(void* unused) {
    size_t bufsize = 256 * 1024; char* buf = malloc(bufsize);
    struct attrlist al; memset(&al, 0, sizeof al); al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR | ATTR_CMN_OBJTYPE | ATTR_CMN_FILEID | (use_devid == 1 ? ATTR_CMN_DEVID : 0);
    al.dirattr = (use_devid == 2 ? ATTR_DIR_MOUNTSTATUS | ATTR_DIR_ENTRYCOUNT : 0);
    al.fileattr = ATTR_FILE_LINKCOUNT | ATTR_FILE_ALLOCSIZE | ATTR_FILE_DATALENGTH;
    if (ext == 3) { al.fileattr = 0; ext = 0; }
    if (ext) { al.commonattr |= ATTR_CMN_FLAGS; al.forkattr = ATTR_CMNEXT_EXT_FLAGS | (ext == 2 ? ATTR_CMNEXT_PRIVATESIZE : 0); }
    for (;;) {
        Dir* d = pop(); if (!d) break;
        int fd = openat(d->parent_fd, d->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) { if (errno == EPERM || errno == EACCES) atomic_fetch_add(&eperm, 1); else atomic_fetch_add(&errs, 1); release(d); done(); continue; }
        d->fd = fd; atomic_fetch_add(&dirs, 1);
        unsigned long long f = 0, ab = 0, lb = 0; uint32_t seen = 0;
        for (;;) {
            int n = getattrlistbulk(fd, &al, buf, bufsize, ext ? FSOPT_ATTR_CMN_EXTENDED : 0); atomic_fetch_add(&calls, 1);
            if (n <= 0) { if (n < 0) atomic_fetch_add(&errs, 1); break; }
            seen += n;
            char* e = buf;
            for (int i = 0; i < n; i++) {
                uint32_t len = *(uint32_t*)e; char* p = e + 4;
                attribute_set_t rs = *(attribute_set_t*)p; p += sizeof(attribute_set_t);
                uint32_t err = 0; if (rs.commonattr & ATTR_CMN_ERROR) { err = *(uint32_t*)p; p += 4; }
                attrreference_t* nr = (attrreference_t*)p; char* name = p + nr->attr_dataoffset; p += sizeof(attrreference_t);
                dev_t dv = root_dev; if (use_devid == 1) { dv = *(dev_t*)p; p += sizeof(dev_t); }
                fsobj_type_t t = *(fsobj_type_t*)p; p += sizeof(fsobj_type_t);
                uint32_t bsdflags = 0; if (rs.commonattr & ATTR_CMN_FLAGS) { bsdflags = *(uint32_t*)p; p += 4; }
                p += 8; // fileid
                if (err) { atomic_fetch_add(&errs, 1); e += len; continue; }
                if (t == VDIR) {
                    uint32_t mnt = 0, cnt = UINT32_MAX;
                    if (rs.dirattr & ATTR_DIR_ENTRYCOUNT) { cnt = *(uint32_t*)p; p += 4; }
                    if (rs.dirattr & ATTR_DIR_MOUNTSTATUS) { mnt = *(uint32_t*)p; p += 4; }
                    if (mnt & DIR_MNTSTATUS_MNTPOINT) dv = 0;
                    if (dv == root_dev) { Dir* c = malloc(sizeof *c); c->parent_fd = fd; c->parent = d; atomic_store(&c->refs, 1); c->fd = -1; c->expect = cnt; strlcpy(c->name, name, sizeof c->name); atomic_fetch_add(&d->refs, 1); push(c); }
                } else {
                    f++;
                    if (rs.fileattr & ATTR_FILE_LINKCOUNT) p += 4;
                    if (rs.fileattr & ATTR_FILE_ALLOCSIZE) { ab += *(off_t*)p; p += 8; }
                    off_t al_ = 0; if (rs.fileattr & ATTR_FILE_DATALENGTH) { lb += *(off_t*)p; p += 8; }
                    if (rs.forkattr & ATTR_CMNEXT_PRIVATESIZE) { atomic_fetch_add(&private_bytes, *(off_t*)p); p += 8; }
                    if (rs.forkattr & ATTR_CMNEXT_EXT_FLAGS) { uint64_t ef = *(uint64_t*)p; p += 8;
                        if (ef & EF_IS_PURGEABLE) atomic_fetch_add(&purgeable_files, 1);
                        if (ef & EF_IS_SPARSE) atomic_fetch_add(&sparse_files, 1);
                        if (ef & EF_MAY_SHARE_BLOCKS) atomic_fetch_add(&shared_files, 1); }
                    if (bsdflags & SF_DATALESS) atomic_fetch_add(&dataless, 1);
                    (void)al_;
                }
                e += len;
            }
            if (seen >= d->expect) break;
        }
        atomic_fetch_add(&files, f); atomic_fetch_add(&alloc_bytes, ab); atomic_fetch_add(&logical_bytes, lb);
        release(d); done();
    }
    free(buf); return NULL;
}
int main(int argc, char** argv) {
    const char* root = argv[1]; nthreads = atoi(argv[2]); use_devid = argc > 3 ? atoi(argv[3]) : 1; ext = argc > 4 ? atoi(argv[4]) : 0;
    struct stat st; lstat(root, &st); root_dev = st.st_dev;
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    Dir* r = malloc(sizeof *r); r->parent_fd = AT_FDCWD; r->parent = NULL; atomic_store(&r->refs, 1); r->fd = -1; r->expect = UINT32_MAX; strlcpy(r->name, root, sizeof r->name); push(r);
    pthread_t th[64]; for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, run, NULL);
    for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    printf("cpu user=%.2f sys=%.2f  ", ru.ru_utime.tv_sec + ru.ru_utime.tv_usec/1e6, ru.ru_stime.tv_sec + ru.ru_stime.tv_usec/1e6);
    printf("bulk threads=%d devid=%d  %.3f s  files=%llu dirs=%llu calls=%llu alloc=%.2f GiB logical=%.2f GiB denied=%llu errors=%llu  %.0f entries/s\n",
        nthreads, use_devid, s, files, dirs, calls, alloc_bytes / 1073741824.0, logical_bytes / 1073741824.0, eperm, errs, (files + dirs) / s);
    if (ext) printf("  purgeable_files=%llu sparse_files=%llu may_share_blocks=%llu dataless=%llu private=%.2f GiB\n", purgeable_files, sparse_files, shared_files, dataless, private_bytes / 1073741824.0);
    return 0;
}
