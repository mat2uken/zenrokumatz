/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "bcas_direct.h"
#include "arib_std_b25.h"
#include "ts_stats.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t stopping;
static void stop(int sig){(void)sig;stopping=1;}
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static int emit(ARIB_STD_B25 *decoder,TsStats *stats){
    ARIB_STD_B25_BUFFER out;int r=decoder->get(decoder,&out);if(r<0)return r;
    ts_stats_feed(stats,out.data,(size_t)out.size);size_t left=(size_t)out.size;const uint8_t *p=out.data;double start=seconds();
    while(left&&!stopping){ssize_t n=write(STDOUT_FILENO,p,left);if(n<0&&errno==EINTR)continue;if(n<0&&errno==EPIPE){stopping=1;return 0;}if(n<=0)return -1;p+=n;left-=(size_t)n;}
    if(seconds()-start>0.1)fprintf(stderr,"B25_OUTPUT_WAIT mono=%.6f seconds=%.6f\n",seconds(),seconds()-start);return 0;
}
static int open_pipeline(DirectBcas *card,const char *socket_path,ARIB_STD_B25 **out){
    memset(card,0,sizeof(*card));
    if(direct_bcas_init(card,socket_path))return -1;
    ARIB_STD_B25 *d=create_arib_std_b25();
    if(!d){if(card->api.release)card->api.release(&card->api);return -1;}
    if(d->set_multi2_round(d,4)<0||d->set_strip(d,0)<0||d->set_emm_proc(d,0)<0||
       d->set_b_cas_card(d,&card->api)<0){
        d->release(d);if(card->api.release)card->api.release(&card->api);return -1;
    }
    *out=d;return 0;
}
int main(int argc,char **argv){
    const char *socket_path=NULL,*trace_path=NULL;
    for(int i=1;i<argc;i++){
        if(i+1<argc&&!strcmp(argv[i],"--card-socket"))socket_path=argv[++i];
        else if(i+1<argc&&!strcmp(argv[i],"--trace"))trace_path=argv[++i];
        else{fprintf(stderr,"usage: torne-b25 --card-socket PATH [--trace FILE]\n");return 2;}
    }
    if(!socket_path)return 2;
    struct sigaction action={0};action.sa_handler=stop;sigemptyset(&action.sa_mask);sigaction(SIGINT,&action,NULL);sigaction(SIGTERM,&action,NULL);signal(SIGPIPE,SIG_IGN);
    TsStats stats;ts_stats_init(&stats,"clear");FILE *trace=NULL;int result=1;
    if(trace_path){int fd=open(trace_path,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return 1;trace=fdopen(fd,"w");if(!trace){close(fd);return 1;}stats.trace=trace;}
    DirectBcas card;memset(&card,0,sizeof(card));
    ARIB_STD_B25 *decoder=NULL;
    if(open_pipeline(&card,socket_path,&decoder)){
        if(!stopping)fprintf(stderr,"ERROR stage=card_init\n");
        if(card.api.release)card.api.release(&card.api);
        if(trace)fclose(trace);
        return stopping?0:1;
    }
    uint8_t bytes[188*256];result=0;
    while(!stopping){
        ssize_t n=read(STDIN_FILENO,bytes,sizeof(bytes));if(n<0&&errno==EINTR)continue;if(n<0){result=1;break;}if(!n)break;
        ARIB_STD_B25_BUFFER input={bytes,(int32_t)n};double started=seconds();int r=decoder->put(decoder,&input);
        if(stopping)break;
        if(seconds()-started>0.1)fprintf(stderr,"B25_WAIT mono=%.6f seconds=%.6f\n",seconds(),seconds()-started);
        if(r<0){
            /*
             * カードのやり取りが切れると put が負を返す。以前はここで終了して
             * いたが、下流が詰まって復号器の出力が止まったときにセッションごと
             * 失われ、録画が数秒で落ちていた。ここで開き直して続ける。
             */
            fprintf(stderr,"torne-b25: card session lost (%d); reopening\n",r);
            decoder->release(decoder);decoder=NULL;
            if(card.api.release)card.api.release(&card.api);
            int tries=0;
            while(!stopping&&tries<20&&open_pipeline(&card,socket_path,&decoder)){struct timespec pause={0,500000000L};tries++;nanosleep(&pause,NULL);decoder=NULL;}
            if(!decoder)break;
            continue;
        }
        if(emit(decoder,&stats)){fprintf(stderr,"ERROR stage=emit\n");result=1;break;}
    }
    if(!result&&!stopping&&(decoder->flush(decoder)<0||emit(decoder,&stats)))result=1;
cleanup:
    ts_stats_print(&stats);fprintf(stderr,"B25_STATS card_commands=%u failed=%d\n",card.commands,result);if(decoder)decoder->release(decoder);if(card.api.release)card.api.release(&card.api);if(trace)fclose(trace);return result;
}
