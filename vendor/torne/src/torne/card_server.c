/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "card_server.h"
#include "card_protocol.h"
#include "torne_card.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
struct CardServer { int listen_fd,started;atomic_int client_fd,stop;atomic_uint commands;pthread_t thread;struct torne_card card;char path[108]; };
static int complete(CardServer *s,int fd,void *buffer,size_t count,int writing){
    uint8_t *p=buffer;while(count&&!atomic_load(&s->stop)){
        struct pollfd f={fd,writing?POLLOUT:POLLIN,0};int r=poll(&f,1,100);if(r<0&&errno==EINTR)continue;if(r<0)return -1;if(!r)continue;
        ssize_t n=writing?write(fd,p,count):read(fd,p,count);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;count-=(size_t)n;
    }return count?-1:0;
}
static void *serve(void *value){
    CardServer *s=value;
    while(!atomic_load(&s->stop)){
        struct pollfd listener={s->listen_fd,POLLIN,0};if(poll(&listener,1,100)<=0)continue;
        int fd=accept(s->listen_fd,NULL,NULL);if(fd<0)continue;atomic_store(&s->client_fd,fd);
        TorneCardMessage m;
        while(!complete(s,fd,&m,sizeof(m),0)){
            if(m.size>sizeof(m.data))break;
            s->commands++;
            if(m.operation==TORNE_CARD_RESET){uint8_t atr[33];unsigned n=sizeof(atr);m.status=torne_card_reset(&s->card,atr,&n);m.size=0;}
            else if(m.operation==TORNE_CARD_TRANSMIT){uint8_t answer[512];unsigned n=sizeof(answer);m.status=torne_card_transmit(&s->card,m.data,m.size,answer,&n);m.size=m.status?0:n;if(!m.status)memcpy(m.data,answer,n);}
            else{m.status=-EINVAL;m.size=0;}
            if(complete(s,fd,&m,sizeof(m),1))break;
        }
        close(fd);atomic_store(&s->client_fd,-1);
    }return NULL;
}
CardServer *card_server_open(struct torne_bridge *bridge,const char *path){
    CardServer *s=calloc(1,sizeof(*s));if(!s)return NULL;s->listen_fd=-1;atomic_init(&s->client_fd,-1);
    struct sockaddr_un a={.sun_family=AF_UNIX};if(strlen(path)>=sizeof(a.sun_path)||strlen(path)>=sizeof(s->path)){free(s);return NULL;}
    strcpy(a.sun_path,path);strcpy(s->path,path);torne_card_setup(&s->card,bridge);
    s->listen_fd=socket(AF_UNIX,SOCK_STREAM,0);if(s->listen_fd<0){free(s);return NULL;}
    if(bind(s->listen_fd,(void*)&a,sizeof(a))){close(s->listen_fd);free(s);return NULL;}
    if(chmod(path,0600)||listen(s->listen_fd,1)){close(s->listen_fd);unlink(path);free(s);return NULL;}return s;
}
int card_server_start(CardServer *s){int r=pthread_create(&s->thread,NULL,serve,s);if(!r)s->started=1;return r;}
unsigned card_server_commands(CardServer *s){return atomic_load(&s->commands);}
void card_server_wait_idle(CardServer *s,unsigned ms){for(unsigned t=0;t<ms&&atomic_load(&s->client_fd)>=0;t+=20){struct timespec pause={0,20000000};nanosleep(&pause,NULL);}}
void card_server_close(CardServer *s){if(!s)return;atomic_store(&s->stop,1);int fd=atomic_load(&s->client_fd);if(fd>=0)shutdown(fd,SHUT_RDWR);shutdown(s->listen_fd,SHUT_RDWR);if(s->started)pthread_join(s->thread,NULL);close(s->listen_fd);unlink(s->path);free(s);}
