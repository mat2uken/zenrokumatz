/*
 * logo_store.h - 局ロゴの保存 (PNG + サイドカー + index.json)
 *
 * 1 サービス 1 PNG を <id>.png に置き、同じ内容を <id>.json に書く。
 * index.json はそのサイドカーを並べ直して作るので、複数の書き手
 * (929 の取得と、地上波 CDT の常時採取) が別々に走っても矛盾しない。
 */
#ifndef PIX_LOGO_STORE_H
#define PIX_LOGO_STORE_H

#include <stddef.h>
#include <stdint.h>

/* A transparent placeholder: TVTest ignores a logo this small. */
#define LOGO_STORE_TRANSPARENT_MAX 93

typedef struct {
    uint32_t id; /* original_network_id * 100000 + service_id (mirakc と同じ) */
    uint16_t network_id;
    uint16_t transport_stream_id;
    uint16_t service_id;
    const char *name;   /* NULL 可 */
    const uint8_t *png;
    size_t png_length;
    uint8_t logo_type;
    uint16_t logo_id;
} logo_output;

/*
 * Write the logos into dir. Files whose bytes did not change are left alone,
 * so a repeated run touches nothing (the SD card is not rewritten for the same
 * data). `system` is "bs", "cs" or "terrestrial" and only labels the entry.
 *
 * Returns the number of files written, or -1 with errno set.
 */
int logo_store_write(const char *dir, const logo_output *logos, size_t count, const char *system,
                     unsigned long if_hz, unsigned long tsid);

/* Create dir and its parents. 0 on success, -1 with errno set. */
int logo_store_mkdirs(const char *path);

#endif /* PIX_LOGO_STORE_H */
