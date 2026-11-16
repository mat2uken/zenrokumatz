/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <libusb.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include "torne_core.h"
#include "torne_dib.h"
#include "card_server.h"
#include "ts_stats.h"

#define TRANSFER_BYTES 61440
#define TRANSFERS 8
#define DEFAULT_BUFFER (8u*1024*1024)
typedef struct App App;
typedef struct {struct libusb_transfer *transfer;App *app;atomic_int active;} Slot;
struct App {
    libusb_context *usb;
    libusb_device_handle *handle;
    struct torne_bridge bridge;
    struct torne_dib dib;
    CardServer *card_service;
    struct torne_framer framer;
    TsStats raw_stats;
    pthread_mutex_t lock,submit_lock;
    pthread_cond_t available;
    uint8_t *queue;
    size_t capacity,read_at,write_at,used,highwater;
    int producer_done,claimed,worker_started,stream_started,dib_initialized,output_flags;
    atomic_int stop,error,inflight,receiver_stopping;
    Slot slots[TRANSFERS];
    uint64_t received,written,transfers,overflows;
    uint64_t framed_input,framed_output,resync_bytes;
    double max_callback_gap,last_callback,max_write_wait;
    pthread_t worker;
};
static volatile sig_atomic_t requested_stop;
static void signal_stop(int sig){(void)sig;requested_stop=1;}
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void fail(App *a,const char *where,int status){
    if(!atomic_exchange(&a->error,1))fprintf(stderr,"ERROR stage=%s status=%d\n",where,status);
    atomic_store(&a->stop,1);
}
static int usb_error(int r){
    if(r==LIBUSB_ERROR_NO_DEVICE)return -ENODEV;
    if(r==LIBUSB_ERROR_TIMEOUT)return -ETIMEDOUT;
    if(r==LIBUSB_ERROR_BUSY)return -EBUSY;
    return -EIO;
}
static int control(void *p,u8 type,u16 value,u16 index,void *data,u16 length){
    App *a=p;int r=libusb_control_transfer(a->handle,type,0xbc,value,index,data,length,1000);
    return r<0?usb_error(r):r;
}
static int bulk(void *p,u8 endpoint,void *data,int length,unsigned timeout){
    App *a=p;int n=0,r=libusb_bulk_transfer(a->handle,endpoint,data,length,&n,timeout);
    return (r==0||(r==LIBUSB_ERROR_TIMEOUT&&n>0))?n:r<0?usb_error(r):n;
}
static void sleep_ms(void *p,unsigned ms){(void)p;struct timespec t={ms/1000,(long)(ms%1000)*1000000};nanosleep(&t,NULL);}
static u64 now_ms(void *p){(void)p;return (u64)(seconds()*1000);}
static void core_log(void *p,int level,const char *format,...){
    (void)p;if(level>TORNE_LOG_ERR)return;
    va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);fputc('\n',stderr);
}
static const struct torne_ops usb_ops={control,bulk,sleep_ms,now_ms,core_log};

static int wait_for_lock(App *a,int channel,struct torne_dib_status *status){
    double deadline=seconds()+5,retry_at=seconds()+1;int r,retried=0;
    do{
        r=torne_dib_status(&a->dib,status);if(r||status->locked)break;
        /* Like coax/dvb-core, retry an unsuccessful initial search after one second. */
        if(!retried&&!requested_stop&&seconds()>=retry_at){
            fprintf(stderr,"RETUNE channel=%d mask=%x tune_status=%d tune_state=%d\n",channel,status->status,a->dib.status,a->dib.tune_state);
            retried=1;r=torne_dib_tune(&a->dib,473142857u+6000000u*(unsigned)(channel-13));if(r)break;
            r=torne_dib_status(&a->dib,status);if(r||status->locked)break;
        }
        sleep_ms(a,50);
    }while(seconds()<deadline&&!requested_stop);
    return r;
}

static int output(App *a,const uint8_t *data,size_t n){
    double started=seconds();
    while(n&&!atomic_load(&a->stop)&&!requested_stop){
        struct pollfd p={STDOUT_FILENO,POLLOUT,0};int r=poll(&p,1,100);
        if(r<0&&errno==EINTR)continue;
        if(r<0)return -1;
        if(!r)continue;
        ssize_t w=write(STDOUT_FILENO,data,n);
        if(w<0&&(errno==EINTR||errno==EAGAIN))continue;
        if(w<0&&errno==EPIPE){fprintf(stderr,"STOP reason=consumer_closed\n");atomic_store(&a->stop,1);return 0;}
        if(w<=0)return -1;
        data+=w;n-=(size_t)w;a->written+=(uint64_t)w;
    }
    double elapsed=seconds()-started;
    if(elapsed>a->max_write_wait)a->max_write_wait=elapsed;
    if(elapsed>0.1)fprintf(stderr,"OUTPUT_WAIT mono=%.6f seconds=%.6f\n",seconds(),elapsed);
    return 0;
}
static void *consume(void *p){
    App *a=p;uint8_t raw[TRANSFER_BYTES],ts[TRANSFER_BYTES+188];
    for(;;){
        pthread_mutex_lock(&a->lock);
        while(!a->used&&!a->producer_done&&!atomic_load(&a->stop))pthread_cond_wait(&a->available,&a->lock);
        if(atomic_load(&a->stop)||(!a->used&&a->producer_done)){pthread_mutex_unlock(&a->lock);break;}
        size_t n=a->used<sizeof(raw)?a->used:sizeof(raw),first=a->capacity-a->read_at;
        if(first>n)first=n;
        memcpy(raw,a->queue+a->read_at,first);memcpy(raw+first,a->queue,n-first);
        a->read_at=(a->read_at+n)%a->capacity;a->used-=n;
        pthread_mutex_unlock(&a->lock);
        size_t count=torne_framer_feed(&a->framer,raw,n,ts);
        a->framed_input+=n;a->framed_output+=count;
        uint64_t discarded=a->framed_input-a->framed_output/188*192-a->framer.len;
        if(discarded>a->resync_bytes){fprintf(stderr,"TS_RESYNC mono=%.6f bytes=%" PRIu64 "\n",seconds(),discarded-a->resync_bytes);a->resync_bytes=discarded;}
        ts_stats_feed(&a->raw_stats,ts,count);
        if(output(a,ts,count)){fail(a,"output",errno);break;}
    }
    return NULL;
}
static void LIBUSB_CALL completed(struct libusb_transfer *transfer){
    Slot *s=transfer->user_data;App *a=s->app;s->active=0;
    if(transfer->status==LIBUSB_TRANSFER_CANCELLED)goto done;
    if(transfer->status!=LIBUSB_TRANSFER_COMPLETED){fail(a,"usb_transfer",transfer->status);goto done;}
    double t=seconds();if(a->last_callback&&t-a->last_callback>a->max_callback_gap)a->max_callback_gap=t-a->last_callback;a->last_callback=t;
    size_t n=(size_t)transfer->actual_length;
    if(n){
        pthread_mutex_lock(&a->lock);
        if(n>a->capacity-a->used){a->overflows++;pthread_mutex_unlock(&a->lock);fail(a,"receive_queue_full",-ENOBUFS);goto done;}
        size_t first=a->capacity-a->write_at;if(first>n)first=n;
        memcpy(a->queue+a->write_at,transfer->buffer,first);memcpy(a->queue,transfer->buffer+first,n-first);
        a->write_at=(a->write_at+n)%a->capacity;a->used+=n;if(a->used>a->highwater)a->highwater=a->used;
        a->received+=n;a->transfers++;pthread_cond_signal(&a->available);pthread_mutex_unlock(&a->lock);
    }
    pthread_mutex_lock(&a->submit_lock);
    if(!atomic_load(&a->stop)&&!atomic_load(&a->receiver_stopping)&&!requested_stop){
        int r=libusb_submit_transfer(transfer);if(r)fail(a,"usb_resubmit",r);else{s->active=1;a->inflight++;}
    }
    pthread_mutex_unlock(&a->submit_lock);
done:
    a->inflight--;
}
static int start_receiver(App *a){
    for(unsigned i=0;i<TRANSFERS;i++){
        Slot *s=&a->slots[i];s->app=a;s->transfer=libusb_alloc_transfer(0);if(!s->transfer)return -1;
        unsigned char *buffer=malloc(TRANSFER_BYTES);if(!buffer)return -1;
        libusb_fill_bulk_transfer(s->transfer,a->handle,0x81,buffer,TRANSFER_BYTES,completed,s,0);
        int r=libusb_submit_transfer(s->transfer);if(r)return r;
        s->active=1;a->inflight++;
    }
    return 0;
}
static void stop_receiver(App *a){
    pthread_mutex_lock(&a->submit_lock);
    atomic_store(&a->receiver_stopping,1);
    for(unsigned i=0;i<TRANSFERS;i++)if(a->slots[i].active)libusb_cancel_transfer(a->slots[i].transfer);
    pthread_mutex_unlock(&a->submit_lock);
    while(a->inflight){struct timeval t={0,100000};libusb_handle_events_timeout(a->usb,&t);}
    for(unsigned i=0;i<TRANSFERS;i++)if(a->slots[i].transfer){free(a->slots[i].transfer->buffer);libusb_free_transfer(a->slots[i].transfer);}
}
int main(int argc,char **argv){
    int channel=0,probe=0;double duration=0;size_t capacity=DEFAULT_BUFFER;const char *trace_path=NULL,*card_path=NULL;FILE *trace=NULL;unsigned card_commands=0;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--raw"))continue;
        else if(!strcmp(argv[i],"--probe"))probe=1;
        else if(i+1<argc&&!strcmp(argv[i],"--channel"))channel=atoi(argv[++i]);
        else if(i+1<argc&&!strcmp(argv[i],"--duration"))duration=atof(argv[++i]);
        else if(i+1<argc&&!strcmp(argv[i],"--buffer-mib"))capacity=(size_t)atoi(argv[++i])*1024*1024;
        else if(i+1<argc&&!strcmp(argv[i],"--trace"))trace_path=argv[++i];
        else if(i+1<argc&&!strcmp(argv[i],"--card-socket"))card_path=argv[++i];
        else{fprintf(stderr,"usage: torne-c --channel 13..62 [--duration SECONDS] [--raw] [--buffer-mib 1..32] [--card-socket PATH] [--trace FILE]\n       torne-c --probe\n");return 2;}
    }
    if((!probe&&(channel<13||channel>62))||duration<0||capacity<1024*1024||capacity>32u*1024*1024)return 2;
    struct sigaction action={0};action.sa_handler=signal_stop;sigemptyset(&action.sa_mask);sigaction(SIGINT,&action,NULL);sigaction(SIGTERM,&action,NULL);signal(SIGPIPE,SIG_IGN);
    App *a=calloc(1,sizeof(*a));if(!a)return 1;a->output_flags=-1;
    pthread_mutex_init(&a->lock,NULL);pthread_mutex_init(&a->submit_lock,NULL);pthread_cond_init(&a->available,NULL);a->capacity=capacity;
    ts_stats_init(&a->raw_stats,"raw");
    int result=1,r;double started=seconds();
    if(trace_path){int fd=open(trace_path,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0){fail(a,"trace_open",errno);goto cleanup;}trace=fdopen(fd,"w");if(!trace){close(fd);fail(a,"trace_open",errno);goto cleanup;}a->raw_stats.trace=trace;}
    r=libusb_init(&a->usb);if(r){fail(a,"usb_init",r);goto cleanup;}
    a->handle=libusb_open_device_with_vid_pid(a->usb,TORNE_USB_VENDOR,TORNE_USB_PRODUCT);
    if(!a->handle){fail(a,"device_open",-ENODEV);goto cleanup;}
    r=libusb_claim_interface(a->handle,0);if(r){fail(a,"claim_interface",r);goto cleanup;}a->claimed=1;
    r=libusb_set_interface_alt_setting(a->handle,0,0);if(r){fail(a,"interface_setting",r);goto cleanup;}
    torne_bridge_init(&a->bridge,&usb_ops,a);
    r=torne_bridge_start(&a->bridge);if(r){fail(a,"bridge_start",r);goto cleanup;}
    r=torne_bridge_reset_demodulator(&a->bridge);if(r){fail(a,"demod_reset",r);goto cleanup;}
    if(probe){
        const u8 select[]={0x05,0x07,0,3},reg[]={0x03,0x80};u8 value[2];
        r=torne_i2c_write(&a->bridge,0x12,select,sizeof(select));if(!r)r=torne_i2c_write_read(&a->bridge,0x12,reg,2,value,2);
        if(r||value[0]!=1||value[1]!=0xb3){fail(a,"demod_identity",r);goto cleanup;}
        fprintf(stderr,"PROBE device=054c:04b2 demod=01b3\n");result=0;goto cleanup;
    }
    if(card_path){a->card_service=card_server_open(&a->bridge,card_path);if(!a->card_service){fail(a,"card_socket",errno);goto cleanup;}}
    torne_dib_setup(&a->dib,&a->bridge);r=torne_dib_init(&a->dib);if(r){fail(a,"demod_init",r);goto cleanup;}a->dib_initialized=1;
    r=torne_bridge_prepare_stream(&a->bridge);if(r){fail(a,"stream_prepare",r);goto cleanup;}
    r=torne_dib_tune(&a->dib,473142857u+6000000u*(unsigned)(channel-13));if(r){fail(a,"tune",r);goto cleanup;}
    struct torne_dib_status status={0};r=wait_for_lock(a,channel,&status);
    if(requested_stop&&!r){result=0;goto cleanup;}
    if(r||!status.locked){fprintf(stderr,"LOCK_STATUS mask=%x tune_status=%d tune_state=%d strength_x1000=%d\n",status.status,a->dib.status,a->dib.tune_state,status.strength_dbm_x1000);fail(a,"lock",r?r:-ETIMEDOUT);goto cleanup;}
    fprintf(stderr,"TUNED channel=%d cnr_x100=%d startup_s=%.6f mode=raw\n",channel,status.cnr_x100,seconds()-started);
    r=torne_dib_set_output(&a->dib,TORNE_OUT_PARALLEL_GATED);if(r){fail(a,"demod_output",r);goto cleanup;}
    a->queue=malloc(capacity);if(!a->queue){fail(a,"queue_allocate",-ENOMEM);goto cleanup;}
    torne_framer_reset(&a->framer);
    int flags=fcntl(STDOUT_FILENO,F_GETFL);if(flags<0||fcntl(STDOUT_FILENO,F_SETFL,flags|O_NONBLOCK)){fail(a,"output_nonblocking",errno);goto cleanup;}a->output_flags=flags;
    r=pthread_create(&a->worker,NULL,consume,a);if(r){fail(a,"worker_create",r);goto cleanup;}a->worker_started=1;
    r=start_receiver(a);if(r){fail(a,"receive_start",r);goto cleanup;}
    r=torne_bridge_start_stream(&a->bridge);if(r){fail(a,"stream_start",r);goto cleanup;}a->stream_started=1;
    if(a->card_service&&card_server_start(a->card_service)){fail(a,"card_thread",-1);goto cleanup;}
    double stream_start=seconds();fprintf(stderr,"READY queue_bytes=%zu transfers=%u transfer_bytes=%u\n",capacity,TRANSFERS,TRANSFER_BYTES);
    while(!atomic_load(&a->stop)&&!requested_stop&&(!duration||seconds()-stream_start<duration)){
        struct timeval timeout={0,100000};r=libusb_handle_events_timeout(a->usb,&timeout);
        if(r&&r!=LIBUSB_ERROR_INTERRUPTED){fail(a,"usb_events",r);break;}
    }
    result=atomic_load(&a->error)?1:0;
cleanup:
    if(a->usb)stop_receiver(a);
    pthread_mutex_lock(&a->lock);a->producer_done=1;if(result||requested_stop)atomic_store(&a->stop,1);pthread_cond_signal(&a->available);pthread_mutex_unlock(&a->lock);
    if(a->worker_started)pthread_join(a->worker,NULL);
    if(a->card_service){if(a->output_flags>=0){fcntl(STDOUT_FILENO,F_SETFL,a->output_flags);a->output_flags=-1;}close(STDOUT_FILENO);card_server_wait_idle(a->card_service,5000);card_commands=card_server_commands(a->card_service);card_server_close(a->card_service);}
    if(a->stream_started)torne_bridge_stop_stream(&a->bridge);
    if(a->dib_initialized){torne_dib_set_output(&a->dib,TORNE_OUT_HIGHZ);torne_dib_sleep(&a->dib);}
    ts_stats_print(&a->raw_stats);
    struct rusage usage;getrusage(RUSAGE_SELF,&usage);double cpu=usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1e6+usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1e6;
    fprintf(stderr,"STATS received=%" PRIu64 " written=%" PRIu64 " transfers=%" PRIu64 " queue_peak=%zu queue_capacity=%zu overflows=%" PRIu64 " max_rx_gap_s=%.6f max_write_s=%.6f card_commands=%u wall_s=%.6f cpu_s=%.6f failed=%d\n",a->received,a->written,a->transfers,a->highwater,a->capacity,a->overflows,a->max_callback_gap,a->max_write_wait,card_commands,seconds()-started,cpu,atomic_load(&a->error));
    if(a->output_flags>=0)fcntl(STDOUT_FILENO,F_SETFL,a->output_flags);
    if(trace)fclose(trace);
    if(a->claimed)libusb_release_interface(a->handle,0);if(a->handle)libusb_close(a->handle);if(a->usb)libusb_exit(a->usb);
    result=atomic_load(&a->error)?1:result;free(a->queue);pthread_cond_destroy(&a->available);pthread_mutex_destroy(&a->submit_lock);pthread_mutex_destroy(&a->lock);free(a);return result;
}
