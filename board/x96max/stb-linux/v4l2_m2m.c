/*
 * v4l2_m2m.c - 最小限の V4L2 mem2mem ラッパー実装
 *
 * エンコーダー（/dev/video1, meson-venc-h264）を想定し、出力側に生画像
 * (NV12M)、入力側に H.264 を置く。デコーダーにも同じコードで使えるよう、
 * フォーマットは設定で受け取る。
 */
#include "v4l2_m2m.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <poll.h>

struct V4l2M2m {
    int fd;
    uint32_t pixfmt_out;
    uint32_t pixfmt_cap;
    int width, height;
    int num_out, num_cap;
    V4l2Buffer out[8];
    V4l2Buffer cap[8];
    int out_free[8];
    int out_free_count;
    int out_queued;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static int set_format(V4l2M2m *m, uint32_t type, uint32_t pixfmt,
                      int width, int height, uint32_t *bytesperline)
{
    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = type;
    f.fmt.pix_mp.width = (uint32_t)width;
    f.fmt.pix_mp.height = (uint32_t)height;
    f.fmt.pix_mp.pixelformat = pixfmt;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;

    if (xioctl(m->fd, VIDIOC_S_FMT, &f) == -1) {
        fprintf(stderr, "[v4l2] S_FMT(type=%u) failed: %s\n", type, strerror(errno));
        return -1;
    }
    if (bytesperline)
        *bytesperline = f.fmt.pix_mp.plane_fmt[0].bytesperline;
    if ((int)f.fmt.pix_mp.width != width || (int)f.fmt.pix_mp.height != height) {
        fprintf(stderr, "[v4l2] driver changed the size to %ux%u\n",
                f.fmt.pix_mp.width, f.fmt.pix_mp.height);
    }
    return 0;
}

static int reqbufs(V4l2M2m *m, uint32_t type, V4l2Buffer *bufs, int count)
{
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = (uint32_t)count;
    rb.type = type;
    rb.memory = V4L2_MEMORY_MMAP;

    if (xioctl(m->fd, VIDIOC_REQBUFS, &rb) == -1) {
        fprintf(stderr, "[v4l2] REQBUFS(type=%u) failed: %s\n", type, strerror(errno));
        return -1;
    }
    if ((int)rb.count < count) {
        fprintf(stderr, "[v4l2] only %u buffers of %d granted\n", rb.count, count);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&b, 0, sizeof(b));
        memset(planes, 0, sizeof(planes));
        b.type = type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = (uint32_t)i;
        b.m.planes = planes;
        /*
         * ffmpeg の v4l2_buffers.c と同じ扱い。length には VIDEO_MAX_PLANES
         * を入れて呼び、ドライバが返した length から有効プレーン数を数える。
         * 先に実際のプレーン数へ絞ると meson の VB2 は EINVAL を返す。
         */
        b.length = VIDEO_MAX_PLANES;

        if (xioctl(m->fd, VIDIOC_QUERYBUF, &b) == -1) {
            fprintf(stderr, "[v4l2] QUERYBUF(%d) failed: %s\n", i, strerror(errno));
            return -1;
        }
        /*
         * プレーンごとに個別の mem_offset を持つ。まとめて mmap すると
         * 2 プレーン目以降のオフセットが無効になり EINVAL になる。
         */
        unsigned nplanes = 0;
        for (unsigned p = 0; p < b.length; p++)
            if (planes[p].length)
                nplanes++;

        size_t total = 0;
        for (unsigned p = 0; p < nplanes; p++)
            total += planes[p].length;

        /*
         * meson の VB2 実装は mem_offset に 0x40000000 のような大きな値を
         * 返す（物理アドレスではなく、ページ番号に近い形）。ページ境界に
         * そろえて mmap し、先頭の差分だけポインタを進めて使う。
         */
        off_t off = (off_t)planes[0].m.mem_offset;
        off_t aligned = off & ~(off_t)4095;
        size_t skew = (size_t)(off - aligned);

        void *base = mmap(NULL, total + skew, PROT_READ | PROT_WRITE,
                          MAP_SHARED, m->fd, aligned);
        void *addr = base;
        if (base == MAP_FAILED) {
            fprintf(stderr, "[v4l2] mmap(%d) failed: %s (off=%lld)\n",
                    i, strerror(errno), (long long)off);
            return -1;
        }
        bufs[i].addr = (uint8_t *)addr + skew;
        bufs[i].map_base = base;
        bufs[i].map_len = total + skew;
        bufs[i].length = total;
        bufs[i].index = i;
        bufs[i].bytesused = 0;
    }
    return 0;
}

static int queue_buf(V4l2M2m *m, uint32_t type, int index, size_t bytesused,
                     int64_t pts_us)
{
    struct v4l2_buffer b;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&b, 0, sizeof(b));
    memset(planes, 0, sizeof(planes));

    b.type = type;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = (uint32_t)index;
    b.m.planes = planes;
    b.length = VIDEO_MAX_PLANES;

    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        b.length = 1;
        planes[0].bytesused = (uint32_t)bytesused;
        b.bytesused = (uint32_t)bytesused;
    }
    if (pts_us >= 0) {
        b.timestamp.tv_sec = pts_us / 1000000;
        b.timestamp.tv_usec = pts_us % 1000000;
        b.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
    }

    if (xioctl(m->fd, VIDIOC_QBUF, &b) == -1) {
        fprintf(stderr, "[v4l2] QBUF(type=%u, idx=%d) failed: %s\n",
                type, index, strerror(errno));
        return -1;
    }
    return 0;
}

V4l2M2m *v4l2_m2m_open(const V4l2M2mConfig *cfg)
{
    if (!cfg || !cfg->device)
        return NULL;

    V4l2M2m *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->pixfmt_out = cfg->pixfmt_out;
    m->pixfmt_cap = cfg->pixfmt_cap;
    m->width = cfg->width;
    m->height = cfg->height;
    m->num_out = cfg->num_out > 0 ? cfg->num_out : 4;
    m->num_cap = cfg->num_cap > 0 ? cfg->num_cap : 4;
    if (m->num_out > 8) m->num_out = 8;
    if (m->num_cap > 8) m->num_cap = 8;

    m->fd = open(cfg->device, O_RDWR | O_NONBLOCK);
    if (m->fd < 0) {
        fprintf(stderr, "[v4l2] open(%s) failed: %s\n", cfg->device, strerror(errno));
        free(m);
        return NULL;
    }

    struct v4l2_capability cap;
    if (xioctl(m->fd, VIDIOC_QUERYCAP, &cap) == -1) {
        fprintf(stderr, "[v4l2] QUERYCAP failed: %s\n", strerror(errno));
        goto fail;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) {
        fprintf(stderr, "[v4l2] %s is not a multiplanar m2m device\n", cfg->device);
        goto fail;
    }

    if (set_format(m, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, cfg->pixfmt_out,
                   cfg->width, cfg->height, NULL) != 0)
        goto fail;
    if (set_format(m, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, cfg->pixfmt_cap,
                   cfg->width, cfg->height, NULL) != 0)
        goto fail;

    if (reqbufs(m, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, m->out, m->num_out) != 0)
        goto fail;
    if (reqbufs(m, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, m->cap, m->num_cap) != 0)
        goto fail;

    m->out_free_count = 0;
    for (int i = 0; i < m->num_out; i++)
        m->out_free[m->out_free_count++] = i;

    /* CAPTURE 側はすべて先にキューへ入れておく */
    for (int i = 0; i < m->num_cap; i++) {
        if (queue_buf(m, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, i, 0, -1) != 0)
            goto fail;
    }

    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(m->fd, VIDIOC_STREAMON, &t) == -1) {
        fprintf(stderr, "[v4l2] STREAMON(out) failed: %s\n", strerror(errno));
        goto fail;
    }
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(m->fd, VIDIOC_STREAMON, &t) == -1) {
        fprintf(stderr, "[v4l2] STREAMON(cap) failed: %s\n", strerror(errno));
        goto fail;
    }

    fprintf(stderr, "[v4l2] %s ready: out fmt %c%c%c%c, cap fmt %c%c%c%c, %dx%d\n",
            cfg->device,
            cfg->pixfmt_out & 0xff, (cfg->pixfmt_out >> 8) & 0xff,
            (cfg->pixfmt_out >> 16) & 0xff, (cfg->pixfmt_out >> 24) & 0xff,
            cfg->pixfmt_cap & 0xff, (cfg->pixfmt_cap >> 8) & 0xff,
            (cfg->pixfmt_cap >> 16) & 0xff, (cfg->pixfmt_cap >> 24) & 0xff,
            cfg->width, cfg->height);
    return m;

fail:
    close(m->fd);
    free(m);
    return NULL;
}

V4l2Buffer *v4l2_m2m_get_free_out(V4l2M2m *m)
{
    if (!m || m->out_free_count <= 0)
        return NULL;
    return &m->out[m->out_free[m->out_free_count - 1]];
}

int v4l2_m2m_queue_out(V4l2M2m *m, const void *data, size_t size,
                       int64_t pts_us, int keyframe)
{
    (void)keyframe;
    if (!m || m->out_free_count <= 0)
        return -1;

    int idx = m->out_free[--m->out_free_count];
    if (size > m->out[idx].length)
        size = m->out[idx].length;
    memcpy(m->out[idx].addr, data, size);

    if (queue_buf(m, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, idx, size, pts_us) != 0)
        return -1;
    m->out_queued++;
    return 0;
}

int v4l2_m2m_dequeue_cap(V4l2M2m *m, V4l2Buffer *out)
{
    if (!m)
        return -1;

    struct pollfd p = { .fd = m->fd, .events = POLLIN };
    int pr = poll(&p, 1, 200);
    if (pr <= 0)
        return 0;

    struct v4l2_buffer b;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(&b, 0, sizeof(b));
    memset(planes, 0, sizeof(planes));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.m.planes = planes;
    b.length = VIDEO_MAX_PLANES;

    if (xioctl(m->fd, VIDIOC_DQBUF, &b) == -1) {
        if (errno == EAGAIN)
            return 0;
        fprintf(stderr, "[v4l2] DQBUF(cap) failed: %s\n", strerror(errno));
        return -1;
    }

    out->index = (int)b.index;
    out->bytesused = planes[0].bytesused ? planes[0].bytesused : b.bytesused;
    out->addr = m->cap[b.index].addr;
    out->length = m->cap[b.index].length;

    /* 出力側のバッファが返ってきたら再利用可能にする */
    struct v4l2_buffer ob;
    struct v4l2_plane oplanes[VIDEO_MAX_PLANES];
    memset(&ob, 0, sizeof(ob));
    memset(oplanes, 0, sizeof(oplanes));
    ob.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ob.memory = V4L2_MEMORY_MMAP;
    ob.m.planes = oplanes;
    ob.length = 1;
    if (xioctl(m->fd, VIDIOC_DQBUF, &ob) == 0) {
        if (m->out_free_count < m->num_out)
            m->out_free[m->out_free_count++] = (int)ob.index;
        if (m->out_queued > 0)
            m->out_queued--;
    }
    return 1;
}

int v4l2_m2m_requeue_cap(V4l2M2m *m, V4l2Buffer *buf)
{
    if (!m || !buf)
        return -1;
    return queue_buf(m, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, buf->index, 0, -1);
}

int v4l2_m2m_drain(V4l2M2m *m)
{
    if (!m)
        return -1;
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(m->fd, VIDIOC_STREAMOFF, &t);
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(m->fd, VIDIOC_STREAMOFF, &t);
    return 0;
}

void v4l2_m2m_close(V4l2M2m *m)
{
    if (!m)
        return;
    for (int i = 0; i < m->num_out; i++)
        if (m->out[i].map_base)
            munmap(m->out[i].map_base, m->out[i].map_len);
    for (int i = 0; i < m->num_cap; i++)
        if (m->cap[i].map_base)
            munmap(m->cap[i].map_base, m->cap[i].map_len);
    close(m->fd);
    free(m);
}
