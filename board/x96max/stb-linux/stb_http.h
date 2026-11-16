#ifndef STB_HTTP_H
#define STB_HTTP_H

#include "ram_spool.h"
#include <stdint.h>

typedef struct StbHttpServer StbHttpServer;

typedef struct {
    int port;
    const char *allowed_ip;  /* "192.168.99.80,192.168.99.52" のようにカンマ区切り (NULLならローカルのみ) */
    RamSpool *spool;
    volatile int *stop_flag;
    /* Called when a stream reader has been given the initialisation segment. */
    void (*on_stream_client)(void *user);
    void *on_stream_user;
    /* Called when the requested channel differs from the one being captured. */
    void (*on_channel_change)(void *user, const char *channel);
    void *on_channel_user;
    /* Seconds without a reader before the capture is stopped (0 = never). */
    int idle_timeout_sec;
} StbHttpConfig;

StbHttpServer *stb_http_start(const StbHttpConfig *cfg);
void stb_http_stop(StbHttpServer *srv);

#endif /* STB_HTTP_H */
