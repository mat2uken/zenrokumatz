/*
 * arib_text.c - ARIB STD-B24 text to UTF-8 for the programme name in the EIT.
 *
 * The broadcast carries the programme title as an 8-bit character stream:
 * GL (0x21-0x7E) and GR (0xA1-0xFE) hold whichever set is invoked there, and
 * escape sequences switch the sets. Two-byte sets (kanji) consume the second
 * byte from the same area. Only the pieces EIT titles use are implemented:
 * kanji, alphanumerics, hiragana and katakana. DRCS (outer font) and the
 * ARIB-only icon rows are dropped, which is what we want for a file name.
 *
 * The decoder follows ARIB STD-B24 Vol.1 Part 2 Chapter 7:
 *   0x0E LS1, 0x0F LS0, 0x19 SS2, 0x1D SS3
 *   ESC 2/4 F designate a two-byte set, ESC 2/8..2/11 F a one-byte set
 *   ESC 6/14 LS2, 6/15 LS3, 7/12 LS3R, 7/13 LS2R, 7/14 LS1R
 * Initial state is G0=kanji, G1=alnum, G2=hiragana, G3=katakana, GL=G0,
 * GR=G2, which is what every Japanese broadcaster assumes.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "arib_table.h"
#include "arib_text.h"

enum arib_set {
    ARIB_SET_KANJI,
    ARIB_SET_ALNUM,
    ARIB_SET_HIRAGANA,
    ARIB_SET_KATAKANA,
    ARIB_SET_SKIP,
};

typedef struct {
    enum arib_set g[4];
    int gl;
    int gr;
    enum arib_set single;
    int have_single;
} AribState;

static size_t put_utf8(uint32_t cp, char *dst, size_t cap, size_t used)
{
    unsigned char b[3];
    size_t n;

    if (cp == 0)
        return used;
    if (cp < 0x80) {
        b[0] = (unsigned char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (unsigned char)(0xC0 | (cp >> 6));
        b[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    } else {
        b[0] = (unsigned char)(0xE0 | (cp >> 12));
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    }
    /* Never split a character: a truncated tail would not decode anywhere. */
    if (used + n >= cap)
        return used;
    memcpy(dst + used, b, n);
    return used + n;
}

static enum arib_set set_from_designation(uint8_t f)
{
    switch (f) {
    case 0x30:
    case 0x37:
        return ARIB_SET_HIRAGANA;
    case 0x31:
    case 0x38:
        return ARIB_SET_KATAKANA;
    case 0x32:
    case 0x36:
    case 0x4A:
        return ARIB_SET_ALNUM;
    case 0x39:
    case 0x3B:
    case 0x41:
    case 0x42:
    case 0x4B:
        return ARIB_SET_KANJI;
    default:
        /* DRCS and the like: consume the characters, emit nothing. */
        return ARIB_SET_SKIP;
    }
}

static uint32_t alnum_char(uint8_t c)
{
    if (c == 0x5C)
        return 0x00A5;              /* yen sign, not a backslash */
    if (c == 0x7E)
        return 0x203E;              /* overline */
    return (uint32_t)(0xFF01 + (c - 0x21));
}

size_t arib_text_to_utf8(const uint8_t *src, size_t len, char *dst, size_t cap)
{
    AribState st = {
        .g = {ARIB_SET_KANJI, ARIB_SET_ALNUM, ARIB_SET_HIRAGANA, ARIB_SET_KATAKANA},
        .gl = 0,
        .gr = 2,
        .single = ARIB_SET_SKIP,
        .have_single = 0,
    };
    size_t i = 0, used = 0;

    if (!dst || cap == 0)
        return 0;
    dst[0] = '\0';

    while (i < len) {
        uint8_t c = src[i];
        enum arib_set set;
        uint8_t gl;

        if (c == 0x1B) {                        /* ESC */
            uint8_t code;
            i++;
            if (i >= len)
                break;
            code = src[i];
            if (code == 0x24 || (code >= 0x28 && code <= 0x2B)) {
                int which = 0;
                if (code == 0x24) {
                    i++;
                    if (i < len && src[i] >= 0x28 && src[i] <= 0x2B) {
                        which = src[i] - 0x28;
                        i++;
                    }
                } else {
                    which = code - 0x28;
                    i++;
                }
                if (i < len) {
                    st.g[which] = set_from_designation(src[i]);
                    i++;
                }
                continue;
            }
            if (code == 0x6E) { st.gl = 2; i++; continue; }   /* LS2 */
            if (code == 0x6F) { st.gl = 3; i++; continue; }   /* LS3 */
            if (code == 0x7C) { st.gr = 3; i++; continue; }   /* LS3R */
            if (code == 0x7D) { st.gr = 2; i++; continue; }   /* LS2R */
            if (code == 0x7E) { st.gr = 1; i++; continue; }   /* LS1R */
            i++;                                              /* C1 selection et al. */
            continue;
        }
        if (c == 0x0E) { st.gl = 1; i++; continue; }          /* LS1 */
        if (c == 0x0F) { st.gl = 0; i++; continue; }          /* LS0 */
        if (c == 0x19 || c == 0x1D) {                         /* SS2 / SS3 */
            st.single = st.g[c == 0x19 ? 2 : 3];
            st.have_single = 1;
            i++;
            continue;
        }
        if (c == 0x0A || c == 0x0D || c == 0x09) {
            used = put_utf8(' ', dst, cap, used);
            i++;
            continue;
        }
        if (c == 0x20 || c == 0x7F) {
            used = put_utf8(0x3000, dst, cap, used);          /* ideographic space */
            i++;
            continue;
        }
        if (c >= 0x21 && c <= 0x7E) {
            set = st.have_single ? st.single : st.g[st.gl];
            st.have_single = 0;
            gl = c;
            i++;
        } else if (c >= 0xA1 && c <= 0xFE) {
            set = st.g[st.gr];
            gl = (uint8_t)(c - 0x80);
            i++;
        } else {
            i++;
            continue;
        }

        switch (set) {
        case ARIB_SET_KANJI: {
            uint8_t b1 = gl, b2;
            int ku, ten;
            if (i >= len)
                break;
            b2 = src[i];
            if (b2 >= 0xA1 && b2 <= 0xFE)
                b2 = (uint8_t)(b2 - 0x80);
            i++;
            ku = (int)b1 - 0x20;
            ten = (int)b2 - 0x20;
            if (ku >= 1 && ku <= 84 && ten >= 1 && ten <= 94)
                used = put_utf8(k_arib_kanji[ku - 1][ten - 1], dst, cap, used);
            break;
        }
        case ARIB_SET_HIRAGANA: {
            int idx = (int)gl - 0x21;
            if (idx >= 0 && idx < 94)
                used = put_utf8(k_arib_hiragana[idx], dst, cap, used);
            break;
        }
        case ARIB_SET_KATAKANA: {
            int idx = (int)gl - 0x21;
            if (idx >= 0 && idx < 94)
                used = put_utf8(k_arib_katakana[idx], dst, cap, used);
            break;
        }
        case ARIB_SET_ALNUM:
            if (gl >= 0x21 && gl <= 0x7E)
                used = put_utf8(alnum_char(gl), dst, cap, used);
            break;
        case ARIB_SET_SKIP:
        default:
            break;
        }
    }

    dst[used] = '\0';
    return used;
}
