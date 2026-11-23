"""Native harnesses for the production programme-continuation decision.

Both cases compile the real implementation text. The streaming side is the
boundary decision itself; the EIT side is the real section handler fed by a
small synthetic section builder. Only I/O, the ARIB decoder and the state file
are replaced, so the decision logic runs as written.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STREAMER = ROOT / "package/stb-streamer/src/stb_linux_pipe.c"
EIT = ROOT / "package/stb-streamer/src/eit_watch.c"


def extract(text, start, end):
    at = text.index(start)
    return text[at:text.index(end, at)]


BOUNDARY_HARNESS = r"""
int main(void) {
    (void)keep_read_program_file;
    printf("RESULT\n");
    struct { int id; long long start; int continues; int stops; int changed; } cases[] = {
        /* A repeated event id is never a new programme. */
        {100, 1794967250000LL, 1, 0, 0},
        /* Another event of a continued programme keeps one segment: this is
         * the shape that produced the 203040..203320 split. */
        {101, 1794967250000LL, 1, 0, 0},
        /* A real change: later start, no continuation marker. */
        {102, 1794987600000LL, 0, 3, 1},
        /* Continuation was seen, so later 0x00 sections do not break it. */
        {103, 1794967250000LL, 1, 1, 0},
        {104, 1794967250000LL, 1, 4, 0},
        /* The next programme starts later without a marker. */
        {107, 1794991800000LL, 0, 0, 1},
        /* Same start reported by another event is a name fluctuation. */
        {110, 1794991800000LL, 0, 0, 0},
        /* Without a start time only the event id change decides. */
        {109, 0, 0, 0, 1},
        /* The next programme starts later and is not marked as continuing. */
        {111, 1795000000000LL, 0, 0, 1},
    };
    for (unsigned i = 1; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ProgramInfo previous = {0}, current = {0};
        previous.service_id = current.service_id = 35864;
        previous.event_id = cases[i - 1].id;
        previous.start_ms = cases[i - 1].start;
        previous.continues = cases[i - 1].continues;
        previous.stops = cases[i - 1].stops;
        current.event_id = cases[i].id;
        current.start_ms = cases[i].start;
        current.continues = cases[i].continues;
        current.stops = cases[i].stops;
        int changed = program_changed(&previous, &current);
        printf("%d:%s:%d\n", cases[i].id, changed ? "changed" : "same",
               changed == cases[i].changed);
    }
    return 0;
}
"""

EIT_SUPPORT = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PROGRAM_NAME_MAX 256
typedef struct {
    int service_id;
    int event_id;
    int64_t start_ms;
    int64_t duration_ms;
    int64_t wall_ms;
    char name[PROGRAM_NAME_MAX];
    int continues;
    int stops;
    int valid;
} Program;
static int g_pmt_pid = -1;
static int g_service_id = -1;
static Program g_last;
static const char *name_cache_get(int event) { (void)event; return ""; }
static void name_cache_put(int event, const char *name) { (void)event; (void)name; }
/* The schedule cache is replaced here; this harness owns the marker decision. */
static void epg_put(int event, long long start_ms, long long duration_ms,
                    const char *name) {
    (void)event; (void)start_ms; (void)duration_ms; (void)name;
}
static void event_name(const unsigned char *s, size_t o, size_t len,
                       char *out, size_t cap) {
    (void)s; (void)o; (void)len; out[0] = 0; (void)cap;
}

static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int k = 0; k < 8; k++)
            c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
        crc_table[i] = c;
    }
}

static void finish(unsigned char *p, size_t body) {
    size_t length = body - 3 + 4;
    p[1] = (unsigned char)(0xb0 | (length >> 8));
    p[2] = (unsigned char)length;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < body; i++)
        crc = (crc << 8) ^ crc_table[((crc >> 24) ^ p[i]) & 0xFF];
    p[body] = (unsigned char)(crc >> 24);
    p[body + 1] = (unsigned char)(crc >> 16);
    p[body + 2] = (unsigned char)(crc >> 8);
    p[body + 3] = (unsigned char)crc;
}

/* One EIT section with a single event, laid out exactly as the production
 * handler reads it: the 14 byte header holds the event loop length, the event
 * record follows at byte 16 with its start at 18 and duration at 23, and the
 * descriptor loop holds one short_event_descriptor. */
static size_t eit(unsigned char *p, int service, int event, int section,
                  int segment_last) {
    static const unsigned char header[14] = {
        0x4e, 0xb0, 0x00,             /* table_id, section_length (set below) */
        0x8c, 0x18,                   /* service_id 35864 */
        0x01,                         /* version 1, current */
        0x00, 0x03,                   /* section_number, last_section_number */
        0x7d, 0xc3,                   /* transport_stream_id */
        0x00,                         /* segment_last_section_number */
        0x7d, 0xc3,                   /* original_network_id */
        0x4e                          /* last_table_id */
    };
    static const unsigned char body[] = {
        0x06, 0x3f,                   /* event 1599 */
        0xef, 0xb2,                   /* MJD 61362 = 2026-11-18 */
        0x09, 0x00, 0x00,             /* start 09:00:00 */
        0x02, 0x00, 0x00,             /* duration 02:00:00 */
        0x00, 0x07,                   /* descriptor loop length */
        0x4d, 0x06,                   /* short_event_descriptor */
        'j', 'p', 'n', 0x03,          /* language, name length */
        'N', 'O', 'W'
    };
    /* The event record goes at 14 so the production handler's offsets for the
     * event, its start (i + 2) and its duration (i + 7) line up. */
    memset(p, 0, 512);
    memcpy(p, header, sizeof(header));
    p[10] = (unsigned char)segment_last;
    p[6] = (unsigned char)section;
    p[14] = (unsigned char)(event >> 8);
    p[15] = (unsigned char)event;
    memcpy(p + 16, body + 2, sizeof(body) - 2);
    finish(p, 14 + sizeof(body) - 2);
    return 12 + sizeof(body);
}

static void handle_pat(const unsigned char *sec, size_t len);
static int handle_eit(const unsigned char *sec, size_t len, int64_t wall_ms);
"""

EIT_HARNESS = r"""
    unsigned char sec[512];
    size_t length;
    /* 09:30:12 JST, inside the programme that starts at 09:00. */
    const int64_t wall = 1794965412000LL;

    unsigned char pat[32];
    memset(pat, 0, sizeof(pat));
    pat[1] = 0xb0;
    unsigned char map[] = {0xc1, 0x00, 0x00, 0x8c, 0x18, 0xf1, 0x01};
    memcpy(pat + 5, map, sizeof(map));
    finish(pat, 5 + sizeof(map));
    handle_pat(pat, 5 + sizeof(map) + 4);
    printf("service=%d\n", g_service_id);

    length = eit(sec, 35864, 1599, 0, 3);
    handle_eit(sec, length, wall);
    printf("continue=%d stops=%d\n", g_last.continues, g_last.stops);

    /* The same programme's next section keeps it continuing. */
    length = eit(sec, 35864, 1599, 1, 3);
    handle_eit(sec, length, wall);
    printf("again=%d stops=%d\n", g_last.continues, g_last.stops);

    /* A long programme repeats its 0x00 first section; it keeps running. */
    for (int i = 0; i < 5; i++) {
        length = eit(sec, 35864, 1599, 0, 0);
        handle_eit(sec, length, wall);
    }
    printf("zero_run=%d stops=%d\n", g_last.continues, g_last.stops);
    return 0;
"""


class BoundaryDecisionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="pix-boundary-")
        cls.addClassCleanup(cls.tmp.cleanup)
        base = Path(cls.tmp.name)
        text = STREAMER.read_text()
        source = base / "boundary.c"
        source.write_text(
            "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
            "typedef struct { int service_id; int event_id; long long start_ms;\n"
            " long long duration_ms; long long wall_ms; char name[256];\n"
            " int continues; int stops; int valid; } ProgramInfo;\n"
            + extract(text, "static int program_changed(", "static void read_program_file(")
            + extract(text, "static void read_program_file(",
                      "/*\n * moof の中に映像トラック")
            + "static void *keep_read_program_file = (void *)read_program_file;\n"
            + BOUNDARY_HARNESS)
        cls.binary = base / "boundary"
        result = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                 "-o", str(cls.binary), str(source)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def test_boundary_decisions(self):
        result = subprocess.run([str(self.binary)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        for line in result.stdout.splitlines()[1:]:
            self.assertTrue(line.endswith(":1"), line)
        self.assertEqual(result.stdout.count(":same:1"), 4)
        self.assertEqual(result.stdout.count(":changed:1"), 4)


class EitMarkerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="pix-eit-marker-")
        cls.addClassCleanup(cls.tmp.cleanup)
        base = Path(cls.tmp.name)
        text = EIT.read_text()
        parts = [
            extract(text, "static int64_t mjd_bcd_to_ms(", "static int64_t bcd_duration_ms(")
            + extract(text, "static int64_t bcd_duration_ms(", "/* ---- PSI/EIT ---- */"),
            extract(text, "/* PAT から最初の実プログラム",
                    "/*\n * 再起動すると g_last が空になり"),
            extract(text, "static int handle_eit(", "/* ---- 入力 ---- */"),
        ]
        source = base / "eit_marker.c"
        source.write_text(EIT_SUPPORT + "\n" + "\n".join(parts)
                          + "\nint main(void) {\n" + EIT_HARNESS + "}\n")
        cls.binary = base / "eit_marker"
        result = subprocess.run(["cc", "-std=c11", "-Wall", "-o", str(cls.binary),
                                 str(source)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def test_marker_survives_zero_sections(self):
        result = subprocess.run([str(self.binary)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("service=35864\n", result.stdout)
        self.assertIn("continue=1 stops=0\n", result.stdout)
        self.assertIn("again=1 stops=0\n", result.stdout)
        self.assertIn("zero_run=1 stops=5\n", result.stdout)


if __name__ == "__main__":
    unittest.main()
