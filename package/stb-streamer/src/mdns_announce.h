#ifndef MDNS_ANNOUNCE_H
#define MDNS_ANNOUNCE_H

#include <stdint.h>
#include <netinet/in.h>

typedef struct MdnsAnnouncer MdnsAnnouncer;

typedef struct {
    int port;              /* HTTPポート */
    const char *host;      /* インスタンス名 (例: "pixstb-x96") */
    const char *display;   /* 管理画面に出す名前 */
    int channel;
    struct in_addr addr;   /* 自分のIPv4 (ネットワークバイトオーダー) */
} MdnsAnnounceConfig;

/** Starts the announcer. Returns NULL when it cannot run. */
MdnsAnnouncer *mdns_announce_start(const MdnsAnnounceConfig *cfg);

void mdns_announce_stop(MdnsAnnouncer *a);

#endif /* MDNS_ANNOUNCE_H */
