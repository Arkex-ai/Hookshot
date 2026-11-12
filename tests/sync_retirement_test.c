/* The actual object table, collector and link/unlink paths are inserted by test_sync_retirement.py.
   Fake monotonic time avoids a real 30-second wait. No game memory or running game is used. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
typedef pthread_mutex_t SRWLOCK;
typedef pthread_cond_t CONDITION_VARIABLE;
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
static void AcquireSRWLockExclusive(SRWLOCK *l){if(pthread_mutex_lock(l))abort();}
static void ReleaseSRWLockExclusive(SRWLOCK *l){if(pthread_mutex_unlock(l))abort();}
static void AcquireSRWLockShared(SRWLOCK *l){if(pthread_mutex_lock(l))abort();}
static void ReleaseSRWLockShared(SRWLOCK *l){if(pthread_mutex_unlock(l))abort();}
static void InitializeSRWLock(SRWLOCK *l){*l=(pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;}
static void InitializeConditionVariable(CONDITION_VARIABLE *c){*c=(pthread_cond_t)PTHREAD_COND_INITIALIZER;}
#endif
typedef struct HosThread {int unused;} HosThread;
typedef struct {uint64_t x[8];} Ctx;
#define HLE(n) static void n(Ctx *c)
#define ARG(n) (c->x[n])
static int64_t clock_ns;
static int keep_dead;
static int64_t hos_now_ns(void){return clock_ns;}
static int env_on(const char *s){return !strcmp(s,"HOS_KEEP_DEAD_OBJECTS")&&keep_dead;}
static void hos_vcore_checkpoint(void){}
static int assertions,failures;
static void check(int ok,const char *why){assertions++;if(!ok){failures++;fprintf(stderr,"FAIL %s\n",why);}}
/* Teardown can free the fixture's allocations even when the baseline deliberately loses a record.
   Tracking is outside the production algorithm and does not make orphaned objects reachable by it. */
static struct {void *p;size_t n;} allocations[16384];
static size_t allocation_n;
static void remember(void *p,size_t n){if(!p||allocation_n==16384)abort();allocations[allocation_n].p=p;allocations[allocation_n++].n=n;}
static void *counted_malloc(size_t n){void*p=malloc(n);remember(p,n);return p;}
static void *counted_calloc(size_t n,size_t z){void*p=calloc(n,z);remember(p,n*z);return p;}
static void counted_free(void *p){if(!p)return;size_t i=0;while(i<allocation_n&&allocations[i].p!=p)i++;if(i==allocation_n)abort();allocations[i]=allocations[--allocation_n];free(p);}
static void *counted_realloc(void *p,size_t n){if(!p)return counted_malloc(n);size_t i=0;while(i<allocation_n&&allocations[i].p!=p)i++;if(i==allocation_n)abort();void*q=realloc(p,n);if(!q)abort();allocations[i].p=q;allocations[i].n=n;return q;}
#define malloc counted_malloc
#define calloc counted_calloc
#define realloc counted_realloc
#define free counted_free
@TABLE@
static SRWLOCK mw_lock=SRWLOCK_INIT;
enum {H_EVENT,H_TIMER,H_THREAD,H_SEMA,H_QUEUE_NOT_EMPTY,H_QUEUE_NOT_FULL,H_NATIVE};
static void mw_watch_add_locked(uint64_t k){(void)k;}
static void mw_watch_remove_locked(uint64_t k){(void)k;}
@HOLDER_WATCH@
@LINK@
@UNLINK@
@UNLINK_ALL@
#undef malloc
#undef calloc
#undef realloc
#undef free
static int queued(Obj *o){int n=0;for(size_t i=0;i<grave_count;i++)n+=graves[(grave_head+i)%grave_cap]==o;return n;}
static int spare(Obj *o){int n=0;size_t steps=0;for(Obj*p=spares;p;p=p->spare){n+=p==o;if(++steps>16384)abort();}return n;}
static void tick(void){Obj*o=counted_calloc(1,sizeof *o);AcquireSRWLockExclusive(&table_lock);bury_locked(o);ReleaseSRWLockExclusive(&table_lock);}
static uint64_t collide(uint64_t key){uint64_t next=key+1;while(slot_of(next,cap)!=slot_of(key,cap))next++;return next;}
static void linked_case(int retire_holder){
 const uint64_t mk=0x1000,hk=0x2000;
 Obj *mw=fresh(mk,K_MULTIWAIT),*h=fresh(hk,K_HOLDER);
 h->sem_max=H_NATIVE;Ctx c={{mk,hk}};
 _ZN2nn2os19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE(&c);
 check(mw->nlinks==1&&mw->links[0]==h&&h->mw==mw,"production Link establishes both references");
 Obj *retired=retire_holder?h:mw;uint64_t key=retire_holder?hk:mk;
 drop(key);Obj*replacement=fresh(collide(key),K_EVENT);
 check(replacement!=retired&&queued(retired)==1,"actual table collision retires the finalized object");
 clock_ns=30000000000LL;tick();
 check(queued(retired)==1&&!spare(retired),"exact 30-second boundary remains protected");
 clock_ns++;tick();
 check(queued(retired)==1&&!spare(retired),"linked retirement remains pending beyond its first deadline");
 check(mw->nlinks==1&&mw->links[0]==h&&h->mw==mw,"collector does not clear live links");
 if(retire_holder){c.x[0]=mk;_ZN2nn2os24UnlinkAllMultiWaitHolderEPNS0_13MultiWaitTypeE(&c);}
 else {c.x[0]=hk;_ZN2nn2os21UnlinkMultiWaitHolderEPNS0_19MultiWaitHolderTypeE(&c);}
 check(mw->nlinks==0&&h->mw==NULL,"actual unlink path removes the blocker");
 clock_ns+=31000000000LL;tick();
 check(queued(retired)==0&&spare(retired)==1,"unlinked retirement becomes reusable exactly once");
 if(!retire_holder)check(retired->links==NULL,"retired multiwait releases its real link buffer");
 int reused=0;for(uint64_t k=0x500000;k<0x500010;k++){Obj *o=fresh(k,K_MUTEX);reused|=o==retired;}
 check(reused,"production allocation reuses the retired object");
 check(spare(retired)==0,"reused object is no longer on spare list");
}
static void retry_and_growth(void){
 enum {N=1100};Obj anchor={0};Obj*held[N];
 AcquireSRWLockExclusive(&table_lock);
 for(int i=0;i<N;i++){held[i]=counted_calloc(1,sizeof *held[i]);held[i]->mw=&anchor;bury_locked(held[i]);}
 ReleaseSRWLockExclusive(&table_lock);
 check(grave_count==N&&grave_cap>=N,"retirement ring grows with all records retained");
 int intact=1;
 for(int k=0;k<100;k++){
  clock_ns+=31000000000LL;tick();
  for(int i=0;i<N;i++)if(queued(held[i])!=1||spare(held[i]))intact=0;
 }
 check(intact,"100 retry passes preserve each linked record once");
 check(grave_count==N+1,"retrying does not append duplicate records");
 for(int i=0;i<N;i++)held[i]->mw=NULL;
 clock_ns+=31000000000LL;tick();
 int reusable=1;for(int i=0;i<N;i++)if(queued(held[i])||spare(held[i])!=1)reusable=0;
 check(reusable,"all 1100 released blockers recycle after ring wrapping");
}
static void follower_and_payload(void){
 Obj anchor={0};Obj *held=counted_calloc(1,sizeof *held),*ready=counted_calloc(1,sizeof *ready);
 held->mw=&anchor;ready->q=counted_calloc(1024,sizeof *ready->q);ready->links=counted_calloc(8,sizeof *ready->links);
 AcquireSRWLockExclusive(&table_lock);bury_locked(held);bury_locked(ready);ReleaseSRWLockExclusive(&table_lock);
 clock_ns=31000000000LL;tick();
 check(queued(held)==1&&!spare(held),"blocked head is retained");
 check(spare(ready)==1&&!ready->q&&!ready->links,"blocked head does not starve eligible payload cleanup");
 held->mw=NULL;clock_ns+=31000000000LL;tick();check(spare(held)==1,"head can recycle on a later pass");
 clock_ns+=31000000000LL;tick();check(spare(held)==1&&spare(ready)==1,"subsequent collection does not recycle twice");
}
static void kept_case(void){
 keep_dead=1;Obj *o=counted_calloc(1,sizeof *o);o->q=counted_calloc(32,sizeof *o->q);
 AcquireSRWLockExclusive(&table_lock);bury_locked(o);ReleaseSRWLockExclusive(&table_lock);
 clock_ns=100000000000LL;tick();
 check(!grave_count&&!spares&&o->q,"HOS_KEEP_DEAD_OBJECTS preserves explicit opt-out");
}
int main(int argc,char**argv){
 if(argc!=2)return 90;
 if(!strcmp(argv[1],"holder"))linked_case(1);
 else if(!strcmp(argv[1],"multiwait"))linked_case(0);
 else if(!strcmp(argv[1],"growth"))retry_and_growth();
 else if(!strcmp(argv[1],"follower"))follower_and_payload();
 else if(!strcmp(argv[1],"keep"))kept_case();
 else return 91;
 printf("case=%s assertions=%d failures=%d\n",argv[1],assertions,failures);
 while(allocation_n)free(allocations[--allocation_n].p);
 return failures?2:0;
}
