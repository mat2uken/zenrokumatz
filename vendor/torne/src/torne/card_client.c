/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "card_client.h"
#include "card_protocol.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
static int complete(int fd,void *buffer,unsigned count,int writing){
    uint8_t *p=buffer;while(count){ssize_t n=writing?write(fd,p,count):read(fd,p,count);if(n<=0)return -1;p+=n;count-=(unsigned)n;}return 0;
}
int card_client_open(CardClient *c,const char *path){
    c->fd=-1;struct sockaddr_un address={.sun_family=AF_UNIX};if(strlen(path)>=sizeof(address.sun_path))return -1;strcpy(address.sun_path,path);
    for(int i=0;i<750;i++){
        int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return -1;
        if(!connect(fd,(void*)&address,sizeof(address))){struct timeval t={15,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&t,sizeof(t));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&t,sizeof(t));c->fd=fd;return 0;}
        int error=errno;close(fd);if(error!=ENOENT&&error!=ECONNREFUSED)return -1;struct timespec t={0,20000000};if(nanosleep(&t,NULL)<0)return -1;
    }return -1;
}
static int exchange(CardClient *c,TorneCardMessage *m){if(c->fd<0||complete(c->fd,m,sizeof(*m),1)||complete(c->fd,m,sizeof(*m),0)||m->size>sizeof(m->data))return -1;return m->status;}
int card_client_reset(CardClient *c){TorneCardMessage m={.operation=TORNE_CARD_RESET};return exchange(c,&m);}
int card_client_transmit(CardClient *c,const uint8_t *cmd,unsigned n,uint8_t *out,unsigned *size){if(n>512)return -1;TorneCardMessage m={.operation=TORNE_CARD_TRANSMIT,.size=n};memcpy(m.data,cmd,n);if(exchange(c,&m)||m.size>*size)return -1;memcpy(out,m.data,m.size);*size=m.size;return 0;}
void card_client_close(CardClient *c){if(c->fd>=0)close(c->fd);c->fd=-1;}
