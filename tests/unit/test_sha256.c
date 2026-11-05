#include "integrityd.h"
#include "check.h"

#include <stdlib.h>

static void digest_chunked(const unsigned char *data, size_t n, size_t chunk, char hex[65])
{
    Sha256 c;
    unsigned char d[32];
    sha256_init(&c);
    for (size_t off = 0; off < n; off += chunk) sha256_update(&c, data + off, n - off < chunk ? n - off : chunk);
    sha256_final(&c, d);
    hex_encode(d, 32, hex);
}

int main(void)
{
    char hex[65];
    sha256_hex("", 0, hex);
    CHECK_STREQ(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    sha256_hex("abc", 3, hex);
    CHECK_STREQ(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_hex(m448, strlen(m448), hex);
    CHECK_STREQ(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    unsigned char *a = malloc(1000000);
    memset(a, 'a', 1000000);
    sha256_hex(a, 1000000, hex);
    CHECK_STREQ(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    digest_chunked(a, 1000000, 1, hex);
    CHECK_STREQ(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    digest_chunked(a, 1000000, 65537, hex);
    CHECK_STREQ(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    static const size_t lens[] = { 0, 1, 55, 56, 63, 64, 65, 119, 120, 127, 128, 129, 1000 };
    for (size_t i = 0; i < sizeof lens / sizeof *lens; i++) {
        for (size_t j = 0; j < lens[i]; j++) a[j] = (unsigned char)(j * 31 + 7);
        char one[65], parts[65];
        sha256_hex(a, lens[i], one);
        for (size_t chunk = 1; chunk <= 70; chunk += 3) {
            digest_chunked(a, lens[i], chunk, parts);
            CHECK_STREQ(one, parts);
        }
    }
    free(a);
    return CHECK_DONE();
}
