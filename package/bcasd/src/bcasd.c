/* bcasd.c - multi-client B-CAS APDU server for 2 PC/SC readers.
 *
 * Each client speaks the 524-byte TorneCardMessage frame over TCP. The server
 * assigns clients to the least-loaded card, serialises APDUs per card, shares
 * the 90 30 init response, caches successful ECM responses and de-duplicates
 * EMM elements. It runs as an unprivileged user in the pcsc group.
 *
 * Build:
 *   cc -O2 -Wall -Wextra -pthread -o bcasd bcasd.c \
 *      $(pkg-config --cflags --libs libpcsclite)
 */
#define _GNU_SOURCE
#include <PCSC/winscard.h>
#include <PCSC/pcsclite.h>
#include <PCSC/reader.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_CARDS 2
#define MAX_CLIENTS 8
#define MAX_TOKENS 16
#define MAX_ALLOW 8
#define MAX_APDU 512
#define FRAME_SIZE 524
#define CACHE_MAX 128
#define READER_MAX 256
#define NAME_MAX 64

typedef struct {
    uint32_t operation;
    uint32_t size;
    int32_t status;
    uint8_t data[MAX_APDU];
} __attribute__((packed)) Frame;

typedef struct {
    uint8_t apdu[MAX_APDU];
    int apdu_len;
    uint8_t resp[MAX_APDU];
    int resp_len;
    int64_t expires_ms;
    int valid;
} CacheEntry;

typedef struct {
    char name[NAME_MAX];
    uint8_t token[32];
} Token;

typedef struct {
    uint32_t net;
    uint32_t mask;
} Allow;

typedef struct {
    int fd;
    int active;
    int card_index;
    char name[NAME_MAX];
    uint8_t token[32];
    char peer[64];
    pthread_t thread;
} Client;

typedef struct {
    int index;
    char reader[READER_MAX];
    SCARDCONTEXT ctx;
    SCARDHANDLE card;
    int connected;
    pthread_mutex_t lock;

    int clients;
    int fault;

    uint8_t atr[64];
    int atr_len;

    uint8_t init_resp[MAX_APDU];
    int init_len;
    int init_valid;

    CacheEntry ecm[CACHE_MAX];
    int ecm_count;
    CacheEntry emm[CACHE_MAX];
    int emm_count;

    uint64_t apdu_count;
    uint64_t cache_hits;
    uint64_t errors;
} Card;

static Card g_cards[MAX_CARDS];
static int g_card_count = 2;
static Client g_clients[MAX_CLIENTS];
static int g_active_clients;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static Token g_tokens[MAX_TOKENS];
static int g_token_count;
static Allow g_allow[MAX_ALLOW];
static int g_allow_count;

static char g_bind[64] = "192.168.99.10";
static int g_port = 40790;
static int g_max_clients = 4;
static int g_clients_per_card = 2;
static int g_ecm_ttl_ms = 5000;
static int g_emm_ttl_ms = 10000;
static int g_ecm_entries = 64;
static int g_emm_entries = 64;
static char g_tokens_path[READER_MAX] = "/etc/bcas/tokens";

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_reload;
static volatile sig_atomic_t g_recover_all;
static int g_listen_fd = -1;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void log_msg(const char *fmt, ...)
{
    struct timespec ts;
    struct tm tm;
    char stamp[32];
    va_list ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    flockfile(stderr);
    fprintf(stderr, "%s ", stamp);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    funlockfile(stderr);
}

static int token_equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t d = 0;
    for (int i = 0; i < 32; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_token_hex(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) return -1;
    for (int i = 0; i < 32; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
    *end = 0;
    return s;
}

static int parse_allow(const char *value)
{
    char buf[READER_MAX];
    char *save = NULL;
    char *item;

    g_allow_count = 0;
    snprintf(buf, sizeof(buf), "%s", value);
    for (item = strtok_r(buf, ",", &save); item && g_allow_count < MAX_ALLOW;
         item = strtok_r(NULL, ",", &save)) {
        char *slash;
        struct in_addr ina;
        uint32_t addr, mask;
        int bits = 32;

        item = trim(item);
        if (!*item) continue;
        slash = strchr(item, '/');
        if (slash) {
            *slash = 0;
            bits = atoi(slash + 1);
            if (bits < 0 || bits > 32) return -1;
        }
        if (inet_pton(AF_INET, item, &ina) != 1) return -1;
        addr = ntohl(ina.s_addr);
        mask = bits == 0 ? 0u : (0xffffffffu << (32 - bits));
        g_allow[g_allow_count].net = addr & mask;
        g_allow[g_allow_count].mask = mask;
        g_allow_count++;
    }
    return 0;
}

static int load_tokens(void)
{
    FILE *f;
    char line[256];
    Token tmp[MAX_TOKENS];
    int count = 0;

    f = fopen(g_tokens_path, "r");
    if (!f) {
        log_msg("cannot open tokens: %s: %s", g_tokens_path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char name[NAME_MAX], hex[128];
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        if (sscanf(p, "%63s %127s", name, hex) != 2) continue;
        if (count >= MAX_TOKENS) {
            log_msg("too many tokens, ignoring %s", name);
            continue;
        }
        if (parse_token_hex(hex, tmp[count].token)) {
            log_msg("invalid token for %s", name);
            continue;
        }
        snprintf(tmp[count].name, sizeof(tmp[count].name), "%s", name);
        count++;
    }
    fclose(f);
    if (!count) {
        log_msg("no valid tokens in %s", g_tokens_path);
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    memcpy(g_tokens, tmp, sizeof(Token) * (size_t)count);
    g_token_count = count;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *cl = &g_clients[i];
        int found = 0;
        if (!cl->active) continue;
        for (int j = 0; j < g_token_count; j++) {
            if (token_equal(cl->token, g_tokens[j].token)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            log_msg("token revoked for %s", cl->name);
            shutdown(cl->fd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&g_lock);
    log_msg("loaded %d tokens", count);
    return 0;
}

static int find_token(const uint8_t token[32], char *name, size_t name_len)
{
    int found = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_token_count; i++) {
        if (token_equal(token, g_tokens[i].token)) {
            snprintf(name, name_len, "%s", g_tokens[i].name);
            found = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

static int peer_allowed(const struct sockaddr_in *peer)
{
    uint32_t addr = ntohl(peer->sin_addr.s_addr);
    if (!g_allow_count) return 1;
    for (int i = 0; i < g_allow_count; i++) {
        if ((addr & g_allow[i].mask) == g_allow[i].net) return 1;
    }
    return 0;
}

static int pcsc_status(LONG ret)
{
    switch (ret) {
    case SCARD_S_SUCCESS: return 0;
    case SCARD_E_SHARING_VIOLATION: return -2;
    case SCARD_W_REMOVED_CARD:
    case SCARD_E_NO_SMARTCARD:
    case SCARD_E_NO_READERS_AVAILABLE:
    case SCARD_E_READER_UNAVAILABLE:
        return -1;
    case SCARD_W_RESET_CARD:
    case SCARD_E_COMM_DATA_LOST:
    case SCARD_F_COMM_ERROR:
    case SCARD_E_NOT_TRANSACTED:
        return -5;
    default: return -5;
    }
}

static void clear_caches(Card *c)
{
    c->init_valid = 0;
    c->init_len = 0;
    for (int i = 0; i < c->ecm_count; i++) c->ecm[i].valid = 0;
    for (int i = 0; i < c->emm_count; i++) c->emm[i].valid = 0;
}

static int card_open_locked(Card *c)
{
    LONG ret;
    DWORD protocol = 0;
    DWORD atr_len = sizeof(c->atr);

    if (c->connected) return 0;
    if (!c->ctx) {
        ret = SCardEstablishContext(SCARD_SCOPE_USER, NULL, NULL, &c->ctx);
        if (ret != SCARD_S_SUCCESS) {
            log_msg("card%d establish failed: %s", c->index, pcsc_stringify_error(ret));
            return pcsc_status(ret);
        }
    }
    if (!c->reader[0]) {
        LPSTR readers = NULL;
        DWORD len = SCARD_AUTOALLOCATE;
        ret = SCardListReaders(c->ctx, NULL, (LPSTR)&readers, &len);
        if (ret != SCARD_S_SUCCESS) {
            log_msg("card%d list readers failed: %s", c->index, pcsc_stringify_error(ret));
            return pcsc_status(ret);
        }
        int i = 0;
        for (LPSTR p = readers; p && *p; p += strlen(p) + 1, i++) {
            if (i == c->index) {
                snprintf(c->reader, sizeof(c->reader), "%s", p);
                break;
            }
        }
        SCardFreeMemory(c->ctx, readers);
        if (!c->reader[0]) {
            log_msg("card%d no reader at index %d", c->index, c->index);
            return -1;
        }
    }
    ret = SCardConnect(c->ctx, c->reader, SCARD_SHARE_EXCLUSIVE, SCARD_PROTOCOL_T1,
                       &c->card, &protocol);
    if (ret != SCARD_S_SUCCESS) {
        log_msg("card%d connect %s failed: %s", c->index, c->reader, pcsc_stringify_error(ret));
        return pcsc_status(ret);
    }
    c->atr_len = 0;
    if (SCardGetAttrib(c->card, SCARD_ATTR_ATR_STRING, c->atr, &atr_len) == SCARD_S_SUCCESS) {
        c->atr_len = (int)atr_len;
    }
    c->connected = 1;
    clear_caches(c);
    log_msg("card%d connected reader=%s", c->index, c->reader);
    return 0;
}

static void card_close_locked(Card *c)
{
    if (c->connected && c->card) {
        SCardDisconnect(c->card, SCARD_LEAVE_CARD);
    }
    if (c->ctx) {
        SCardReleaseContext(c->ctx);
    }
    c->card = 0;
    c->ctx = 0;
    c->connected = 0;
}

static int card_recover_locked(Card *c)
{
    int st;
    card_close_locked(c);
    st = card_open_locked(c);
    if (st) return st;
    pthread_mutex_lock(&g_lock);
    c->fault = 0;
    pthread_mutex_unlock(&g_lock);
    log_msg("card%d recovered", c->index);
    return 0;
}

static void card_fail_locked(Card *c, int status)
{
    (void)status;
    pthread_mutex_lock(&g_lock);
    c->fault = 1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *cl = &g_clients[i];
        if (cl->active && cl->card_index == c->index) {
            log_msg("card%d closing client %s", c->index, cl->name);
            shutdown(cl->fd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&g_lock);
    card_close_locked(c);
    clear_caches(c);
    c->errors++;
    log_msg("card%d fault", c->index);
}

static int card_transmit_locked(Card *c, const uint8_t *apdu, int len,
                                uint8_t *resp, int *resp_len)
{
    DWORD rlen = MAX_APDU;
    LONG ret;
    int st;

    if (!c->connected) {
        st = card_open_locked(c);
        if (st) return st;
    }
    ret = SCardTransmit(c->card, SCARD_PCI_T1, apdu, (DWORD)len, NULL, resp, &rlen);
    if (ret != SCARD_S_SUCCESS) {
        st = pcsc_status(ret);
        log_msg("card%d transmit failed: %s", c->index, pcsc_stringify_error(ret));
        card_fail_locked(c, st);
        return st;
    }
    if (rlen > MAX_APDU) return -5;
    c->apdu_count++;
    *resp_len = (int)rlen;
    return 0;
}

static CacheEntry *cache_find(CacheEntry *arr, int n, const uint8_t *apdu, int len, int64_t now)
{
    for (int i = 0; i < n; i++) {
        CacheEntry *e = &arr[i];
        if (!e->valid) continue;
        if (e->expires_ms <= now) {
            e->valid = 0;
            continue;
        }
        if (e->apdu_len == len && !memcmp(e->apdu, apdu, (size_t)len)) return e;
    }
    return NULL;
}

static void cache_store(CacheEntry *arr, int n, const uint8_t *apdu, int apdu_len,
                        const uint8_t *resp, int resp_len, int ttl_ms, int64_t now)
{
    int slot = -1;
    int64_t oldest = INT64_MAX;

    for (int i = 0; i < n; i++) {
        if (!arr[i].valid || arr[i].expires_ms <= now) {
            slot = i;
            break;
        }
        if (arr[i].expires_ms < oldest) {
            oldest = arr[i].expires_ms;
            slot = i;
        }
    }
    if (slot < 0) return;
    CacheEntry *e = &arr[slot];
    memcpy(e->apdu, apdu, (size_t)apdu_len);
    e->apdu_len = apdu_len;
    memcpy(e->resp, resp, (size_t)resp_len);
    e->resp_len = resp_len;
    e->expires_ms = now + ttl_ms;
    e->valid = 1;
}

static int be16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

static int ecm_purchased(const uint8_t *resp, int len)
{
    int rc;
    if (len < 25) return 0;
    rc = be16(resp + 4);
    return rc == 0x0800 || rc == 0x0400 || rc == 0x0200;
}

static int handle_frame(Card *c, Frame *req, Frame *resp)
{
    int st = 0;
    LONG ret;
    int rlen = 0;
    uint8_t answer[MAX_APDU];
    int fault;
    int ins;
    int64_t now;

    resp->operation = req->operation;
    resp->size = 0;
    resp->status = 0;

    if (req->operation == 1) {
        pthread_mutex_lock(&g_lock);
        fault = c->fault;
        pthread_mutex_unlock(&g_lock);
        pthread_mutex_lock(&c->lock);
        if (fault) {
            st = card_recover_locked(c);
        } else if (!c->connected) {
            st = card_open_locked(c);
        } else {
            /*
             * A reset has to mean reset.  torne-b25 sends RESET and then the
             * 90 30 init, and expects the card to have been power-cycled in
             * between: the CFW card path rejects a block when the firmware is
             * still holding the previous exchange (measured, 2026-11-21).
             * Leaving an already-open card alone made the init fail with
             * "Transaction failed" even though a fresh connection worked.
             */
            DWORD protocol = 0;

            ret = SCardReconnect(c->card, SCARD_SHARE_EXCLUSIVE,
                                 SCARD_PROTOCOL_T1, SCARD_RESET_CARD, &protocol);
            if (ret == SCARD_S_SUCCESS) {
                c->atr_len = 0;
                {
                    DWORD n = sizeof(c->atr);
                    if (SCardGetAttrib(c->card, SCARD_ATTR_ATR_STRING, c->atr, &n)
                        == SCARD_S_SUCCESS)
                        c->atr_len = (int)n;
                }
                clear_caches(c);
                c->fault = 0;
            } else {
                st = card_recover_locked(c);
                if (st) {
                    pthread_mutex_unlock(&c->lock);
                    return st;
                }
            }
        }
        pthread_mutex_unlock(&c->lock);
        return st;
    }
    if (req->operation != 2) return -6;
    if (req->size < 2 || req->size > MAX_APDU) return -6;

    pthread_mutex_lock(&g_lock);
    fault = c->fault;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_lock(&c->lock);
    if (fault) {
        st = card_recover_locked(c);
        if (st) {
            pthread_mutex_unlock(&c->lock);
            return st;
        }
    }

    ins = req->data[1];
    now = now_ms();

    if (ins == 0x30) {
        if (c->init_valid) {
            memcpy(answer, c->init_resp, (size_t)c->init_len);
            rlen = c->init_len;
            c->cache_hits++;
        } else {
            st = card_transmit_locked(c, req->data, (int)req->size, answer, &rlen);
            if (st) goto out;
            if (rlen >= 57 && be16(answer + 4) == 0x2100) {
                memcpy(c->init_resp, answer, (size_t)rlen);
                c->init_len = rlen;
                c->init_valid = 1;
            }
        }
    } else if (ins == 0x34) {
        CacheEntry *e = cache_find(c->ecm, c->ecm_count, req->data, (int)req->size, now);
        if (e) {
            memcpy(answer, e->resp, (size_t)e->resp_len);
            rlen = e->resp_len;
            c->cache_hits++;
        } else {
            st = card_transmit_locked(c, req->data, (int)req->size, answer, &rlen);
            if (st) goto out;
            if (ecm_purchased(answer, rlen)) {
                cache_store(c->ecm, c->ecm_count, req->data, (int)req->size,
                            answer, rlen, g_ecm_ttl_ms, now);
            }
        }
    } else if (ins == 0x36) {
        CacheEntry *e = cache_find(c->emm, c->emm_count, req->data, (int)req->size, now);
        if (e) {
            memcpy(answer, e->resp, (size_t)e->resp_len);
            rlen = e->resp_len;
            c->cache_hits++;
        } else {
            st = card_transmit_locked(c, req->data, (int)req->size, answer, &rlen);
            if (st) goto out;
            if (rlen >= 6 && be16(answer + 4) == 0x2100) {
                cache_store(c->emm, c->emm_count, req->data, (int)req->size,
                            answer, rlen, g_emm_ttl_ms, now);
                for (int i = 0; i < c->ecm_count; i++) c->ecm[i].valid = 0;
            }
        }
    } else {
        st = card_transmit_locked(c, req->data, (int)req->size, answer, &rlen);
        if (st) goto out;
    }

    if (rlen > MAX_APDU) {
        st = -5;
        goto out;
    }
    memcpy(resp->data, answer, (size_t)rlen);
    resp->size = (uint32_t)rlen;

out:
    pthread_mutex_unlock(&c->lock);
    return st;
}

static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static void client_cleanup(Client *cl)
{
    pthread_mutex_lock(&g_lock);
    if (cl->active) {
        cl->active = 0;
        if (g_active_clients > 0) g_active_clients--;
        if (cl->card_index >= 0 && cl->card_index < g_card_count &&
            g_cards[cl->card_index].clients > 0) {
            g_cards[cl->card_index].clients--;
        }
        if (cl->fd >= 0) close(cl->fd);
        cl->fd = -1;
        log_msg("client %s disconnected", cl->name);
    }
    pthread_mutex_unlock(&g_lock);
}

static void *client_thread(void *arg)
{
    Client *cl = arg;
    Card *c = &g_cards[cl->card_index];
    Frame req, resp;

    while (!g_stop) {
        if (read_full(cl->fd, &req, sizeof(req))) break;
        if (req.size > MAX_APDU) {
            memset(&resp, 0, sizeof(resp));
            resp.operation = req.operation;
            resp.status = -6;
        } else {
            memset(&resp, 0, sizeof(resp));
            resp.status = handle_frame(c, &req, &resp);
        }
        if (write_full(cl->fd, &resp, sizeof(resp))) break;
    }
    client_cleanup(cl);
    return NULL;
}

static int choose_card(void)
{
    int best = -1;
    int best_clients = INT32_MAX;

    for (int i = 0; i < g_card_count; i++) {
        Card *c = &g_cards[i];
        if (c->fault) continue;
        if (c->clients >= g_clients_per_card) continue;
        if (c->clients < best_clients) {
            best_clients = c->clients;
            best = i;
        }
    }
    if (best >= 0) return best;

    for (int i = 0; i < g_card_count; i++) {
        Card *c = &g_cards[i];
        if (!c->fault) continue;
        if (c->clients >= g_clients_per_card) continue;
        if (c->clients < best_clients) {
            best_clients = c->clients;
            best = i;
        }
    }
    return best;
}

static int send_frame(int fd, Frame *f)
{
    return write_full(fd, f, sizeof(*f));
}

static void handle_accept(void)
{
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    int fd;
    Frame hello, reply;
    char name[NAME_MAX] = "";
    Token token;
    int card_index;
    int slot = -1;
    struct timeval tv = { 2, 0 };

    fd = accept(g_listen_fd, (struct sockaddr *)&peer, &peer_len);
    if (fd < 0) return;
    if (!peer_allowed(&peer)) {
        log_msg("reject peer %s", inet_ntoa(peer.sin_addr));
        close(fd);
        return;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (read_full(fd, &hello, sizeof(hello)) || hello.operation != 0 || hello.size != 33) {
        close(fd);
        return;
    }
    if (hello.data[0] != 1) {
        memset(&reply, 0, sizeof(reply));
        reply.operation = 0;
        reply.status = -3;
        send_frame(fd, &reply);
        close(fd);
        return;
    }
    memcpy(token.token, hello.data + 1, 32);
    if (find_token(token.token, name, sizeof(name)) < 0) {
        memset(&reply, 0, sizeof(reply));
        reply.operation = 0;
        reply.status = -3;
        send_frame(fd, &reply);
        close(fd);
        return;
    }

    pthread_mutex_lock(&g_lock);
    if (g_active_clients >= g_max_clients) {
        pthread_mutex_unlock(&g_lock);
        memset(&reply, 0, sizeof(reply));
        reply.operation = 0;
        reply.status = -2;
        send_frame(fd, &reply);
        close(fd);
        return;
    }
    card_index = choose_card();
    if (card_index < 0) {
        pthread_mutex_unlock(&g_lock);
        memset(&reply, 0, sizeof(reply));
        reply.operation = 0;
        reply.status = -2;
        send_frame(fd, &reply);
        close(fd);
        return;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_lock);
        close(fd);
        return;
    }
    Client *cl = &g_clients[slot];
    memset(cl, 0, sizeof(*cl));
    cl->fd = fd;
    cl->active = 1;
    cl->card_index = card_index;
    snprintf(cl->name, sizeof(cl->name), "%s", name);
    memcpy(cl->token, token.token, 32);
    snprintf(cl->peer, sizeof(cl->peer), "%s", inet_ntoa(peer.sin_addr));
    g_active_clients++;
    g_cards[card_index].clients++;
    pthread_mutex_unlock(&g_lock);

    memset(&reply, 0, sizeof(reply));
    reply.operation = 0;
    reply.status = 0;
    reply.size = 0;
    send_frame(fd, &reply);

    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    log_msg("client %s from %s -> card%d", cl->name, cl->peer, card_index);
    if (pthread_create(&cl->thread, NULL, client_thread, cl) != 0) {
        client_cleanup(cl);
        return;
    }
    pthread_detach(cl->thread);
}

static int listen_socket(void)
{
    int fd, one = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_port);
    if (inet_pton(AF_INET, g_bind, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 8)) {
        close(fd);
        return -1;
    }
    return fd;
}

static void on_stop(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void on_hup(int sig)
{
    (void)sig;
    g_reload = 1;
}

static void on_usr1(int sig)
{
    (void)sig;
    g_recover_all = 1;
}

static int load_config(const char *path)
{
    FILE *f;
    char line[512];

    f = fopen(path, "r");
    if (!f) {
        log_msg("cannot open config %s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        char *eq;
        if (!*p || *p == '#') continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(p);
        char *val = trim(eq + 1);
        if (!strcmp(key, "bind")) snprintf(g_bind, sizeof(g_bind), "%s", val);
        else if (!strcmp(key, "port")) g_port = atoi(val);
        else if (!strcmp(key, "cards")) g_card_count = atoi(val);
        else if (!strcmp(key, "max_clients")) g_max_clients = atoi(val);
        else if (!strcmp(key, "clients_per_card")) g_clients_per_card = atoi(val);
        else if (!strcmp(key, "tokens")) snprintf(g_tokens_path, sizeof(g_tokens_path), "%s", val);
        else if (!strcmp(key, "allow")) {
            if (parse_allow(val)) {
                log_msg("invalid allow: %s", val);
                fclose(f);
                return -1;
            }
        } else if (!strcmp(key, "ecm_cache_ttl_ms")) g_ecm_ttl_ms = atoi(val);
        else if (!strcmp(key, "emm_cache_ttl_ms")) g_emm_ttl_ms = atoi(val);
        else if (!strcmp(key, "ecm_cache_entries")) g_ecm_entries = atoi(val);
        else if (!strcmp(key, "emm_cache_entries")) g_emm_entries = atoi(val);
        else if (!strncmp(key, "reader.", 7)) {
            int idx = atoi(key + 7);
            if (idx >= 0 && idx < MAX_CARDS) snprintf(g_cards[idx].reader, sizeof(g_cards[idx].reader), "%s", val);
        }
    }
    fclose(f);
    if (g_card_count < 1 || g_card_count > MAX_CARDS) return -1;
    if (g_max_clients < 1 || g_max_clients > MAX_CLIENTS) return -1;
    if (g_clients_per_card < 1) return -1;
    if (g_ecm_entries < 1 || g_ecm_entries > CACHE_MAX) return -1;
    if (g_emm_entries < 1 || g_emm_entries > CACHE_MAX) return -1;
    if (!g_allow_count) {
        log_msg("warning: allow is empty; all peers are accepted");
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *config = "/etc/bcas/bcasd.conf";
    struct sigaction sa;
    struct pollfd pfd;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) config = argv[++i];
        else {
            fprintf(stderr, "usage: %s --config PATH\n", argv[0]);
            return 2;
        }
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_hup;
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    memset(g_cards, 0, sizeof(g_cards));
    for (int i = 0; i < MAX_CARDS; i++) {
        g_cards[i].index = i;
        g_cards[i].clients = 0;
        g_cards[i].fault = 0;
        pthread_mutex_init(&g_cards[i].lock, NULL);
    }
    if (load_config(config)) return 1;
    for (int i = 0; i < g_card_count; i++) {
        g_cards[i].ecm_count = g_ecm_entries;
        g_cards[i].emm_count = g_emm_entries;
    }
    if (load_tokens()) return 1;

    g_listen_fd = listen_socket();
    if (g_listen_fd < 0) {
        log_msg("listen %s:%d failed: %s", g_bind, g_port, strerror(errno));
        return 1;
    }
    log_msg("bcasd listening on %s:%d cards=%d max_clients=%d", g_bind, g_port, g_card_count, g_max_clients);

    pfd.fd = g_listen_fd;
    pfd.events = POLLIN;
    while (!g_stop) {
        int r = poll(&pfd, 1, 1000);
        if (r < 0 && errno == EINTR) continue;
        if (g_reload) {
            g_reload = 0;
            load_tokens();
        }
        if (g_recover_all) {
            g_recover_all = 0;
            log_msg("forced recovery all cards");
            pthread_mutex_lock(&g_lock);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (g_clients[i].active) shutdown(g_clients[i].fd, SHUT_RDWR);
            }
            for (int i = 0; i < g_card_count; i++) g_cards[i].fault = 1;
            pthread_mutex_unlock(&g_lock);
        }
        if (r > 0 && (pfd.revents & POLLIN)) handle_accept();
    }

    log_msg("shutting down");
    close(g_listen_fd);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active) shutdown(g_clients[i].fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&g_lock);

    int active = -1;
    for (int i = 0; i < 40; i++) {
        pthread_mutex_lock(&g_lock);
        active = g_active_clients;
        pthread_mutex_unlock(&g_lock);
        if (active == 0) break;
        usleep(50000);
    }
    for (int i = 0; i < g_card_count; i++) {
        log_msg("stats card%d apdu=%llu cache_hits=%llu errors=%llu clients=%d",
                i, (unsigned long long)g_cards[i].apdu_count,
                (unsigned long long)g_cards[i].cache_hits,
                (unsigned long long)g_cards[i].errors, g_cards[i].clients);
    }
    if (active == 0) {
        for (int i = 0; i < g_card_count; i++) {
            pthread_mutex_lock(&g_cards[i].lock);
            card_close_locked(&g_cards[i]);
            pthread_mutex_unlock(&g_cards[i].lock);
            pthread_mutex_destroy(&g_cards[i].lock);
        }
    } else {
        log_msg("shutdown timeout: %d clients still active; pcscd will release the cards", active);
    }
    return 0;
}
