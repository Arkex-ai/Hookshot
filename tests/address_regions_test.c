/* Address-region lifecycle regression. Production functions are inserted by test_address_regions.py.
   All addresses are synthetic; commit/discard are controlled stand-ins. No game allocation occurs.
   Windows uses native SRW locks/events/threads; the POSIX equivalents below control the same interleavings. */

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#ifndef POSIX_TEST_SUPPORT_H
#define POSIX_TEST_SUPPORT_H
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
typedef unsigned long DWORD;
typedef long LONG;
#define WINAPI
#define TRUE 1
#define FALSE 0
#define WAIT_OBJECT_0 0UL
#define WAIT_TIMEOUT 258UL
#define ExitProcess(n) exit(n)
#define InterlockedIncrement(p) __atomic_add_fetch((p),1,__ATOMIC_SEQ_CST)
#define InterlockedExchange(p,n) __atomic_exchange_n((p),(n),__ATOMIC_SEQ_CST)
static int TryAcquireSRWLockExclusive(pthread_mutex_t *m) { return pthread_mutex_trylock(m)==0; }
static void ReleaseSRWLockExclusive(pthread_mutex_t *m) { if(pthread_mutex_unlock(m)) abort(); }
typedef struct TestHandle {
    int kind, signaled;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    DWORD (*fn)(void *);
    void *arg;
} *HANDLE;
static HANDLE new_handle(int kind) {
    HANDLE h=calloc(1,sizeof *h);
    if(!h) abort();
    h->kind=kind;
    if(pthread_mutex_init(&h->mutex,NULL)||pthread_cond_init(&h->cond,NULL)) abort();
    return h;
}
static HANDLE CreateEventW(void *security,int manual,int initial,const void *name) {
    (void)security;(void)name;
    if(!manual) abort();
    HANDLE h=new_handle(1);h->signaled=initial;return h;
}
static int SetEvent(HANDLE h) {
    if(pthread_mutex_lock(&h->mutex)) abort();
    h->signaled=1;
    if(pthread_cond_broadcast(&h->cond)||pthread_mutex_unlock(&h->mutex)) abort();
    return 1;
}
static void *thread_entry(void *arg) {
    HANDLE h=arg; h->fn(h->arg); SetEvent(h); return NULL;
}
static HANDLE CreateThread(void *security,size_t stack,DWORD (*fn)(void *),void *arg,DWORD flags,void *id) {
    (void)security;(void)stack;(void)flags;(void)id;
    HANDLE h=new_handle(2);h->fn=fn;h->arg=arg;
    if(pthread_create(&h->thread,NULL,thread_entry,h)) abort();
    return h;
}
static DWORD WaitForSingleObject(HANDLE h,DWORD ms) {
    struct timespec deadline;
    if(clock_gettime(CLOCK_REALTIME,&deadline)) abort();
    deadline.tv_sec+=(time_t)(ms/1000);
    deadline.tv_nsec+=(long)(ms%1000)*1000000;
    if(deadline.tv_nsec>=1000000000){deadline.tv_sec++;deadline.tv_nsec-=1000000000;}
    if(pthread_mutex_lock(&h->mutex)) abort();
    int rc=0;
    while(!h->signaled && !rc)rc=pthread_cond_timedwait(&h->cond,&h->mutex,&deadline);
    int signaled=h->signaled;
    if(pthread_mutex_unlock(&h->mutex)) abort();
    if(rc && rc!=ETIMEDOUT)abort();
    return signaled ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
static int CloseHandle(HANDLE h) {
    if(h->kind==2 && pthread_join(h->thread,NULL)) abort();
    if(pthread_cond_destroy(&h->cond)||pthread_mutex_destroy(&h->mutex)) abort();
    free(h);return 1;
}
#endif

#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HOS_HEAP_BASE 0x1000000000ULL
#define HOS_SPACE_END 0x8000000000ULL
#if defined(_WIN32)
static SRWLOCK lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&lock)
#define UNLOCK() ReleaseSRWLockExclusive(&lock)
#else
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() do { if (pthread_mutex_lock(&lock)) abort(); } while (0)
#define UNLOCK() do { if (pthread_mutex_unlock(&lock)) abort(); } while (0)
#endif
static uint64_t hos_heap_size;
static uint64_t low_hole[2][2];
static uint64_t pages_used[(1ULL << 32) / 0x1000 / 64];
@RANGE@
static Range *regions;
static int nregions, capregions;
static uint64_t region_floor = ~0ULL;
static HANDLE entered, release_discard;
static uint64_t blocked_addr, payload_addr;
static volatile LONG block_discard, discard_calls, payload;
static void hos_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
static void hos_fatal(void *ctx, const char *msg, unsigned long long a, unsigned long long b) {
    (void)ctx; (void)msg; (void)a; (void)b; fputs("unexpected address exhaustion\n",stderr); ExitProcess(90);
}

static int callback_lock_ok;
static int reenter_discard;
static uint64_t reentrant_addr;

uint64_t hos_region_alloc(uint64_t size, uint64_t align);
static void discard_pages(uint64_t addr, uint64_t size) {
    callback_lock_ok = TryAcquireSRWLockExclusive(&lock);
    if (callback_lock_ok) ReleaseSRWLockExclusive(&lock);
    if (reenter_discard && callback_lock_ok) { reenter_discard=0; reentrant_addr=hos_region_alloc(0x10000,0x10000); }
    (void)size; InterlockedIncrement(&discard_calls);
    if (block_discard && addr == blocked_addr) {
        SetEvent(entered);
        if (WaitForSingleObject(release_discard,10000)!=WAIT_OBJECT_0) ExitProcess(91);
        if (payload_addr==addr) InterlockedExchange(&payload,0);
    }
}

@FUNCTION_0@

@FUNCTION_1@

@FUNCTION_2@

@FUNCTION_3@


#define HOS_SYSTEM_BASE 0x01000000ULL
#define HOS_SYSTEM_END 0x07000000ULL
static uint64_t sys_next=HOS_SYSTEM_BASE;
static uint64_t hos_image_base=0x80000000ULL, hos_image_end=0x81000000ULL;
static int hos_guest_range(uint64_t a,uint64_t n) {return a<HOS_SPACE_END && n<=HOS_SPACE_END-a;}
enum { HOS_MEM_IMAGE=1,HOS_MEM_SYSTEM,HOS_MEM_HEAP,HOS_MEM_REGION };

@FUNCTION_4@
@FUNCTION_5@

@FUNCTION_6@
static int failures, assertions;
static void check(int ok, const char *name) {
    assertions++;
    if (!ok) { failures++; printf("FAIL %s\n",name); }
}

static HANDLE read_started, read_done;
static int read_kind, read_fits;
static uint64_t read_floor;
static DWORD WINAPI reader_worker(void *unused) {
    (void)unused;SetEvent(read_started);
    if (read_kind==1) read_floor=hos_region_floor();
    else read_fits=hos_heap_size_fits(HOS_SPACE_END-HOS_HEAP_BASE);
    SetEvent(read_done);return 0;
}
static void test_floor_read_serialization(void) {
    for (read_kind=1;read_kind<=2;read_kind++) {
        read_started=CreateEventW(NULL,TRUE,FALSE,NULL);read_done=CreateEventW(NULL,TRUE,FALSE,NULL);
        if (!read_started || !read_done) ExitProcess(95);
        LOCK();region_floor=HOS_SPACE_END-0x10000;
        HANDLE thread=CreateThread(NULL,0,reader_worker,NULL,0,NULL);
        if (!thread || WaitForSingleObject(read_started,10000)!=WAIT_OBJECT_0) ExitProcess(96);
        DWORD waited=WaitForSingleObject(read_done,150);
        check(waited==WAIT_TIMEOUT,read_kind==1 ? "floor accessor waits for allocator lock":"heap bound read waits for allocator lock");
        region_floor=~0ULL;UNLOCK();
        if (WaitForSingleObject(thread,10000)!=WAIT_OBJECT_0) ExitProcess(97);
        check(read_kind==1 ? read_floor==~0ULL : read_fits,
            read_kind==1 ? "floor accessor sees protected update":"heap bound sees protected update");
        CloseHandle(thread);CloseHandle(read_started);CloseHandle(read_done);
    }
}

static void reset(void) {
    free(regions);regions=NULL;nregions=capregions=0;region_floor=~0ULL;
    hos_heap_size=0;memset(low_hole,0,sizeof low_hole);memset(pages_used,0,sizeof pages_used);
    block_discard=0;discard_calls=0;payload_addr=0;payload=0;blocked_addr=0;
}
static DWORD WINAPI free_worker(void *p) { hos_region_free((uint64_t)(uintptr_t)p);return 0; }
int main(int argc, char **argv) {
    const uint64_t size=0x10000, full=HOS_SPACE_END-HOS_HEAP_BASE;

    if (argc==2 && !strcmp(argv[1],"--poison")) {
        reset();capregions=8;regions=(Range *)malloc((size_t)capregions*sizeof(Range));
        memset(regions,0x7f,(size_t)capregions*sizeof(Range));
        uint64_t x=hos_region_alloc(size,size);hos_region_free(x);
        check(nregions==0,"known nonzero prior storage must not skip finalization");
        printf("assertions=%d failures=%d\n",assertions,failures);
        free(regions);return failures ? 2 : 0;
    }
    reset();
    check(hos_heap_size_fits(full),"empty window permits full heap");
    uint64_t a=hos_region_alloc(size,size);
    check(!hos_heap_size_fits(full),"live top region blocks heap");
    hos_region_free(a);
    check(nregions==0,"single free removes region");
    check(hos_heap_size_fits(full),"free restores heap capacity");
    hos_region_free(a);hos_region_free(0);
    check(nregions==0 && discard_calls==1,"unknown or repeated free is harmless");
    reset();
    a=hos_region_alloc(size,size);
    uint64_t b=hos_region_alloc(size,size);
    check(b<a,"descending allocator");
    hos_region_free(b);
    check(hos_heap_size_fits(a-HOS_HEAP_BASE),"lowest freed region raises floor to next live region");
    check(!hos_heap_size_fits(a-HOS_HEAP_BASE+size),"next live region still blocks heap");
    uint64_t c=hos_region_alloc(size,size);
    check(c==b,"completed free permits address reuse");
    hos_region_free(a);
    check(!hos_heap_size_fits(c-HOS_HEAP_BASE+size),"freeing a higher region preserves lowest bound");
    hos_region_free(c);
    check(hos_heap_size_fits(full),"last free resets floor");
    // Force the allocator's existing below-heap fallback without allocating any guest bytes.
    reset();hos_heap_size=full;low_hole[1][0]=0x80000000ULL;low_hole[1][1]=0x80040000ULL;
    a=hos_region_alloc(size,size);
    check(a<HOS_HEAP_BASE,"low-hole region allocated");
    check(hos_heap_size_fits(full),"low-hole region must not clamp heap");
    hos_region_free(a);
    check(hos_heap_size_fits(full),"low-hole free keeps heap boundary");
    reset();entered=CreateEventW(NULL,TRUE,FALSE,NULL);release_discard=CreateEventW(NULL,TRUE,FALSE,NULL);
    if (!entered || !release_discard) return 92;
    a=hos_region_alloc(size,size);blocked_addr=a;block_discard=1;
    HANDLE thread=CreateThread(NULL,0,free_worker,(void *)(uintptr_t)a,0,NULL);
    if (!thread || WaitForSingleObject(entered,10000)!=WAIT_OBJECT_0) return 93;
    check(hos_mem_region(a,size)==0,"retiring region is refused by bridge access classifier");

    check(hos_regions(NULL,0)==0,"pending-only table reports no live resource regions");
    uint64_t single[2][2]={{0x1234,0x5678},{0x9abc,0xdef0}};
    check(hos_regions(single,1)==0 && single[0][0]==0x1234 && single[0][1]==0x5678,
          "pending-only enumeration leaves caller buffer untouched");

    b=hos_region_alloc(size,size);
    payload_addr=b;InterlockedExchange(&payload,0x5a);
    check(b!=a,"discarding region cannot be reused concurrently");
    check(!hos_heap_size_fits(full),"retiring region remains a heap boundary");
    // Table movement/reallocation must not leave finalization with a stale Range pointer.
    for (int k=0;k<90;k++) hos_region_alloc(size,size);

    uint64_t view[128][2];memset(view,0xa5,sizeof view);
    int count=hos_regions(view,3);
    check(count==91,"truncated enumeration returns total live count, not slots written");
    check(view[0][0]==b && view[0][1]==b+size,"enumeration skips a retiring leading entry");
    check(view[1][0]==b-size && view[2][0]==b-2*size,"enumeration is compact and descending");
    check(view[3][0]==0xa5a5a5a5a5a5a5a5ULL && view[3][1]==0xa5a5a5a5a5a5a5a5ULL,
          "enumeration never writes beyond caller capacity");
    check(hos_regions(NULL,0)==91,"zero-capacity count includes all live regions");
    count=hos_regions(view,128);
    int pending_seen=0;
    for(int k=0;k<count && k<128;k++) if(view[k][0]==a) pending_seen=1;
    check(count==91 && !pending_seen,"complete enumeration excludes pending retirement");

    SetEvent(release_discard);
    if (WaitForSingleObject(thread,10000)!=WAIT_OBJECT_0) return 94;
    CloseHandle(thread);CloseHandle(entered);CloseHandle(release_discard);block_discard=0;
    check(payload==0x5a,"old discard does not erase new owner's payload");
    check(nregions==91,"exactly retiring entry removed despite table growth");
    check(regions[0].addr==b,"highest surviving entry is new allocation");
    c=hos_region_alloc(size,size);
    check(c==a,"address becomes reusable only after discard");
    hos_region_free(c);
    check(nregions==91,"reused entry can itself be finalized");

    check(callback_lock_ok,"discard callback runs outside region lock");
    // Reused table storage may have nonzero bytes: initialize the new per-entry lifecycle field.
    reset();capregions=8;regions=(Range *)malloc((size_t)capregions*sizeof(Range));
    memset(regions,0x7f,(size_t)capregions*sizeof(Range));
    a=hos_region_alloc(size,size);hos_region_free(a);
    check(nregions==0,"new entry clears previous releasing bytes");
    reset();a=hos_region_alloc(size,size);reentrant_addr=0;reenter_discard=1;
    hos_region_free(a);
    check(callback_lock_ok,"callback lock remains available on reentry");
    check(reentrant_addr && reentrant_addr!=a,"reentrant allocation cannot reuse retiring address");
    check(nregions==1 && regions[0].addr==reentrant_addr,"reentrant allocation survives outer finalizer");
    hos_region_free(reentrant_addr);
    check(nregions==0 && hos_heap_size_fits(full),"reentrant lifecycle returns to empty window");
    // A remaining low-hole entry must not be mistaken for a high address-region boundary.
    reset();hos_heap_size=full;low_hole[1][0]=0x80000000ULL;low_hole[1][1]=0x80040000ULL;
    a=hos_region_alloc(size,size);b=hos_region_alloc(size,size);hos_region_free(a);
    check(nregions==1 && hos_heap_size_fits(full),"retained low-hole entry does not clamp heap after another free");
    hos_region_free(b);
    // Sequential churn must not grow the metadata table without a simultaneous live consumer.
    reset();
    for(int k=0;k<10000;k++) { uint64_t x=hos_region_alloc(size,size); hos_region_free(x); }
    check(nregions==0 && capregions<=64,"10000 cycles retain no live region and bounded metadata capacity");
    check(hos_heap_size_fits(full),"10000 cycles recover all address capacity");

    test_floor_read_serialization();
    // The old allocator accepts an empty reservation; adding lifecycle state must not strand it.
    reset();a=hos_region_alloc(0,size);hos_region_free(a);
    check(nregions==0,"empty reservation can still be finalized");
    check(hos_heap_size_fits(full),"empty reservation leaves no stale boundary");
    // Every retained interval is explicit; no fake physical-memory savings are measured.
    printf("assertions=%d failures=%d regions=%d discard_calls=%ld\n",assertions,failures,nregions,discard_calls);
    free(regions);regions=NULL;
    return failures ? 2 : 0;
}
