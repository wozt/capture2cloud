/* La poignee de main et le cadrage, verifies contre les valeurs de la RFC. */
/* The framing, verified against the RFC's own test vector. */
#include "../../ws_frame.c"
#include <stdio.h>
#include <string.h>
static int fails = 0;
static void ok(const char *name, int cond) {
    printf("  %s %s\n", cond ? "ok  " : "FAIL", name);
    if (!cond) fails++;
}
int main(void) {
    /* RFC 6455 section 1.3: cette cle donne exactement cet accept. */
    char out[256];
    size_t n = ws_accept_response("dGhlIHNhbXBsZSBub25jZQ==", out, sizeof(out));
    ok("la reponse est produite", n > 0);
    ok("l'accept est celui de la RFC",
       strstr(out, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
    ok("c'est bien un 101", strncmp(out, "HTTP/1.1 101", 12) == 0);
    ok("une cle vide est refusee", ws_accept_response("", out, sizeof(out)) == 0);

    uint8_t h[10];
    ok("en-tete court", ws_binary_header(5, h) == 2 && h[0] == 0x82 && h[1] == 5);
    ok("en-tete 16 bits", ws_binary_header(200, h) == 4 && h[1] == 126 && h[2] == 0 && h[3] == 200);
    ok("en-tete 64 bits", ws_binary_header(70000, h) == 10 && h[1] == 127);

    /* Une trame masquee du client: "Hi" avec le masque 0x01020304. */
    uint8_t frame[] = {0x82, 0x82, 1, 2, 3, 4, 'H' ^ 1, 'i' ^ 2};
    uint8_t op = 0; uint8_t *pl = NULL; size_t pn = 0;
    long used = ws_take_frame(frame, sizeof(frame), &op, &pl, &pn);
    ok("la trame est consommee entiere", used == (long)sizeof(frame));
    ok("l'opcode est binaire", op == WS_OP_BINARY);
    ok("le demasquage est correct", pn == 2 && pl[0] == 'H' && pl[1] == 'i');

    uint8_t partial[] = {0x82, 0x82, 1, 2};
    ok("une trame incomplete attend", ws_take_frame(partial, sizeof(partial), &op, &pl, &pn) == 0);

    uint8_t unmasked[] = {0x82, 0x02, 'H', 'i'};
    ok("une trame non masquee est refusee",
       ws_take_frame(unmasked, sizeof(unmasked), &op, &pl, &pn) == -1);

    /* Une longueur enorme est rejetee avant qu'on ne la croie. */
    uint8_t huge[] = {0x82, 0xff, 0,0,0,1, 0,0,0,0, 1,2,3,4};
    ok("une longueur absurde est refusee",
       ws_take_frame(huge, sizeof(huge), &op, &pl, &pn) == -1);

    printf("\n%s\n", fails ? "des tests ont echoue" : "tout passe");
    return fails ? 1 : 0;
}
