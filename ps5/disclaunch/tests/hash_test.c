/* Host check of disc_hash.c: prints crc md5 sha1 of stdin read in uneven pieces (tests/run-hash-test.sh compares with Python). */
#include "../disc_hash.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    disc_hash_ctx c;
    disc_hash_init(&c);
    static unsigned char buf[1 << 20];
    size_t piece = argc > 1 ? strtoul(argv[1], NULL, 10) : 37;
    size_t n;
    while ((n = fread(buf, 1, piece, stdin)) > 0) disc_hash_update(&c, buf, n);
    char crc[9], md5[33], sha1[41];
    disc_hash_final(&c, crc, md5, sha1);
    printf("%s %s %s\n", crc, md5, sha1);
    return 0;
}
