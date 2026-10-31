/* Join by link (join.h): the join code out of a link or a code, and the link for a code. */
#include "join.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; }
static int alphabet(char c) { return c != 0 && strchr(JOIN_ALPHABET, c) != NULL; }

int join_code_valid(const char *code)
{
    int i;
    if (!code) return 0;
    for (i = 0; i < JOIN_CODE_LENGTH; i++)
        if (!alphabet(upper(code[i]))) return 0;
    return code[JOIN_CODE_LENGTH] == 0;
}

/* The six alphabet characters p begins with, ended: a seventh alphabet character means a longer
   word, not a code ("K7QX2MZ" is no code, "K7QX2M/" is). */
static int code_at(const char *p, char *out)
{
    int i;
    if (!p) return 0;
    for (i = 0; i < JOIN_CODE_LENGTH; i++)
        if (!alphabet(p[i])) return 0;
    if (alphabet(p[JOIN_CODE_LENGTH])) return 0;
    for (i = 0; i < JOIN_CODE_LENGTH; i++) out[i] = p[i];
    out[JOIN_CODE_LENGTH] = 0;
    return 1;
}

int join_code_from_text(char *out, size_t size, const char *text)
{
    char up[512], code[JOIN_CODE_LENGTH + 1];
    size_t n = 0, i, at = 0;
    const char *p, *q;
    if (!text || size < (size_t)JOIN_CODE_LENGTH + 1) return 0;
    for (i = 0; text[i] && n < sizeof up - 1; i++)
        up[n++] = upper(text[i]);
    up[n] = 0;
    while (at < n && (up[at] == ' ' || up[at] == '\t' || up[at] == '\r' || up[at] == '\n')) at++;
    while (n > at && (up[n - 1] == ' ' || up[n - 1] == '\t' || up[n - 1] == '\r' || up[n - 1] == '\n'))
        up[--n] = 0;
    if (at >= n) return 0;
    p = up + at;
    if (join_code_valid(p)) {
        memcpy(out, p, JOIN_CODE_LENGTH + 1);
        return 1;
    }
    /* a link's /join/<CODE> (its case is gone by now), also the dhd:join/CODE form */
    for (i = 0; i < 2; i++) {
        const char *needle = i ? "JOIN:" : "JOIN/";
        if ((q = strstr(p, needle)) != NULL && q[5] && code_at(q + 5, code)) {
            memcpy(out, code, JOIN_CODE_LENGTH + 1);
            return 1;
        }
    }
    /* the last path segment or the part after a scheme's colon (a generic link) */
    if ((q = strrchr(p, '/')) != NULL && code_at(q + 1, code)) {
        memcpy(out, code, JOIN_CODE_LENGTH + 1);
        return 1;
    }
    if ((q = strrchr(p, ':')) != NULL && code_at(q + 1, code)) {
        memcpy(out, code, JOIN_CODE_LENGTH + 1);
        return 1;
    }
    /* a lone six-character token somewhere in the text */
    for (q = p; *q;) {
        size_t len;
        if (!alphabet(*q)) {
            q++;
            continue;
        }
        for (len = 0; alphabet(q[len]); len++)
            ;
        if (len == JOIN_CODE_LENGTH) {
            memcpy(code, q, JOIN_CODE_LENGTH);
            code[JOIN_CODE_LENGTH] = 0;
            memcpy(out, code, JOIN_CODE_LENGTH + 1);
            return 1;
        }
        q += len;
    }
    return 0;
}

int join_link(char *out, size_t size, const char *base_url, const char *code)
{
    char norm[JOIN_CODE_LENGTH + 1], lower[9];
    const char *scheme = "https://", *host;
    size_t n, i;
    int written;
    if (!base_url || !*base_url || !out || size < 16) return 0;
    if (!join_code_valid(code)) return 0;
    for (i = 0; i < JOIN_CODE_LENGTH; i++) norm[i] = upper(code[i]);
    norm[JOIN_CODE_LENGTH] = 0;
    for (i = 0; i < 8 && base_url[i]; i++) lower[i] = (char)tolower((unsigned char)base_url[i]);
    lower[i] = 0;
    host = base_url;
    if (strncmp(lower, "wss://", 6) == 0) host = base_url + 6;
    else if (strncmp(lower, "https://", 8) == 0) host = base_url + 8;
    else if (strncmp(lower, "ws://", 5) == 0) scheme = "http://", host = base_url + 5;
    else if (strncmp(lower, "http://", 7) == 0) scheme = "http://", host = base_url + 7;
    n = strlen(host);
    while (n && host[n - 1] == '/') n--;
    if (!n) return 0;
    written = snprintf(out, size, "%s%.*s/join/%s", scheme, (int)n, host, norm);
    return written > 0 && (size_t)written < size;
}
