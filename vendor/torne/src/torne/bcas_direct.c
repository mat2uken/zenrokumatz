/* SPDX-License-Identifier: Apache-2.0 */
/* Card response layout follows stz2012/libarib25 b_cas_card.c (93ff3afd). */
#include "bcas_direct.h"
#include <string.h>
static unsigned be16(const uint8_t *p){return (p[0]<<8)|p[1];}
static int64_t be48(const uint8_t *p){int64_t v=0;for(int i=0;i<6;i++)v=(v<<8)|p[i];return v;}
static DirectBcas *state(void *v){return ((B_CAS_CARD *)v)->private_data;}
static int exchange(DirectBcas *s,const uint8_t *cmd,unsigned len,uint8_t *answer,unsigned *size){s->commands++;return card_client_transmit(&s->client,cmd,len,answer,size);}
static void release_card(void *v){DirectBcas *s=state(v);card_client_close(&s->client);memset(&s->status,0,sizeof(s->status));}
static int init_card(void *v){
    DirectBcas *s=state(v);uint8_t answer[260];unsigned size=sizeof(answer);
    if(card_client_reset(&s->client))return -1;
    const uint8_t cmd[]={0x90,0x30,0,0,0};
    if(exchange(s,cmd,sizeof(cmd),answer,&size)||size<57||be16(answer+4)!=0x2100)return -1;
    memcpy(s->status.system_key,answer+16,32);memcpy(s->status.init_cbc,answer+48,8);
    s->status.bcas_card_id=be48(answer+8);s->status.card_status=(int32_t)be16(answer+2);s->status.ca_system_id=(int32_t)be16(answer+6);
    memset(answer,0,sizeof(answer));return 0;
}
static int get_status(void *v,B_CAS_INIT_STATUS *out){*out=state(v)->status;return 0;}
static int get_ids(void *v,B_CAS_ID *out){
    DirectBcas *s=state(v);uint8_t answer[260];unsigned n=sizeof(answer);const uint8_t cmd[]={0x90,0x32,0,0,0};
    if(exchange(s,cmd,sizeof(cmd),answer,&n)||n<19)return -1;
    unsigned count=answer[6];if(count>16||7+count*10>n)return -1;
    for(unsigned i=0;i<count;i++)s->ids[i]=be48(answer+7+i*10+2);
    s->id_count=(int)count;out->count=(int32_t)count;out->data=s->ids;return 0;
}
static int get_power(void *v,B_CAS_PWR_ON_CTRL_INFO *out){(void)v;memset(out,0,sizeof(*out));return 0;}
static int ecm(void *v,B_CAS_ECM_RESULT *out,uint8_t *data,int len){
    DirectBcas *s=state(v);if(len<1||len>255)return -1;
    uint8_t cmd[261]={0x90,0x34,0,0},answer[260];unsigned n=sizeof(answer);
    cmd[4]=(uint8_t)len;memcpy(cmd+5,data,(size_t)len);cmd[5+len]=0;
    if(exchange(s,cmd,(unsigned)len+6,answer,&n)||n<25)return -1;
    memcpy(out->scramble_key,answer+6,16);out->return_code=be16(answer+4);memset(answer,0,sizeof(answer));return 0;
}
static int emm_disabled(void *v,uint8_t *data,int len){(void)v;(void)data;(void)len;return -1;}
int direct_bcas_init(DirectBcas *s,const char *path){
    memset(s,0,sizeof(*s));s->client.fd=-1;if(card_client_open(&s->client,path))return -1;
    s->api=(B_CAS_CARD){.private_data=s,.release=release_card,.init=init_card,.get_init_status=get_status,.get_id=get_ids,.get_pwr_on_ctrl=get_power,.proc_ecm=ecm,.proc_emm=emm_disabled};
    return s->api.init(&s->api);
}
