#include "cm_queue.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <time.h>
#include <unistd.h>

int cm_write_all(int fd,const void *data,size_t size) {
    const unsigned char *p=data;
    while(size){ssize_t n=write(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;size-=(size_t)n;}
    return 0;
}
int cm_require_tmpfs_directory(const char *directory) {
    if(mkdir(directory,0755)!=0&&errno!=EEXIST)return -1;
    struct statfs fs;
    if(statfs(directory,&fs)!=0)return -1;
    if(fs.f_type!=TMPFS_MAGIC){errno=EXDEV;return -1;}
    return 0;
}
int cm_queue_submit(const char *directory,const char *id,const char *source,
                    const char *original_mp4,const char *original_json,int keep_input,const cJSON *metadata) {
    if(cm_require_tmpfs_directory(directory)!=0)return -1;
    char temp[4096],dest[4096];
    if(snprintf(temp,sizeof(temp),"%s/.%s.%ld.tmp",directory,id,(long)getpid())>=(int)sizeof(temp)||
       snprintf(dest,sizeof(dest),"%s/%s.json",directory,id)>=(int)sizeof(dest)){errno=ENAMETOOLONG;return -1;}
    cJSON *job=cJSON_CreateObject();if(!job)return -1;
    cJSON_AddNumberToObject(job,"version",2);
    cJSON_AddStringToObject(job,"input_kind","fmp4");
    cJSON_AddStringToObject(job,"producer",PIX_CM_PRODUCER);
    cJSON_AddNumberToObject(job,"receiver_pid",(double)getpid());
    cJSON_AddNumberToObject(job,"queued_at",(double)time(NULL));
    cJSON_AddBoolToObject(job,"retain_original",1);
    cJSON_AddStringToObject(job,"source",source);
    cJSON_AddStringToObject(job,"original_mp4",original_mp4);
    cJSON_AddStringToObject(job,"original_json",original_json);
    cJSON_AddBoolToObject(job,"keep_input",keep_input);
    cJSON_AddItemToObject(job,"metadata",cJSON_Duplicate(metadata,1));
    char *text=cJSON_PrintUnformatted(job);cJSON_Delete(job);if(!text)return -1;
    int fd=open(temp,O_WRONLY|O_CREAT|O_EXCL,0644),rc=-1;
    if(fd>=0){rc=cm_write_all(fd,text,strlen(text));if(rc==0)rc=fsync(fd);if(close(fd)!=0)rc=-1;if(rc==0)rc=rename(temp,dest);}
    free(text);if(rc!=0)unlink(temp);return rc;
}
