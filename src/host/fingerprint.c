#include "host.h"

#include <bcrypt.h>
#include <stdio.h>

int host_sha256_file(const char* path, char hex[65]) {
    static unsigned char block[1 << 16];
    unsigned char digest[32];
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    size_t n;
    int ok = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0 &&
        BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) >= 0) {
        ok = 1;
        while (ok && (n = fread(block, 1, sizeof block, f)) > 0)
            ok = BCryptHashData(hash, block, (ULONG)n, 0) >= 0;
        ok = ok && !ferror(f) && BCryptFinishHash(hash, digest, sizeof digest, 0) >= 0;
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    fclose(f);
    if (ok)
        for (int i = 0; i < 32; i++) _snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return ok;
}
