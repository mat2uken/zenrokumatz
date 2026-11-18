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
    /*
     * eit_watch state file. GET /v1/program returns it verbatim so a reader
     * that connects in the middle of a programme can name its first segment
     * without waiting for the next boundary.
     */
    const char *program_path;
    /*
     * Station logos written by pix-dt295-logo: GET /v1/logos serves its
     * index.json, GET /v1/logos/<name>.png one file. NULL disables both.
     */
    const char *logo_dir;
} StbHttpConfig;

StbHttpServer *stb_http_start(const StbHttpConfig *cfg);
void stb_http_stop(StbHttpServer *srv);

/*
 * Feature sideband version requested by the most recent stream reader.
 * 0 means the reader did not ask for features. The capture side must not
 * emit any sideband while this is 0.
 */
int stb_http_feature_version(StbHttpServer *srv);

/*
 * Set when a new reader starts streaming, cleared by whoever consumes it.
 * The caller uses it to line the feature timeline up with the media it sends.
 */
int stb_http_take_feature_restart(StbHttpServer *srv);

#endif /* STB_HTTP_H */
