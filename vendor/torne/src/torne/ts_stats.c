/* SPDX-License-Identifier: MIT */
#include "ts_stats.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
void ts_stats_init(TsStats *s,const char *stage){memset(s,0,sizeof(*s));memset(s->cc,0xff,sizeof(s->cc));s->stage=stage;}
void ts_stats_feed(TsStats *s,const uint8_t *data,size_t n){
    for(size_t i=0;i+188<=n;i+=188){
        const uint8_t *p=data+i;if(p[0]!=0x47)continue;
        unsigned pid=((p[1]&31)<<8)|p[2],afc=(p[3]>>4)&3,cc=p[3]&15;
        s->packets++;if(p[1]&128)s->tei++;if(p[3]&192)s->scrambled++;
        if(pid==8191)continue;
        if((afc&2)&&p[4]>0&&p[4]<=183&&(p[5]&128))s->cc[pid]=-1;
        if(!(afc&1))continue;
        if(s->cc[pid]>=0){
            if(cc==(unsigned)s->cc[pid])s->duplicates++;
            else if(cc!=((unsigned)s->cc[pid]+1)%16){
                struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);s->cc_errors++;
                fprintf(stderr,"TS_CC stage=%s mono=%lld.%09ld packet=%" PRIu64 " pid=%u previous=%d current=%u\n",s->stage,(long long)t.tv_sec,t.tv_nsec,s->packets,pid,s->cc[pid],cc);
            }
        }
        s->cc[pid]=(int16_t)cc;
        if(s->trace && !(p[3]&192) && (p[1]&64)){
            size_t at=4+((afc&2)?1+p[4]:0);
            if(at+14<=188 && p[at]==0 && p[at+1]==0 && p[at+2]==1 && (p[at+7]&128)){
                const uint8_t *d=p+at+9;
                int64_t pts=((int64_t)((d[0]>>1)&7)<<30)|((int64_t)d[1]<<22)|((int64_t)(d[2]>>1)<<15)|((int64_t)d[3]<<7)|(d[4]>>1);
                struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
                fprintf(s->trace,"PES stage=%s mono=%lld.%09ld packet=%" PRIu64 " pid=%u pts90k=%" PRId64 "\n",s->stage,(long long)t.tv_sec,t.tv_nsec,s->packets,pid,pts);
            }
        }
    }
}
void ts_stats_print(const TsStats *s){fprintf(stderr,"TS_STATS stage=%s packets=%" PRIu64 " tei=%" PRIu64 " cc_errors=%" PRIu64 " duplicate_cc=%" PRIu64 " scrambled=%" PRIu64 "\n",s->stage,s->packets,s->tei,s->cc_errors,s->duplicates,s->scrambled);}
