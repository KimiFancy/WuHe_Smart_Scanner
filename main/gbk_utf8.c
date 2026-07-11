/**
 * gbk_utf8.c — GBK → UTF-8 transcoder.
 *
 * See gbk_utf8.h for the design rationale. The lookup table lives in
 * gbk_table.c (generated). This file holds only the conversion logic.
 */

#include "gbk_utf8.h"

#include <stdint.h>

/* GBK trail-byte → column index. Returns -1 for the unused 0x7F gap and
 * anything outside the valid 0x40-0xFE range. */
static int trail_col(uint8_t trail)
{
    if (trail >= 0x40 && trail <= 0x7E) {
        return trail - 0x40;
    }
    if (trail >= 0x80 && trail <= 0xFE) {
        return trail - 0x80 + 63;
    }
    return -1;
}

/* UTF-8 byte length for a BMP codepoint (1, 2, or 3). */
static size_t utf8_len(uint32_t cp)
{
    if (cp < 0x80) {
        return 1;
    }
    if (cp < 0x800) {
        return 2;
    }
    return 3;
}

/* Write a BMP codepoint as UTF-8 into out[0..). Returns the byte count.
 * Caller must guarantee out has room (check utf8_len first). */
static size_t utf8_encode(uint32_t cp, uint8_t *out)
{
    if (cp < 0x80) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (uint8_t)(0xC0 | (cp >> 6));
        out[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (uint8_t)(0xE0 | (cp >> 12));
    out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (uint8_t)(0x80 | (cp & 0x3F));
    return 3;
}

size_t gbk_to_utf8(const uint8_t *gbk, size_t gbk_len,
                   char *utf8, size_t utf8_cap)
{
    if (gbk == NULL || utf8 == NULL || utf8_cap == 0) {
        return 0;
    }

    size_t oi = 0;
    size_t i = 0;

    while (i < gbk_len) {
        uint8_t b = gbk[i];

        if (b < 0x80) {
            /* ASCII: identical in UTF-8. Leave room for NUL. */
            if (oi + 1 >= utf8_cap) {
                break;
            }
            utf8[oi++] = (char)b;
            i++;
            continue;
        }

        /* GBK lead byte: needs a trail byte. */
        if (i + 1 >= gbk_len) {
            /* Dangling lead byte at end of input. */
            break;
        }

        uint8_t lead = b;
        uint8_t trail = gbk[i + 1];
        int col = trail_col(trail);

        uint32_t cp;
        if (col < 0 || lead < 0x81 || lead > 0xFE) {
            cp = 0xFFFD;   /* invalid position */
        } else {
            uint16_t mapped = gbk_to_unicode[lead - 0x81][col];
            cp = (mapped == 0) ? 0xFFFD : mapped;
        }

        size_t need = utf8_len(cp);
        if (oi + need >= utf8_cap) {
            /* Would overflow (no room for this char + NUL). Stop here so the
             * output ends on a complete character boundary. */
            break;
        }
        oi += utf8_encode(cp, (uint8_t *)utf8 + oi);
        i += 2;
    }

    utf8[oi] = '\0';
    return oi;
}
