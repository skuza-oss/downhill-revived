/* Tests of join.c: the join code out of a link or a code, and the shareable link for a code
   (docs/online-protocol.md, join codes; finding 125). */
#include "join.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* 1 when text names code (join_code_from_text puts it in code). */
static int names(const char *text, const char *code)
{
    char out[JOIN_CODE_LENGTH + 1];
    return join_code_from_text(out, sizeof out, text) && !strcmp(out, code);
}

static int rejects(const char *text)
{
    char out[JOIN_CODE_LENGTH + 1];
    return !join_code_from_text(out, sizeof out, text);
}

int main(void)
{
    char link[200], code[JOIN_CODE_LENGTH + 1];

    /* the code itself, any case, with padding */
    CHECK(names("K7QX2M", "K7QX2M"));
    CHECK(names("k7qx2m", "K7QX2M"));
    CHECK(names("  K7QX2M\n", "K7QX2M"));
    CHECK(names("K7QX2M?s=1", "K7QX2M"));
    CHECK(names("Join code K7QX2M", "K7QX2M"));
    CHECK(names("the code is abcd2m", "ABCD2M"));

    /* the links the game and the launcher build and accept */
    CHECK(names("https://dhd.skuza.app/join/K7QX2M", "K7QX2M"));
    CHECK(names("https://dhd.skuza.app/join/k7qx2m", "K7QX2M"));
    CHECK(names("https://dhd.skuza.app/join/k7qx2m?x=1#top", "K7QX2M"));
    CHECK(names("https://dhd.skuza.app/K7QX2M", "K7QX2M"));
    CHECK(names("http://127.0.0.1:8787/join/K7QX2M", "K7QX2M"));
    CHECK(names("http://127.0.0.1:8787/K7QX2M/", "K7QX2M"));
    CHECK(names("dhd://join/K7QX2M", "K7QX2M"));
    CHECK(names("dhd:join/K7QX2M", "K7QX2M"));
    CHECK(names("dhd://K7QX2M", "K7QX2M"));
    CHECK(names("dhd:K7QX2M", "K7QX2M"));
    CHECK(names("https://downhill-revived.pages.dev/join/K7QX2M", "K7QX2M"));

    /* nothing, or not a code */
    CHECK(rejects(""));
    CHECK(rejects(NULL));
    CHECK(rejects("https://dhd.skuza.app/join/"));
    CHECK(rejects("https://dhd.skuza.app/"));
    CHECK(rejects("K7QX2"));
    CHECK(rejects("K7QX2MZ"));        /* a seventh letter: a word, not a code */
    CHECK(rejects("0O1IL2"));         /* the alphabet leaves out 0 O 1 I L */
    CHECK(rejects("hello world"));
    CHECK(rejects("https://dhd.skuza.app/join/HELLO"));  /* five letters */

    /* the alphabet and the length */
    CHECK(join_code_valid("K7QX2M"));
    CHECK(join_code_valid("k7qx2m"));
    CHECK(!join_code_valid("K7QX2"));
    CHECK(!join_code_valid("K7QX2MZ"));
    CHECK(!join_code_valid("K7QX2O"));  /* O is not in the alphabet */
    CHECK(!join_code_valid("K7QX2M "));
    CHECK(!join_code_valid(""));
    CHECK(!join_code_valid(NULL));

    /* the link the room shares, from the relay's base URL */
    CHECK(join_link(link, sizeof link, "wss://dhd.skuza.app", "K7QX2M") && !strcmp(link, "https://dhd.skuza.app/join/K7QX2M"));
    CHECK(join_link(link, sizeof link, "WSS://dhd.skuza.app", "k7qx2m") && !strcmp(link, "https://dhd.skuza.app/join/K7QX2M"));
    CHECK(join_link(link, sizeof link, "ws://127.0.0.1:8787", "K7QX2M") && !strcmp(link, "http://127.0.0.1:8787/join/K7QX2M"));
    CHECK(join_link(link, sizeof link, "wss://host/", "K7QX2M") && !strcmp(link, "https://host/join/K7QX2M"));
    CHECK(join_link(link, sizeof link, "https://host", "K7QX2M") && !strcmp(link, "https://host/join/K7QX2M"));
    CHECK(!join_link(link, sizeof link, "", "K7QX2M"));
    CHECK(!join_link(link, sizeof link, "wss://", "K7QX2M"));
    CHECK(!join_link(link, sizeof link, "wss://host", "bad"));
    CHECK(!join_link(link, sizeof link, "wss://host", "K7QX2"));  /* too short */
    CHECK(!join_link(link, 8, "wss://host", "K7QX2M"));           /* the buffer is too small */

    /* a link built here is a link this parses (round trip) */
    CHECK(join_link(link, sizeof link, "wss://dhd.skuza.app", "K7QX2M"));
    CHECK(join_code_from_text(code, sizeof code, link) && !strcmp(code, "K7QX2M"));

    if (failures) {
        printf("test_join: %d failures\n", failures);
        return 1;
    }
    printf("test_join: ok\n");
    return 0;
}
