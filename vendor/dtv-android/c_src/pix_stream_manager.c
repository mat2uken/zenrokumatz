/*
 * pix-stream-manager: 管理APIと画面。
 *
 * STBの登録・削除、録画の開始・停止、状態の表示をまとめて扱う。
 * 既存の pix-archive や miraview とは別のポートで動き、それらには触れない。
 *
 * 依存は cJSON のみ。HTTPは自前実装で、同時に1リクエストずつ処理する。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/select.h>

#include "cJSON.h"

#define MAX_NODES 16
#define MAX_BODY 8192

/* An STB seen on the network, whether or not it is registered. */
typedef struct {
    char instance[128];
    char display[96];
    char ip[32];
    int port;
    int channel;
    time_t last_seen;
} Discovered;

#define MAX_DISCOVERED 32
static Discovered g_found[MAX_DISCOVERED];
static int g_found_count;
static pthread_mutex_t g_found_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char id[64];
    char name[128];
    char host[64];
    int port;
    int channel;
    int enabled;
    pid_t pid;   /* recorder child, 0 when stopped */
} Node;

static Node g_nodes[MAX_NODES];
static int g_node_count;
static char g_config_path[512];
static char g_recorder_path[512];
static char g_archive_url[256];
static char g_recordings_root[256];
static char g_tmpfs_dir[256];
static char g_state_dir[256];
static int g_bitrate;


/* ---- 自前のmDNSリスナ ---- */

#define MDNS_PORT 5353
#define MDNS_GROUP "224.0.0.251"
#define SERVICE "_pixstb._tcp.local"

/* Reads a possibly compressed DNS name starting at *off. */
static int read_name(const uint8_t *buf, size_t len, size_t *off,
                     char *out, size_t out_size) {
    size_t pos = *off;
    size_t written = 0;
    int jumped = 0;
    size_t after = 0;

    for (int guard = 0; guard < 64; guard++) {
        if (pos >= len) return -1;
        uint8_t c = buf[pos];
        if (c == 0) {
            pos++;
            break;
        }
        if ((c & 0xc0) == 0xc0) {
            if (pos + 1 >= len) return -1;
            size_t target = ((size_t)(c & 0x3f) << 8) | buf[pos + 1];
            if (!jumped) after = pos + 2;
            jumped = 1;
            pos = target;
            continue;
        }
        size_t n = c;
        if (pos + 1 + n > len) return -1;
        if (written && written + 1 < out_size) out[written++] = '.';
        for (size_t i = 0; i < n && written + 1 < out_size; i++) {
            out[written++] = (char)buf[pos + 1 + i];
        }
        pos += 1 + n;
    }
    out[written] = '\0';
    *off = jumped ? after : pos;
    return 0;
}

static int starts_with_service(const char *name) {
    return strncmp(name, SERVICE, strlen(SERVICE)) == 0;
}

/* Records the contents of one announcement. */
static void store_discovery(const char *instance, const char *display,
                            const char *ip, int port, int channel) {
    if (!instance[0] || !ip[0]) return;
    pthread_mutex_lock(&g_found_lock);
    int idx = -1;
    for (int i = 0; i < g_found_count; i++) {
        if (strcmp(g_found[i].instance, instance) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        if (g_found_count >= MAX_DISCOVERED) {
            pthread_mutex_unlock(&g_found_lock);
            return;
        }
        idx = g_found_count++;
        memset(&g_found[idx], 0, sizeof(g_found[idx]));
        snprintf(g_found[idx].instance, sizeof(g_found[idx].instance), "%s", instance);
        fprintf(stderr, "manager: discovered %s at %s\n", instance, ip);
    }
    if (display && display[0]) {
        size_t n = strlen(display);
        if (n >= sizeof(g_found[idx].display)) n = sizeof(g_found[idx].display) - 1;
        memcpy(g_found[idx].display, display, n);
        g_found[idx].display[n] = '\0';
    }
    {
        size_t n = strlen(ip);
        if (n >= sizeof(g_found[idx].ip)) n = sizeof(g_found[idx].ip) - 1;
        memcpy(g_found[idx].ip, ip, n);
        g_found[idx].ip[n] = '\0';
    }
    if (port) g_found[idx].port = port;
    if (channel) g_found[idx].channel = channel;
    g_found[idx].last_seen = time(NULL);
    pthread_mutex_unlock(&g_found_lock);
}

/* Parses one mDNS packet and folds anything useful into the list. */
static void parse_mdns(const uint8_t *buf, size_t len) {
    if (len < 12) return;
    uint16_t qd = (uint16_t)((buf[4] << 8) | buf[5]);
    uint16_t an = (uint16_t)((buf[6] << 8) | buf[7]);

    size_t off = 12;
    char name[160];
    for (int i = 0; i < qd; i++) {
        if (read_name(buf, len, &off, name, sizeof(name)) != 0) return;
        if (off + 4 > len) return;
        off += 4;                       /* type + class */
    }

    char instance[160] = {0};
    char target[160] = {0};
    int have_our_ptr = 0;
    char ip[32] = {0};
    char display[96] = {0};
    int port = 0, channel = 0;

    for (int i = 0; i < an; i++) {
        if (read_name(buf, len, &off, name, sizeof(name)) != 0) return;
        if (off + 10 > len) return;
        uint16_t type = (uint16_t)((buf[off] << 8) | buf[off + 1]);
        off += 4;                       /* type + class */
        off += 4;                       /* ttl */
        uint16_t rdlen = (uint16_t)((buf[off] << 8) | buf[off + 1]);
        off += 2;
        if (off + rdlen > len) return;
        size_t rdstart = off;

        if (type == 12) {               /* PTR */
            size_t p = rdstart;
            char ptr[160];
            /*
             * The owner name is the service ("_pixstb._tcp.local") and the
             * record data is the instance, so the two must be checked for
             * different things.
             */
            if (starts_with_service(name)
                && read_name(buf, len, &p, ptr, sizeof(ptr)) == 0) {
                have_our_ptr = 1;
                const char *dot = strstr(ptr, "._pixstb");
                size_t n = dot ? (size_t)(dot - ptr) : strlen(ptr);
                if (n >= sizeof(instance)) n = sizeof(instance) - 1;
                memcpy(instance, ptr, n);
                instance[n] = '\0';
            }
        } else if (type == 33) {        /* SRV */
            if (rdlen >= 6) {
                port = (buf[rdstart + 4] << 8) | buf[rdstart + 5];
                size_t p = rdstart + 6;
                read_name(buf, len, &p, target, sizeof(target));
                if (!instance[0]) {
                    const char *dot = strstr(name, "._pixstb");
                    if (dot) {
                        size_t n = (size_t)(dot - name);
                        if (n >= sizeof(instance)) n = sizeof(instance) - 1;
                        memcpy(instance, name, n);
                        instance[n] = '\0';
                    }
                }
            }
        } else if (type == 16) {        /* TXT */
            size_t p = rdstart;
            while (p < rdstart + rdlen) {
                uint8_t l = buf[p++];
                if (l == 0 || p + l > rdstart + rdlen) break;
                char item[128];
                size_t cp = l < sizeof(item) - 1 ? l : sizeof(item) - 1;
                memcpy(item, buf + p, cp);
                item[cp] = '\0';
                p += l;
                if (strncmp(item, "name=", 5) == 0) {
                    size_t n = strlen(item + 5);
                    if (n >= sizeof(display)) n = sizeof(display) - 1;
                    memcpy(display, item + 5, n);
                    display[n] = '\0';
                } else if (strncmp(item, "ch=", 3) == 0) {
                    channel = atoi(item + 3);
                } else if (strncmp(item, "ip=", 3) == 0) {
                    size_t n = strlen(item + 3);
                    if (n >= sizeof(ip)) n = sizeof(ip) - 1;
                    memcpy(ip, item + 3, n);
                    ip[n] = '\0';
                }
            }
        } else if (type == 1 && rdlen == 4) {   /* A */
            if (strcmp(name, target) == 0 || !ip[0]) {
                snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                         buf[rdstart], buf[rdstart + 1],
                         buf[rdstart + 2], buf[rdstart + 3]);
            }
        }
        off = rdstart + rdlen;
    }

    /* Only records belonging to our own service are interesting. */
    if (have_our_ptr && instance[0] && ip[0]) {
        store_discovery(instance, display, ip, port, channel);
    }
}

static void *mdns_listen_thread(void *arg) {
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NULL;
    int one = 1;
    /*
     * SO_REUSEADDR only. With SO_REUSEPORT the kernel spreads arriving
     * datagrams across every socket in the group, so this listener would see
     * only a fraction of the announcements.
     */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "manager: cannot bind mDNS port: %s\n", strerror(errno));
        close(fd);
        return NULL;
    }

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    fprintf(stderr, "manager: listening for %s announcements\n", SERVICE);

    for (;;) {
        uint8_t buf[2048];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &flen);
        if (n <= 0) continue;
        parse_mdns(buf, (size_t)n);
    }
    return NULL;
}

static void start_discovery(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, mdns_listen_thread, NULL) != 0) {
        fprintf(stderr, "manager: cannot start discovery: %s\n", strerror(errno));
        return;
    }
    pthread_detach(t);
}

static void load_config(void) {
    FILE *f = fopen(g_config_path, "rb");
    if (!f) {
        fprintf(stderr, "manager: no registry at %s, starting empty\n", g_config_path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 1024 * 1024) { fclose(f); return; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;
    cJSON *nodes = cJSON_GetObjectItem(root, "nodes");
    cJSON *item = NULL;
    g_node_count = 0;
    cJSON_ArrayForEach(item, nodes) {
        if (g_node_count >= MAX_NODES) break;
        Node *nd = &g_nodes[g_node_count];
        memset(nd, 0, sizeof(*nd));
        cJSON *v;
        v = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(v)) snprintf(nd->id, sizeof(nd->id), "%s", v->valuestring);
        v = cJSON_GetObjectItem(item, "name");
        if (cJSON_IsString(v)) snprintf(nd->name, sizeof(nd->name), "%s", v->valuestring);
        v = cJSON_GetObjectItem(item, "host");
        if (cJSON_IsString(v)) snprintf(nd->host, sizeof(nd->host), "%s", v->valuestring);
        v = cJSON_GetObjectItem(item, "port");
        nd->port = cJSON_IsNumber(v) ? (int)v->valuedouble : 40774;
        v = cJSON_GetObjectItem(item, "channel");
        nd->channel = cJSON_IsNumber(v) ? (int)v->valuedouble : 20;
        v = cJSON_GetObjectItem(item, "enabled");
        nd->enabled = cJSON_IsTrue(v) ? 1 : 0;
        if (nd->id[0] && nd->host[0]) g_node_count++;
    }
    cJSON_Delete(root);
    fprintf(stderr, "manager: loaded %d node(s)\n", g_node_count);
}

static void save_config(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");
    for (int i = 0; i < g_node_count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", g_nodes[i].id);
        cJSON_AddStringToObject(o, "name", g_nodes[i].name);
        cJSON_AddStringToObject(o, "host", g_nodes[i].host);
        cJSON_AddNumberToObject(o, "port", g_nodes[i].port);
        cJSON_AddNumberToObject(o, "channel", g_nodes[i].channel);
        cJSON_AddBoolToObject(o, "enabled", g_nodes[i].enabled);
        cJSON_AddItemToArray(nodes, o);
    }
    char *text = cJSON_Print(root);
    if (text) {
        char tmp[600];
        snprintf(tmp, sizeof(tmp), "%s.tmp", g_config_path);
        FILE *f = fopen(tmp, "w");
        if (f) {
            fputs(text, f);
            fclose(f);
            rename(tmp, g_config_path);
        }
        free(text);
    }
    cJSON_Delete(root);
}

static Node *find_node(const char *id) {
    for (int i = 0; i < g_node_count; i++) {
        if (strcmp(g_nodes[i].id, id) == 0) return &g_nodes[i];
    }
    return NULL;
}

static int node_running(const Node *nd) {
    if (nd->pid <= 0) return 0;
    int status = 0;
    pid_t r = waitpid(nd->pid, &status, WNOHANG);
    if (r == nd->pid || (r < 0 && errno == ECHILD)) return 0;
    return 1;
}

static void start_node(Node *nd) {
    if (node_running(nd)) return;
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "manager: fork failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        char channel[16], port[16], bitrate[16];
        snprintf(channel, sizeof(channel), "%d", nd->channel);
        snprintf(port, sizeof(port), "%d", nd->port);
        snprintf(bitrate, sizeof(bitrate), "%d", g_bitrate);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execl(g_recorder_path, g_recorder_path,
              "--stb-ip", nd->host,
              "--port", port,
              "--stream-id", channel,
              "--archive-url", g_archive_url,
              "--recordings-root", g_recordings_root,
              "--tmpfs-dir", g_tmpfs_dir,
              "--state-dir", g_state_dir,
              "--channel", channel,
              "--bitrate", bitrate,
              (char *)NULL);
        _exit(127);
    }
    nd->pid = pid;
    fprintf(stderr, "manager: started node %s (pid %d)\n", nd->id, (int)pid);
}

static void stop_node(Node *nd) {
    if (!node_running(nd)) {
        nd->pid = 0;
        return;
    }
    kill(nd->pid, SIGTERM);
    for (int i = 0; i < 20; i++) {
        if (!node_running(nd)) break;
        usleep(100 * 1000);
    }
    if (node_running(nd)) kill(nd->pid, SIGKILL);
    nd->pid = 0;
    fprintf(stderr, "manager: stopped node %s\n", nd->id);
}

/* 1秒で諦める短いHTTP取得。管理画面の表示を遅らせないため。 */
static int http_get(const char *host, int port, const char *path,
                    char *out, size_t out_size) {
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    char req[256];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                      path, host);
    if (send(fd, req, (size_t)rl, MSG_NOSIGNAL) != rl) {
        close(fd);
        return -1;
    }
    size_t total = 0;
    while (total + 1 < out_size) {
        ssize_t n = recv(fd, out + total, out_size - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
    }
    out[total] = '\0';
    close(fd);

    char *body = strstr(out, "\r\n\r\n");
    if (body) {
        body += 4;
        memmove(out, body, strlen(body) + 1);
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void send_response(int fd, int code, const char *type,
                          const char *body, size_t len) {
    char head[512];
    const char *reason = code == 200 ? "OK" : (code == 201 ? "Created"
                             : (code == 404 ? "Not Found" : "Bad Request"));
    int hl = snprintf(head, sizeof(head),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "Cache-Control: no-store\r\n"
                      "\r\n",
                      code, reason, type, len);
    write_all(fd, head, (size_t)hl);
    if (len) write_all(fd, body, len);
}

static cJSON *node_json(const Node *nd) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", nd->id);
    cJSON_AddStringToObject(o, "name", nd->name);
    cJSON_AddStringToObject(o, "host", nd->host);
    cJSON_AddNumberToObject(o, "port", nd->port);
    cJSON_AddNumberToObject(o, "channel", nd->channel);
    cJSON_AddBoolToObject(o, "enabled", nd->enabled != 0);
    cJSON_AddBoolToObject(o, "recording", node_running(nd) != 0);

    char raw[1024];
    char path[128];
    snprintf(path, sizeof(path), "/v1/status");
    if (http_get(nd->host, nd->port, path, raw, sizeof(raw)) == 0) {
        cJSON *st = cJSON_Parse(raw);
        if (st) {
            cJSON_AddItemToObject(o, "stb", st);
        } else {
            cJSON_AddStringToObject(o, "stbError", "unparsable status");
        }
    } else {
        cJSON_AddStringToObject(o, "stbError", "unreachable");
    }
    return o;
}

static const char *PAGE =
"<!doctype html><meta charset=\"utf-8\"><title>録画管理</title>\n"
"<style>\n"
"body{font-family:system-ui,sans-serif;margin:24px;background:#f6f7f9;color:#1b1d21}\n"
"h1{font-size:20px;margin:0 0 16px}\n"
"table{border-collapse:collapse;width:100%;background:#fff;box-shadow:0 1px 2px #0002}\n"
"th,td{padding:8px 10px;border-bottom:1px solid #e6e8ec;text-align:left;font-size:14px}\n"
"th{background:#eef0f3;font-weight:600}\n"
"button{padding:4px 10px;margin-right:4px;border:1px solid #c8ccd4;background:#fff;border-radius:4px;cursor:pointer;font-size:13px}\n"
"button:hover{background:#eef2ff}\n"
"button.danger{color:#b3261e}\n"
"form{margin-top:18px;background:#fff;padding:14px;border-radius:6px;box-shadow:0 1px 2px #0002}\n"
"input{padding:5px 8px;margin-right:8px;border:1px solid #c8ccd4;border-radius:4px}\n"
".on{color:#146c2e;font-weight:600}.off{color:#8a8f98}\n"
"</style>\n"
"<h1>STB録画管理</h1>\n"
"<table><thead><tr><th>名前</th><th>STB</th><th>ch</th><th>状態</th><th>録画</th><th>操作</th></tr></thead>\n"
"<tbody id=\"rows\"><tr><td colspan=6>読み込み中…</td></tr></tbody></table>\n"
"<h1 style=\"margin-top:26px;font-size:17px\">ネットワーク上のSTB（自動検出）</h1>\n"
"<table><thead><tr><th>名前</th><th>アドレス</th><th>ch</th><th>状態</th><th>操作</th></tr></thead>\n"
"<tbody id=\"found\"><tr><td colspan=5>検索中…</td></tr></tbody></table>\n"
"<form id=\"add\">\n"
"<b>STBを追加</b><br><br>\n"
"<input name=\"name\" placeholder=\"名前\" required>\n"
"<input name=\"host\" placeholder=\"192.168.99.57\" required>\n"
"<input name=\"port\" placeholder=\"40774\" value=\"40774\" size=6>\n"
"<input name=\"channel\" placeholder=\"20\" value=\"20\" size=4>\n"
"<button>追加</button>\n"
"</form>\n"
"<script>\n"
"async function load(){\n"
" const r=await fetch('/api/nodes');const ns=await r.json();\n"
" const tb=document.getElementById('rows');\n"
" if(!ns.length){tb.innerHTML='<tr><td colspan=6>登録されたSTBはありません</td></tr>';return;}\n"
" tb.innerHTML=ns.map(n=>{\n"
"  const st=n.stb||{};\n"
"  const cap=st.capturing?'<span class=on>録画中 ch'+st.channel+'</span>':'<span class=off>待機</span>';\n"
"  const rec=n.recording?'<span class=on>稼働</span>':'<span class=off>停止</span>';\n"
"  return '<tr><td>'+n.name+'</td><td>'+n.host+':'+n.port+'</td><td>'+n.channel+'</td>'\n"
"   +'<td>'+(n.stbError?('<span class=off>'+n.stbError+'</span>'):cap)+'</td><td>'+rec+'</td>'\n"
"   +'<td><button onclick=\"act(\\''+n.id+'\\',\\'start\\')\">開始</button>'\n"
"   +'<button onclick=\"act(\\''+n.id+'\\',\\'stop\\')\">停止</button>'\n"
"   +'<button class=danger onclick=\"del(\\''+n.id+'\\')\">削除</button></td></tr>';\n"
" }).join('');\n"
"}\n"
"async function act(id,a){await fetch('/api/nodes/'+id+'/'+a,{method:'POST'});load();}\n"
"async function del(id){if(!confirm('削除しますか'))return;await fetch('/api/nodes/'+id,{method:'DELETE'});load();}\n"
"document.getElementById('add').onsubmit=async e=>{\n"
" e.preventDefault();const f=new FormData(e.target);\n"
" await fetch('/api/nodes',{method:'POST',headers:{'Content-Type':'application/json'},\n"
"  body:JSON.stringify({name:f.get('name'),host:f.get('host'),port:+f.get('port'),channel:+f.get('channel'),enabled:true})});\n"
" e.target.reset();load();};\n"
"async function loadFound(){\n"
" const r=await fetch('/api/discovered');const ds=await r.json();\n"
" const tb=document.getElementById('found');\n"
" if(!ds.length){tb.innerHTML='<tr><td colspan=5>見つかりません。STBの電源とネットワークを確認してください</td></tr>';return;}\n"
" tb.innerHTML=ds.map(d=>{\n"
"  const nm=d.name||d.instance;\n"
"  const state=d.online?'<span class=on>オンライン</span>':'<span class=off>応答なし</span>';\n"
"  const act=d.known?'<span class=off>登録済み</span>'\n"
"   :'<button onclick=\\\"addFound(\\\''+d.host+'\\\','+d.port+','+d.channel+',\\\''+nm+'\\\')\\\">追加</button>';\n"
"  return '<tr><td>'+nm+'</td><td>'+d.host+':'+d.port+'</td><td>'+d.channel+'</td><td>'+state+'</td><td>'+act+'</td></tr>';\n"
" }).join('');\n"
"}\n"
"async function addFound(host,port,ch,name){\n"
" await fetch('/api/nodes',{method:'POST',headers:{'Content-Type':'application/json'},\n"
"  body:JSON.stringify({name:name||'STB',host:host,port:port,channel:ch,enabled:true})});\n"
" load();loadFound();\n"
"}\n"
"load();loadFound();setInterval(()=>{load();loadFound();},3000);\n"
"</script>\n";

static void handle_add(int fd, const char *body) {
    cJSON *o = cJSON_Parse(body);
    if (!o) {
        send_response(fd, 400, "text/plain", "bad json\n", 9);
        return;
    }
    if (g_node_count >= MAX_NODES) {
        cJSON_Delete(o);
        send_response(fd, 400, "text/plain", "too many nodes\n", 15);
        return;
    }
    Node *nd = &g_nodes[g_node_count];
    memset(nd, 0, sizeof(*nd));
    cJSON *v = cJSON_GetObjectItem(o, "name");
    snprintf(nd->name, sizeof(nd->name), "%s", cJSON_IsString(v) ? v->valuestring : "STB");
    v = cJSON_GetObjectItem(o, "host");
    if (!cJSON_IsString(v) || !v->valuestring[0]) {
        cJSON_Delete(o);
        send_response(fd, 400, "text/plain", "host is required\n", 17);
        return;
    }
    snprintf(nd->host, sizeof(nd->host), "%s", v->valuestring);
    v = cJSON_GetObjectItem(o, "port");
    nd->port = cJSON_IsNumber(v) ? (int)v->valuedouble : 40774;
    v = cJSON_GetObjectItem(o, "channel");
    nd->channel = cJSON_IsNumber(v) ? (int)v->valuedouble : 20;
    v = cJSON_GetObjectItem(o, "enabled");
    nd->enabled = cJSON_IsTrue(v) ? 1 : 0;

    /* ID はホストとチャンネルから作る。同じ組み合わせは重複させない。 */
    {
        char new_id[96];
        snprintf(new_id, sizeof(new_id), "%s-%d", nd->host, nd->channel);
        memcpy(nd->id, new_id, sizeof(nd->id));
        nd->id[sizeof(nd->id) - 1] = '\0';
    }
    for (int i = 0; i < g_node_count; i++) {
        if (strcmp(g_nodes[i].id, nd->id) == 0) {
            cJSON_Delete(o);
            send_response(fd, 400, "text/plain", "already registered\n", 18);
            return;
        }
    }
    g_node_count++;
    cJSON_Delete(o);
    save_config();
    if (nd->enabled) start_node(nd);
    send_response(fd, 201, "application/json", "{\"ok\":true}\n", 12);
    fprintf(stderr, "manager: added node %s\n", nd->id);
}

static void handle_request(int fd) {
    char buf[MAX_BODY + 4096];
    size_t total = 0;
    /* ヘッダ終端まで読む */
    while (total + 1 < sizeof(buf)) {
        ssize_t n = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    if (total == 0) return;

    char method[16] = {0}, uri[256] = {0}, proto[16] = {0};
    if (sscanf(buf, "%15s %255s %15s", method, uri, proto) != 3) return;

    /* Content-Length 分の本文を読む */
    static char body[MAX_BODY];
    body[0] = '\0';
    char *sep = strstr(buf, "\r\n\r\n");
    if (sep) {
        size_t have = total - (size_t)(sep + 4 - buf);
        int want = 0;
        char *cl = strcasestr(buf, "content-length:");
        if (cl) want = atoi(cl + 15);
        if (want > 0 && (size_t)want < sizeof(body)) {
            size_t copied = have < (size_t)want ? have : (size_t)want;
            memcpy(body, sep + 4, copied);
            while (copied < (size_t)want) {
                ssize_t n = recv(fd, body + copied, (size_t)want - copied, 0);
                if (n <= 0) break;
                copied += (size_t)n;
            }
            body[copied] = '\0';
        }
    }

    if (strcmp(method, "GET") == 0 && strcmp(uri, "/") == 0) {
        send_response(fd, 200, "text/html; charset=utf-8", PAGE, strlen(PAGE));
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(uri, "/api/nodes") == 0) {
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < g_node_count; i++) {
            cJSON_AddItemToArray(arr, node_json(&g_nodes[i]));
        }
        char *text = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        if (text) {
            send_response(fd, 200, "application/json", text, strlen(text));
            free(text);
        }
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(uri, "/api/discovered") == 0) {
        cJSON *arr = cJSON_CreateArray();
        time_t now = time(NULL);
        pthread_mutex_lock(&g_found_lock);
        for (int i = 0; i < g_found_count; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "instance", g_found[i].instance);
            cJSON_AddStringToObject(o, "name", g_found[i].display);
            cJSON_AddStringToObject(o, "host", g_found[i].ip);
            cJSON_AddNumberToObject(o, "port", g_found[i].port ? g_found[i].port : 40774);
            cJSON_AddNumberToObject(o, "channel", g_found[i].channel ? g_found[i].channel : 20);
            cJSON_AddNumberToObject(o, "ageSec", (double)(now - g_found[i].last_seen));
            cJSON_AddBoolToObject(o, "online", (now - g_found[i].last_seen) < 90);
            /* 登録済みかどうか */
            char id[64];
            snprintf(id, sizeof(id), "%s-%d", g_found[i].ip,
                     g_found[i].channel ? g_found[i].channel : 20);
            cJSON_AddBoolToObject(o, "known", find_node(id) != NULL);
            cJSON_AddItemToArray(arr, o);
        }
        pthread_mutex_unlock(&g_found_lock);
        char *text = cJSON_PrintUnformatted(arr);
        cJSON_Delete(arr);
        if (text) {
            send_response(fd, 200, "application/json", text, strlen(text));
            free(text);
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(uri, "/api/nodes") == 0) {
        handle_add(fd, body);
        return;
    }
    if (strncmp(uri, "/api/nodes/", 11) == 0) {
        char id[64] = {0};
        char action[32] = {0};
        /* /api/nodes/<id> または /api/nodes/<id>/<action> */
        const char *p = uri + 11;
        const char *slash = strchr(p, '/');
        if (slash) {
            size_t n = (size_t)(slash - p);
            if (n >= sizeof(id)) n = sizeof(id) - 1;
            memcpy(id, p, n);
            id[n] = '\0';
            snprintf(action, sizeof(action), "%s", slash + 1);
        } else {
            size_t n = strlen(p);
            if (n >= sizeof(id)) n = sizeof(id) - 1;
            memcpy(id, p, n);
            id[n] = '\0';
        }

        Node *nd = find_node(id);
        if (!nd) {
            send_response(fd, 404, "text/plain", "not found\n", 10);
            return;
        }
        if (strcmp(method, "DELETE") == 0 && !action[0]) {
            stop_node(nd);
            int idx = (int)(nd - g_nodes);
            for (int i = idx; i + 1 < g_node_count; i++) g_nodes[i] = g_nodes[i + 1];
            g_node_count--;
            save_config();
            send_response(fd, 200, "application/json", "{\"ok\":true}\n", 12);
            fprintf(stderr, "manager: removed node %s\n", id);
            return;
        }
        if (strcmp(method, "POST") == 0 && strcmp(action, "start") == 0) {
            nd->enabled = 1;
            start_node(nd);
            save_config();
            send_response(fd, 200, "application/json", "{\"ok\":true}\n", 12);
            return;
        }
        if (strcmp(method, "POST") == 0 && strcmp(action, "stop") == 0) {
            nd->enabled = 0;
            stop_node(nd);
            save_config();
            send_response(fd, 200, "application/json", "{\"ok\":true}\n", 12);
            return;
        }
    }
    send_response(fd, 404, "text/plain", "not found\n", 10);
}

int main(int argc, char **argv) {
    int port = 40776;
    snprintf(g_config_path, sizeof(g_config_path), "/home/ku/pix-stream-20261101/nodes.json");
    snprintf(g_recorder_path, sizeof(g_recorder_path), "/home/ku/pix-stream-20261101/pix_stream_recorder");
    snprintf(g_archive_url, sizeof(g_archive_url), "http://192.168.99.80:40773");
    snprintf(g_recordings_root, sizeof(g_recordings_root), "/mnt/xblk/video/pix-smb400");
    snprintf(g_tmpfs_dir, sizeof(g_tmpfs_dir), "/dev/shm/pix_stream");
    snprintf(g_state_dir, sizeof(g_state_dir), "/home/ku/pix-stream-20261101");
    g_bitrate = 5000000;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) snprintf(g_config_path, sizeof(g_config_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--recorder") && i + 1 < argc) snprintf(g_recorder_path, sizeof(g_recorder_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--archive-url") && i + 1 < argc) snprintf(g_archive_url, sizeof(g_archive_url), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--recordings-root") && i + 1 < argc) snprintf(g_recordings_root, sizeof(g_recordings_root), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--tmpfs-dir") && i + 1 < argc) snprintf(g_tmpfs_dir, sizeof(g_tmpfs_dir), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--state-dir") && i + 1 < argc) snprintf(g_state_dir, sizeof(g_state_dir), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) g_bitrate = atoi(argv[++i]);
    }

    signal(SIGPIPE, SIG_IGN);
    /*
     * SIGCHLD must not be ignored: ignoring it makes the kernel reap the
     * children, after which waitpid can no longer tell a running recorder from
     * a finished one and every node looks stopped.
     */

    load_config();
    start_discovery();

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    listen(listen_fd, 16);
    /* The loop also supervises the recorders, so accept must not block forever. */
    struct timeval poll_tv = { .tv_sec = 0, .tv_usec = 500 * 1000 };
    setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &poll_tv, sizeof(poll_tv));
    fprintf(stderr, "manager: listening on 0.0.0.0:%d, registry %s\n", port, g_config_path);

    /* Start the nodes that were enabled when the manager last stopped. */
    for (int i = 0; i < g_node_count; i++) {
        if (g_nodes[i].enabled) start_node(&g_nodes[i]);
    }

    time_t next_check = 0;
    for (;;) {
        /*
         * Bring an enabled recorder back after it stopped on its own, for
         * example because the STB rebooted underneath it. This is what makes
         * the registration survive a power cycle on either side.
         */
        time_t now = time(NULL);
        if (now >= next_check) {
            next_check = now + 10;
            for (int i = 0; i < g_node_count; i++) {
                if (g_nodes[i].enabled && !node_running(&g_nodes[i])) {
                    fprintf(stderr, "manager: node %s stopped, restarting\n", g_nodes[i].id);
                    start_node(&g_nodes[i]);
                }
            }
        }

        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(200 * 1000);
                continue;
            }
            break;
        }
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        handle_request(fd);
        close(fd);
    }
    close(listen_fd);
    return 0;
}
