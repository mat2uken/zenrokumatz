#ifndef PIX_CM_QUEUE_H
#define PIX_CM_QUEUE_H
#include <stddef.h>
#include "cJSON.h"
#define PIX_CM_PRODUCER "pix-stream-recorder-cm-fmp4-v2"
int cm_write_all(int fd, const void *data, size_t size);
int cm_require_tmpfs_directory(const char *directory);
int cm_queue_submit(const char *directory, const char *id, const char *source,
                    const char *original_mp4, const char *original_json,
                    int keep_input, const cJSON *metadata);
#endif
