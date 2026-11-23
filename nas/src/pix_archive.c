/*
 * pix-archive - 録画フォルダのカタログとアーカイブ EPG の配信。
 * canonical はこのファイル（X96MaxLinuxTSEnc/nas/src/pix_archive.c）。NAS へは
 * /home/ku/pix-archive-20261029/repo/src/archive/pix-archive.c に置いて
 * scripts/build_pix_archive.sh でビルドする。
 * EPG の収集元は pix-epg-bridge（127.0.0.1:40775）である。旧収集元の停止は
 * docs/epg-decommission-design.md を参照。collect の API
 * （/api/services・/api/programs）と live proxy の 4 パスは従来のまま。
 */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "sqlite3.h"
#include "cJSON.h"
#include "civetweb.h"
typedef struct { sqlite3 *db;const char *root,*epg,*dbpath;int interval; } Archive;
static volatile sig_atomic_t stopped;
static void stop(int s){(void)s;stopped=1;}
static double number(const cJSON *j,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsNumber(v)?v->valuedouble:0;}
static const char *string(const cJSON *j,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsString(v)?v->valuestring:"";}
static int64_t epoch_ms(void){struct timespec t;clock_gettime(CLOCK_REALTIME,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void uuid(char out[37]){unsigned char b[16];FILE *f=fopen("/dev/urandom","rb");if(!f||fread(b,1,16,f)!=16)abort();fclose(f);b[6]=(b[6]&15)|64;b[8]=(b[8]&63)|128;snprintf(out,37,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);}
static int sql(Archive *a,const char *s){char *e=NULL;int rc=sqlite3_exec(a->db,s,NULL,NULL,&e);if(rc){fprintf(stderr,"sqlite: %s\n",e);sqlite3_free(e);}return rc;}
static sqlite3_stmt *prepare(Archive *a,const char *s){sqlite3_stmt *p=NULL;if(sqlite3_prepare_v2(a->db,s,-1,&p,NULL)!=SQLITE_OK){fprintf(stderr,"sqlite prepare: %s\n",sqlite3_errmsg(a->db));return NULL;}return p;}
static void text(sqlite3_stmt *p,int i,const char *s){sqlite3_bind_text(p,i,s?s:"",-1,SQLITE_TRANSIENT);}
static cJSON *fetch(const char *base,const char *path){
    if(strncmp(base,"http://",7))return NULL;
    char host[256],port[8]="80",request[512],error[256];const char *p=base+7;size_t n=strcspn(p,"/");if(n>=sizeof(host))return NULL;memcpy(host,p,n);host[n]=0;char *colon=strchr(host,':');if(colon){*colon++=0;if(strlen(colon)>=sizeof(port))return NULL;strcpy(port,colon);}
    snprintf(request,sizeof(request),"GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",path,host);
    struct mg_connection *c=mg_download(host,atoi(port),0,error,sizeof(error),"%s",request);if(!c){fprintf(stderr,"EPG HTTP: %s\n",error);return NULL;}
    const struct mg_response_info *info=mg_get_response_info(c);if(!info||info->status_code!=200){mg_close_connection(c);return NULL;}
    size_t used=0,cap=65536;char *data=malloc(cap);if(!data){mg_close_connection(c);return NULL;}
    int r;while((r=mg_read(c,data+used,cap-used-1))>0){used+=r;if(used+1==cap){if(cap>=64*1024*1024){free(data);mg_close_connection(c);return NULL;}cap*=2;void *v=realloc(data,cap);if(!v){free(data);mg_close_connection(c);return NULL;}data=v;}}
    mg_close_connection(c);data[used]=0;cJSON *j=r<0?NULL:cJSON_Parse(data);free(data);return j;
}
typedef struct { int64_t id;cJSON *json; } Entry;
static int compare(const void *a,const void *b){int64_t x=((const Entry*)a)->id,y=((const Entry*)b)->id;return (x>y)-(x<y);}
static cJSON *resolve(Entry *all,int count,cJSON *p,int depth){
    if(*string(p,"name")||depth>8)return p;
    cJSON *rel=cJSON_GetObjectItem(p,"relatedItems"),*v;
    cJSON_ArrayForEach(v,rel)if(!strcmp(string(v,"type"),"shared")){
        double nid=number(v,"networkId");if(!nid)nid=number(p,"networkId");Entry key={.id=(int64_t)nid*10000000000LL+(int64_t)number(v,"serviceId")*100000+(int64_t)number(v,"eventId")};
        if(key.id==(int64_t)number(p,"id"))continue;Entry *found=bsearch(&key,all,count,sizeof(*all),compare);if(!found)continue;cJSON *source=resolve(all,count,found->json,depth+1);
        if(*string(source,"name"))return source;
    }
    return p;
}
static int collect(Archive *a){
    cJSON *services=fetch(a->epg,"/api/services"),*programs=fetch(a->epg,"/api/programs");
    if(!cJSON_IsArray(services)||!cJSON_IsArray(programs)){cJSON_Delete(services);cJSON_Delete(programs);return -1;}
    int count=cJSON_GetArraySize(programs);Entry *entries=calloc(count,sizeof(*entries));if(!entries){cJSON_Delete(services);cJSON_Delete(programs);return -1;}
    cJSON *p;int i=0; cJSON_ArrayForEach(p,programs){entries[i++]=(Entry){(int64_t)number(p,"id"),p};}qsort(entries,count,sizeof(*entries),compare);
    int rc=sql(a,"BEGIN IMMEDIATE");int64_t now=epoch_ms();int inherited=0;fprintf(stderr,"EPG_BEGIN programs=%d utc_ms=%" PRId64 "\n",count,now);
    sqlite3_stmt *service=prepare(a,"INSERT INTO services(id,json) VALUES(?,?) ON CONFLICT(id) DO UPDATE SET json=excluded.json");
    sqlite3_stmt *find=prepare(a,"SELECT id,json FROM airings WHERE source_id=? AND (start_at=? OR (start_at<? AND start_at+duration>?)) ORDER BY last_seen DESC LIMIT 1");
    sqlite3_stmt *put=prepare(a,"INSERT INTO airings(id,source_id,service_id,initial_start,start_at,duration,title,json,source_json,first_seen,last_seen) VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET start_at=excluded.start_at,duration=excluded.duration,title=excluded.title,json=excluded.json,source_json=excluded.source_json,last_seen=excluded.last_seen");
    sqlite3_stmt *revision=prepare(a,"INSERT INTO revisions(airing_id,observed_at,json) VALUES(?,?,?)");
    if(!service||!find||!put||!revision)rc=1;
    cJSON_ArrayForEach(p,services){if(rc)break;char *raw=cJSON_PrintUnformatted(p);sqlite3_reset(service);sqlite3_bind_int64(service,1,(int64_t)number(p,"id"));text(service,2,raw);if(sqlite3_step(service)!=SQLITE_DONE)rc=1;free(raw);}
    cJSON_ArrayForEach(p,programs){if(rc)break;
        int64_t sid=(int64_t)number(p,"networkId")*100000+(int64_t)number(p,"serviceId"),id=(int64_t)number(p,"id"),start=(int64_t)number(p,"startAt"),duration=(int64_t)number(p,"duration");
        char *raw=cJSON_PrintUnformatted(p);cJSON *normalized=cJSON_Duplicate(p,1),*source=resolve(entries,count,p,0);
        if(source!=p){const char *keys[]={"name","description","extended","genres","video","audio","audios"};for(size_t k=0;k<sizeof(keys)/sizeof(*keys);k++){cJSON *old=cJSON_GetObjectItem(normalized,keys[k]),*value=cJSON_GetObjectItem(source,keys[k]);if((!old||cJSON_IsNull(old))&&value){cJSON_DeleteItemFromObject(normalized,keys[k]);cJSON_AddItemToObject(normalized,keys[k],cJSON_Duplicate(value,1));}}cJSON_AddNumberToObject(normalized,"metadataSourceProgramId",number(source,"id"));inherited++;}
        char *body=cJSON_PrintUnformatted(normalized);char airing[37];uuid(airing);int changed=1;
        sqlite3_reset(find);sqlite3_bind_int64(find,1,id);sqlite3_bind_int64(find,2,start);sqlite3_bind_int64(find,3,start+duration);sqlite3_bind_int64(find,4,start);
        if(sqlite3_step(find)==SQLITE_ROW){snprintf(airing,sizeof(airing),"%s",sqlite3_column_text(find,0));const char *old=(const char*)sqlite3_column_text(find,1);changed=!old||strcmp(old,body);}
        sqlite3_reset(put);text(put,1,airing);sqlite3_bind_int64(put,2,id);sqlite3_bind_int64(put,3,sid);sqlite3_bind_int64(put,4,start);sqlite3_bind_int64(put,5,start);sqlite3_bind_int64(put,6,duration);text(put,7,string(normalized,"name"));text(put,8,body);text(put,9,raw);sqlite3_bind_int64(put,10,now);sqlite3_bind_int64(put,11,now);if(sqlite3_step(put)!=SQLITE_DONE)rc=1;
        if(changed){sqlite3_reset(revision);text(revision,1,airing);sqlite3_bind_int64(revision,2,now);text(revision,3,body);if(sqlite3_step(revision)!=SQLITE_DONE)rc=1;}
        cJSON_Delete(normalized);free(raw);free(body);
    }
    sqlite3_finalize(service);sqlite3_finalize(find);sqlite3_finalize(put);sqlite3_finalize(revision);if(rc)sql(a,"ROLLBACK");else sql(a,"COMMIT");
    fprintf(stderr,"EPG collect programs=%d inherited_names=%d result=%d utc_ms=%" PRId64 "\n",count,inherited,rc,epoch_ms());
    free(entries);cJSON_Delete(services);cJSON_Delete(programs);return rc?-1:0;
}
static int valid_relative(const char *path){return *path && path[0]!='/' && !strstr(path,"../") && !strstr(path,"/..") && strcmp(path,"..");}
static void ingest(Archive *a,const char *directory){
    DIR *dir=opendir(directory);if(!dir)return;struct dirent *entry;
    while((entry=readdir(dir))){if(entry->d_name[0]=='.')continue;char path[4096];if(snprintf(path,sizeof(path),"%s/%s",directory,entry->d_name)>=(int)sizeof(path))continue;struct stat st;if(lstat(path,&st))continue;
        if(S_ISDIR(st.st_mode)){ingest(a,path);continue;}size_t n=strlen(path);if(!S_ISREG(st.st_mode)||n<5||strcmp(path+n-5,".json")||st.st_size>1024*1024)continue;
        FILE *f=fopen(path,"rb");if(!f)continue;char *raw=malloc(st.st_size+1);if(!raw){fclose(f);continue;}size_t got=fread(raw,1,st.st_size,f);fclose(f);raw[got]=0;cJSON *j=cJSON_Parse(raw);
        cJSON *media=cJSON_GetObjectItem(j,"media"),*program=cJSON_GetObjectItem(j,"program"),*service=cJSON_GetObjectItem(j,"service");
        const char *part=string(j,"partId"),*relative=string(media,"relativePath");
        if(*part&&valid_relative(relative)){

            if(program && number(program,"id") && number(program,"startAt")){
                sqlite3_stmt *find=prepare(a,"SELECT id FROM airings WHERE source_id=? AND (start_at=? OR (start_at<? AND start_at+duration>?)) LIMIT 1");
                int64_t start=(int64_t)number(program,"startAt"),duration=(int64_t)number(program,"duration");int exists=0;
                if(find){sqlite3_bind_int64(find,1,(int64_t)number(program,"id"));sqlite3_bind_int64(find,2,start);sqlite3_bind_int64(find,3,start+duration);sqlite3_bind_int64(find,4,start);exists=sqlite3_step(find)==SQLITE_ROW;sqlite3_finalize(find);}
                if(!exists){
                    cJSON *epg=cJSON_Duplicate(program,1);const char *fields[]={"networkId","serviceId","transportStreamId"};
                    for(unsigned i=0;i<3;i++)if(!cJSON_GetObjectItem(epg,fields[i]))cJSON_AddNumberToObject(epg,fields[i],number(service,fields[i]));
                    char *body=cJSON_PrintUnformatted(epg);char id[37];uuid(id);sqlite3_stmt *put=prepare(a,"INSERT INTO airings VALUES(?,?,?,?,?,?,?,?,?,?,?)");
                    if(put){text(put,1,id);sqlite3_bind_int64(put,2,(int64_t)number(program,"id"));sqlite3_bind_int64(put,3,(int64_t)number(service,"id"));sqlite3_bind_int64(put,4,start);sqlite3_bind_int64(put,5,start);sqlite3_bind_int64(put,6,duration);text(put,7,string(epg,"name"));text(put,8,body);text(put,9,body);sqlite3_bind_int64(put,10,epoch_ms());sqlite3_bind_int64(put,11,epoch_ms());sqlite3_step(put);sqlite3_finalize(put);}free(body);cJSON_Delete(epg);
                }
            }
            sqlite3_stmt *p=prepare(a,"INSERT INTO recordings(part_id,recording_id,source_id,start_at,service_id,path,json) VALUES(?,?,?,?,?,?,?) ON CONFLICT(part_id) DO UPDATE SET path=excluded.path,json=excluded.json WHERE recordings.path!=excluded.path OR recordings.json!=excluded.json");
            if(p){text(p,1,part);text(p,2,string(j,"recordingId"));sqlite3_bind_int64(p,3,(int64_t)number(program,"id"));sqlite3_bind_int64(p,4,(int64_t)number(program,"startAt"));sqlite3_bind_int64(p,5,(int64_t)number(service,"id"));text(p,6,relative);text(p,7,raw);sqlite3_step(p);sqlite3_finalize(p);}
        }
        cJSON_Delete(j);free(raw);
    }
    closedir(dir);
}
static void *worker(void *ptr){Archive *a=ptr;int elapsed=a->interval,scan=10;while(!stopped){if(elapsed>=a->interval){collect(a);elapsed=0;}if(scan>=10){char path[4096];snprintf(path,sizeof(path),"%s/recordings",a->root);ingest(a,path);scan=0;}sleep(1);elapsed++;scan++;}return NULL;}
static int send_json(struct mg_connection *c,cJSON *j){char *s=cJSON_PrintUnformatted(j);if(!s)return 500;mg_printf(c,"HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n\r\n",strlen(s));if(strcmp(mg_get_request_info(c)->request_method,"HEAD"))mg_write(c,s,strlen(s));free(s);return 200;}
static int media_file(struct mg_connection *c,const char *path){
    int fd=open(path,O_RDONLY);struct stat st;if(fd<0||fstat(fd,&st)||!S_ISREG(st.st_mode)){if(fd>=0)close(fd);mg_send_http_error(c,404,"media not found");return 404;}
    uint64_t size=st.st_size,start=0,end=size?size-1:0;int partial=0;const char *range=mg_get_header(c,"Range");
    if(range){
        if(strncmp(range,"bytes=",6)||!size)goto invalid;
        const char *p=range+6;char *tail;errno=0;
        if(*p=='-'){
            p++;if(!isdigit((unsigned char)*p))goto invalid;uint64_t n=strtoull(p,&tail,10);if(errno||tail==p||*tail||!n)goto invalid;start=n>=size?0:size-n;
        }else{
            if(!isdigit((unsigned char)*p))goto invalid;start=strtoull(p,&tail,10);if(errno||tail==p||*tail!='-')goto invalid;p=tail+1;
            if(*p){if(!isdigit((unsigned char)*p))goto invalid;end=strtoull(p,&tail,10);if(errno||tail==p||*tail)goto invalid;}
            if(start>=size||end<start)goto invalid;if(end>=size)end=size-1;
        }
        partial=1;
    }
    uint64_t count=size?end-start+1:0;
    mg_printf(c,"HTTP/1.1 %s\r\nContent-Type: video/mp4\r\nAccept-Ranges: bytes\r\nContent-Length: %" PRIu64 "\r\n",partial?"206 Partial Content":"200 OK",count);
    if(partial)mg_printf(c,"Content-Range: bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64 "\r\n",start,end,size);
    mg_printf(c,"\r\n");
    if(strcmp(mg_get_request_info(c)->request_method,"HEAD")){
        uint8_t data[65536];while(count){size_t n=count>sizeof(data)?sizeof(data):(size_t)count;ssize_t got=pread(fd,data,n,(off_t)start);if(got<=0)break;int at=0;while(at<got){int sent=mg_write(c,data+at,(size_t)(got-at));if(sent<=0){close(fd);return partial?206:200;}at+=sent;}start+=got;count-=got;}
    }
    close(fd);return partial?206:200;
invalid:
    mg_printf(c,"HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%" PRIu64 "\r\nContent-Length: 0\r\n\r\n",size);close(fd);return 416;
}
static int api(struct mg_connection *c,void *ptr){
    Archive *shared=ptr;Archive local={.root=shared->root};Archive *a=&local;const struct mg_request_info *r=mg_get_request_info(c);const char *uri=r->local_uri;const char *q=r->query_string?r->query_string:"";
    if(strcmp(r->request_method,"GET")&&strcmp(r->request_method,"HEAD")){mg_send_http_error(c,405,"GET/HEAD only");return 405;}
    if(sqlite3_open_v2(shared->dbpath,&a->db,SQLITE_OPEN_READONLY,NULL)!=SQLITE_OK){if(a->db)sqlite3_close(a->db);mg_send_http_error(c,503,"catalog unavailable");return 503;}
    sqlite3_busy_timeout(a->db,1000);
    if(!strncmp(uri,"/media/",7)){
        char id[80];snprintf(id,sizeof(id),"%s",uri+7);char *dot=strstr(id,".mp4");if(dot)*dot=0;
        sqlite3_stmt *p=prepare(a,"SELECT path FROM recordings WHERE part_id=?");char path[4096]={0};
        if(p){text(p,1,id);if(sqlite3_step(p)==SQLITE_ROW)snprintf(path,sizeof(path),"%s/%s",a->root,sqlite3_column_text(p,0));sqlite3_finalize(p);}
        sqlite3_close(a->db);
        if(!path[0]){mg_send_http_error(c,404,"recording not found");return 404;}return media_file(c,path);
    }
    cJSON *out=cJSON_CreateArray();sqlite3_stmt *p=NULL;
    if(!strcmp(uri,"/api/archive/services"))p=prepare(a,"SELECT json FROM services ORDER BY id");
    else if(!strcmp(uri,"/api/archive/dates")){p=prepare(a,"SELECT DISTINCT date(start_at/1000,'unixepoch','+4 hours') FROM airings WHERE start_at>0 ORDER BY 1");}
    else if(!strcmp(uri,"/api/archive/programs")){
        char from[32],to[32],sid[32];if(mg_get_var(q,strlen(q),"from",from,sizeof(from))<0)strcpy(from,"0");if(mg_get_var(q,strlen(q),"to",to,sizeof(to))<0)strcpy(to,"9223372036854775807");if(mg_get_var(q,strlen(q),"serviceId",sid,sizeof(sid))<0)strcpy(sid,"0");
        p=prepare(a,"SELECT id,json FROM airings WHERE start_at<? AND start_at+duration>? AND (?=0 OR service_id=?) ORDER BY start_at,service_id");
        if(p){sqlite3_bind_int64(p,1,strtoll(to,NULL,10));sqlite3_bind_int64(p,2,strtoll(from,NULL,10));sqlite3_bind_int64(p,3,strtoll(sid,NULL,10));sqlite3_bind_int64(p,4,strtoll(sid,NULL,10));}
    }else if(!strncmp(uri,"/api/archive/recordings/",24)){
        p=prepare(a,"SELECT json FROM recordings WHERE recording_id=? ORDER BY path");if(p)text(p,1,uri+24);
    }else if(!strcmp(uri,"/health")){
        cJSON_Delete(out);out=cJSON_CreateObject();cJSON_AddStringToObject(out,"status","ok");p=prepare(a,"SELECT COUNT(*),MAX(last_seen) FROM airings");
        if(p&&sqlite3_step(p)==SQLITE_ROW){cJSON_AddNumberToObject(out,"programs",sqlite3_column_int64(p,0));cJSON_AddNumberToObject(out,"lastFetchAt",sqlite3_column_int64(p,1));}sqlite3_finalize(p);p=NULL;
    }
    if(p){while(sqlite3_step(p)==SQLITE_ROW){if(!strcmp(uri,"/api/archive/dates")){cJSON_AddItemToArray(out,cJSON_CreateString((const char*)sqlite3_column_text(p,0)));continue;}int listing=!strcmp(uri,"/api/archive/programs");cJSON *j=cJSON_Parse((const char*)sqlite3_column_text(p,listing?1:0));if(!j)continue;
            if(listing){cJSON_AddStringToObject(j,"airingId",(const char*)sqlite3_column_text(p,0));cJSON *links=cJSON_AddArrayToObject(j,"recordings");sqlite3_stmt *parts=prepare(a,"SELECT part_id,recording_id,json FROM recordings WHERE source_id=? AND (start_at=? OR (json_extract(json,'$.capture.startAt')<? AND json_extract(json,'$.capture.endAt')>?))");if(parts){sqlite3_bind_int64(parts,1,(int64_t)number(j,"id"));sqlite3_bind_int64(parts,2,(int64_t)number(j,"startAt"));sqlite3_bind_int64(parts,3,(int64_t)(number(j,"startAt")+number(j,"duration")));sqlite3_bind_int64(parts,4,(int64_t)number(j,"startAt"));while(sqlite3_step(parts)==SQLITE_ROW){cJSON *v=cJSON_Parse((const char*)sqlite3_column_text(parts,2));if(v){char url[128];snprintf(url,sizeof(url),"/media/%s.mp4",sqlite3_column_text(parts,0));cJSON_AddStringToObject(v,"playbackUrl",url);cJSON_AddItemToArray(links,v);}}sqlite3_finalize(parts);}}
            cJSON_AddItemToArray(out,j);}
        sqlite3_finalize(p);
    }
    sqlite3_close(a->db);int status=send_json(c,out);cJSON_Delete(out);return status;
}
static int live_api(struct mg_connection *c,void *ptr){
    Archive *a=ptr;const struct mg_request_info *r=mg_get_request_info(c);const char *allowed[]={"/api/programs","/api/services","/api/tuners","/api/version"};
    if(strcmp(r->request_method,"GET")){mg_send_http_error(c,405,"GET only");return 405;}
    for(unsigned i=0;i<4;i++)if(!strcmp(r->local_uri,allowed[i])){cJSON *j=fetch(a->epg,allowed[i]);if(!j){mg_send_http_error(c,502,"epg source unavailable");return 502;}int status=send_json(c,j);cJSON_Delete(j);return status;}
    mg_send_http_error(c,404,"not found");return 404;
}
int main(int argc,char **argv){
    Archive a={.interval=300,.epg="http://127.0.0.1:40775"};const char *listen="192.168.99.80:40773",*web_root=NULL;int once=0;
    for(int i=1;i<argc;i++){if(!strcmp(argv[i],"--collect-once"))once=1;else if(i+1<argc&&!strcmp(argv[i],"--root"))a.root=argv[++i];else if(i+1<argc&&!strcmp(argv[i],"--epg"))a.epg=argv[++i];else if(i+1<argc&&!strcmp(argv[i],"--listen"))listen=argv[++i];else if(i+1<argc&&!strcmp(argv[i],"--web-root"))web_root=argv[++i];else if(i+1<argc&&!strcmp(argv[i],"--interval"))a.interval=atoi(argv[++i]);else return 2;}
    if(!a.root||a.interval<1){fprintf(stderr,"usage: pix-archive --root DIR [--epg URL --listen IP:PORT --web-root DIR --interval 300 --collect-once]\n");return 2;}
    char directory[4096],dbpath[4096];snprintf(directory,sizeof(directory),"%s/_catalog",a.root);if(mkdir(directory,0700)&&errno!=EEXIST){perror(directory);return 1;}snprintf(dbpath,sizeof(dbpath),"%s/_catalog/archive.sqlite",a.root);
    a.dbpath=dbpath;
    if(sqlite3_open(dbpath,&a.db)!=SQLITE_OK)return 1;sqlite3_busy_timeout(a.db,10000);
    if(sql(&a,"PRAGMA journal_mode=WAL;CREATE TABLE IF NOT EXISTS services(id INTEGER PRIMARY KEY,json TEXT NOT NULL);CREATE TABLE IF NOT EXISTS airings(id TEXT PRIMARY KEY,source_id INTEGER,service_id INTEGER,initial_start INTEGER,start_at INTEGER,duration INTEGER,title TEXT,json TEXT,source_json TEXT,first_seen INTEGER,last_seen INTEGER);CREATE INDEX IF NOT EXISTS airings_time ON airings(service_id,start_at);CREATE INDEX IF NOT EXISTS airings_source ON airings(source_id,start_at);CREATE TABLE IF NOT EXISTS revisions(airing_id TEXT,observed_at INTEGER,json TEXT);CREATE TABLE IF NOT EXISTS recordings(part_id TEXT PRIMARY KEY,recording_id TEXT,source_id INTEGER,start_at INTEGER,service_id INTEGER,path TEXT,json TEXT);CREATE INDEX IF NOT EXISTS recordings_program ON recordings(source_id,start_at);"))return 1;
    mg_init_library(0);signal(SIGINT,stop);signal(SIGTERM,stop);
    if(once){int rc=collect(&a);sqlite3_close(a.db);mg_exit_library();return rc?1:0;}
    const char *options[]={"listening_ports",listen,"num_threads","3","enable_directory_listing","no",NULL,NULL,NULL,NULL,NULL};
    if(web_root){struct stat st;if(stat(web_root,&st)||!S_ISDIR(st.st_mode)){fprintf(stderr,"web root is not a directory\n");return 1;}options[6]="document_root";options[7]=web_root;options[8]="index_files";options[9]="index.html";}
    struct mg_context *server=mg_start(NULL,&a,options);if(!server)return 1;
    mg_set_request_handler(server,"/api/archive",api,&a);mg_set_request_handler(server,"/media",api,&a);mg_set_request_handler(server,"/health",api,&a);
    const char *live_paths[]={"/api/programs","/api/services","/api/tuners","/api/version"};for(unsigned i=0;i<4;i++)mg_set_request_handler(server,live_paths[i],live_api,&a);
    pthread_t task;pthread_create(&task,NULL,worker,&a);fprintf(stderr,"pix-archive listening=%s root=%s\n",listen,a.root);
    while(!stopped)sleep(1);pthread_join(task,NULL);mg_stop(server);sqlite3_close(a.db);mg_exit_library();return 0;
}
