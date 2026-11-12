/* Actual pull()/mf_av1_decode() are inserted by test_mf_buffer_lifetime.py.
   Deterministic HRESULT injection never exhausts RAM or dereferences a failed lock. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int32_t HRESULT;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int64_t LONGLONG;
typedef uint8_t BYTE;
#define SUCCEEDED(x) ((HRESULT)(x)>=0)
#define FAILED(x) ((HRESULT)(x)<0)
#define S_OK ((HRESULT)0)
#define E_FAIL ((HRESULT)-1)
#define MF_E_TRANSFORM_NEED_MORE_INPUT ((HRESULT)-2)
#define MF_E_TRANSFORM_STREAM_CHANGE ((HRESULT)-3)
#define MF_E_NOTACCEPTING ((HRESULT)-4)
#define COINIT_MULTITHREADED 0
#define MFT_MESSAGE_NOTIFY_END_OF_STREAM 1
#define MFT_MESSAGE_COMMAND_DRAIN 2
#define MFT_MESSAGE_COMMAND_FLUSH 3
#define MFT_MESSAGE_NOTIFY_START_OF_STREAM 4
static const int IID_IMF2DBuffer=1;
typedef struct Buffer {int refs,linear,twod;DWORD len;BYTE data[512];} IMFMediaBuffer;
typedef IMFMediaBuffer IMF2DBuffer;
typedef struct {int refs;IMFMediaBuffer *buffer;} IMFSample;
typedef struct {int refs;} IMFCollection;
typedef struct {int unused;} IMFTransform;
typedef struct {DWORD dwStreamID;IMFSample *pSample;DWORD dwStatus;IMFCollection *pEvents;} MFT_OUTPUT_DATA_BUFFER;
@TYPES@
static IMFMediaBuffer buffers[8];static IMFSample samples[8];static int nb,ns;
static IMFSample *pending;static int want_2d,fail_2d,fail_linear,null_pointer,fail_length,fail_add,fail_create_sample,fail_create_buffer;
static LONG output_pitch;static int stale_failure,output_once,submit_calls,output_calls,callbacks,unlocks2d,unlocks_linear,locks2d,locks_linear,bad_unlock,bad_copy,copy_calls,released_locked;
static BYTE input_data[8]={1,2,3,4,5,6,7,8};static int failures,assertions;
static void check(int ok,const char *why){assertions++;if(!ok){failures++;fprintf(stderr,"FAIL %s\n",why);}}
static HRESULT MFCreateSample(IMFSample **p){if(fail_create_sample){*p=NULL;return E_FAIL;}if(ns==8)abort();*p=&samples[ns++];(*p)->refs=1;return S_OK;}
static HRESULT MFCreateMemoryBuffer(DWORD n,IMFMediaBuffer **p){if(fail_create_buffer){*p=NULL;return E_FAIL;}if(nb==8||n>512)abort();*p=&buffers[nb++];(*p)->refs=1;(*p)->len=n;return S_OK;}
static void IMFMediaBuffer_Release(IMFMediaBuffer *b){if(--b->refs<0)abort();if(!b->refs&&(b->linear||b->twod))released_locked++;}
static HRESULT IMFSample_AddBuffer(IMFSample *s,IMFMediaBuffer *b){if(fail_add)return E_FAIL;s->buffer=b;b->refs++;return S_OK;}
static void IMFSample_Release(IMFSample *s){if(--s->refs<0)abort();if(!s->refs&&s->buffer)IMFMediaBuffer_Release(s->buffer);}
static HRESULT IMFSample_GetSampleTime(IMFSample *s,LONGLONG *t){(void)s;*t=123;return S_OK;}
static HRESULT IMFSample_SetSampleTime(IMFSample *s,LONGLONG t){(void)s;(void)t;return S_OK;}
static HRESULT IMFSample_ConvertToContiguousBuffer(IMFSample *s,IMFMediaBuffer **b){if(!s->buffer)return E_FAIL;*b=s->buffer;(*b)->refs++;return S_OK;}
static HRESULT IMFMediaBuffer_GetCurrentLength(IMFMediaBuffer *b,DWORD *n){*n=b->len;return S_OK;}
static HRESULT IMFMediaBuffer_SetCurrentLength(IMFMediaBuffer *b,DWORD n){if(fail_length)return E_FAIL;b->len=n;return S_OK;}
static HRESULT IMFMediaBuffer_QueryInterface(IMFMediaBuffer *b,const int *iid,void **out){(void)iid;if(!want_2d){*out=NULL;return E_FAIL;}*out=b;b->refs++;return S_OK;}
static HRESULT IMF2DBuffer_Lock2D(IMF2DBuffer *b,BYTE **p,LONG *pitch){
 if(fail_2d){if(stale_failure){*p=b->data;*pitch=16;}return E_FAIL;}
 b->twod++;locks2d++;*p=null_pointer?NULL:b->data;*pitch=output_pitch;return S_OK;
}
static HRESULT IMFMediaBuffer_Lock(IMFMediaBuffer *b,BYTE **p,DWORD *maximum,DWORD *length){
 if(fail_linear)return E_FAIL;
 b->linear++;locks_linear++;*p=null_pointer?NULL:b->data;if(maximum)*maximum=sizeof b->data;if(length)*length=b->len;return S_OK;
}
static void IMF2DBuffer_Unlock2D(IMF2DBuffer *b){unlocks2d++;if(!b->twod)bad_unlock++;else b->twod--;}
static void IMFMediaBuffer_Unlock(IMFMediaBuffer *b){unlocks_linear++;if(!b->linear)bad_unlock++;else b->linear--;}
static void IMF2DBuffer_Release(IMF2DBuffer *b){IMFMediaBuffer_Release(b);}
static void IMFCollection_Release(IMFCollection *c){c->refs--;}
static HRESULT IMFTransform_ProcessOutput(IMFTransform *m,DWORD flags,DWORD count,MFT_OUTPUT_DATA_BUFFER *o,DWORD *status){
 (void)m;(void)flags;(void)count;*status=0;output_calls++;
 if(pending){o->pSample=pending;pending=NULL;return S_OK;}
 if(output_once){output_once=0;return S_OK;}
 return MF_E_TRANSFORM_NEED_MORE_INPUT;
}
static HRESULT IMFTransform_ProcessInput(IMFTransform *m,DWORD stream,IMFSample *s,DWORD flags){
 (void)m;(void)stream;(void)flags;submit_calls++;
 check(s->buffer&&s->buffer->len==sizeof input_data,"input submits an attached complete buffer");
 if(s->buffer)check(!memcmp(s->buffer->data,input_data,sizeof input_data),"successful input copies original bytes");
 return S_OK;
}
static HRESULT IMFTransform_ProcessMessage(IMFTransform *m,int message,int value){(void)m;(void)message;(void)value;return S_OK;}
static HRESULT CoInitializeEx(void *reserved,int mode){(void)reserved;(void)mode;return S_OK;}
static void hos_log(const char *fmt,...){(void)fmt;}
static int set_output_nv12(MfAv1 *m){(void)m;return 1;}
static void *checked_copy(void *dst,const void *src,size_t n){
 copy_calls++;int valid=0;for(int i=0;i<nb;i++)if(dst==buffers[i].data&&buffers[i].linear&&n<=sizeof buffers[i].data)valid=1;
 if(!valid){bad_copy++;return dst;}return memcpy(dst,src,n);
}
#define memcpy checked_copy
@PULL@
@DECODE@
#undef memcpy
static void picture(void *user,const uint8_t *y,const uint8_t *uv,int w,int h,int stride,int64_t pts){
 (void)user;callbacks++;int held=0;for(int i=0;i<nb;i++)if(y==buffers[i].data&&(buffers[i].linear||buffers[i].twod))held=1;
 check(held,"picture callback runs only while a successful lock is owned");
 check(w==8&&h==4&&stride==16&&uv==y+64&&pts==12300,"accepted picture geometry and timestamp remain unchanged");
}
static void balanced(void){
 int refs=0,locks=0;for(int i=0;i<nb;i++){refs+=buffers[i].refs;locks+=buffers[i].linear+buffers[i].twod;}for(int i=0;i<ns;i++)refs+=samples[i].refs;
 check(refs==0,"all acquired sample and buffer references are released");check(locks==0,"all successful buffer locks are balanced");
 check(unlocks2d==locks2d && unlocks_linear==locks_linear,"every successful acquisition is unlocked exactly once");check(!bad_unlock,"no failed or absent lock is unlocked");check(!released_locked,"no last buffer reference is released with a lock held");
 check(!bad_copy,"failed input lock is never used for memcpy");
}
static void prepare_picture(void){IMFMediaBuffer *b=NULL;IMFSample*s=NULL;if(FAILED(MFCreateSample(&s))||FAILED(MFCreateMemoryBuffer(96,&b)))abort();if(FAILED(IMFSample_AddBuffer(s,b)))abort();IMFMediaBuffer_Release(b);pending=s;}
int main(int argc,char**argv){
 if(argc!=2)return 90;
 const char *mode=argv[1];MfAv1 m={0};m.w=m.want_w=8;m.h=m.want_h=4;m.stride=16;m.provides_samples=1;m.out_size=96;output_pitch=16;
 if(!strncmp(mode,"output-",7)){
  if(!strcmp(mode,"output-own-add-fail")){m.provides_samples=0;fail_add=1;output_once=1;}
  else {prepare_picture();want_2d=1;
   if(!strcmp(mode,"output-negative"))output_pitch=-16;
   else if(!strcmp(mode,"output-zero"))output_pitch=0;
   else if(!strcmp(mode,"output-null"))null_pointer=1;
   else if(!strcmp(mode,"output-linear"))want_2d=0;
   else if(!strcmp(mode,"output-fallback"))fail_2d=1;
   else if(!strcmp(mode,"output-both-fail")){fail_2d=fail_linear=1;stale_failure=1;}
   else if(strcmp(mode,"output-2d"))return 91;
  }
  pull(&m,picture,NULL);
  int accepted=!strcmp(mode,"output-2d")||!strcmp(mode,"output-linear")||!strcmp(mode,"output-fallback");
  check(callbacks==accepted,"only valid acquired output pictures reach the callback");
  if(!strcmp(mode,"output-own-add-fail"))check(output_calls==0,"failed output attachment is not sent to the transform");
 }else {
  if(!strcmp(mode,"input-lock-fail"))fail_linear=1;
  else if(!strcmp(mode,"input-null"))null_pointer=1;
  else if(!strcmp(mode,"input-length-fail"))fail_length=1;
  else if(!strcmp(mode,"input-add-fail"))fail_add=1;
  else if(!strcmp(mode,"input-sample-fail"))fail_create_sample=1;
  else if(!strcmp(mode,"input-buffer-fail"))fail_create_buffer=1;
  else if(strcmp(mode,"input-ok"))return 92;
  mf_av1_decode(&m,input_data,sizeof input_data,1000,picture,NULL);
  check(submit_calls==!strcmp(mode,"input-ok"),"only successfully acquired and attached inputs are submitted");
  if(fail_linear||null_pointer)check(copy_calls==0,"rejected input lock does not attempt a copy");
 }
 balanced();printf("case=%s assertions=%d failures=%d\n",mode,assertions,failures);return failures?2:0;
}
