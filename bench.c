/*
 * CNG ChaCha20-Poly1305 throughput, against AES-256-GCM on the same provider and
 * against the CRYPTOGAMS assembly doing the same AEAD.
 *
 * Single thread, pinned to one core, everything in L2. Keys and algorithm handles are
 * created once and reused, so the loop measures the cipher and nothing else.
 *
 * Copyright (c) 2026 Lev Stipakov. 2-clause BSD; see LICENSE.
 */

#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "bcrypt.lib")

#define TAG_LEN   16
#define NONCE_LEN 12
#define AAD_LEN   16
#define MIN_SECONDS 0.5

/* ---------------------------------------------------------------- CRYPTOGAMS */

unsigned int OPENSSL_ia32cap_P[4] = { 0 };

void ChaCha20_ctr32(unsigned char *out, const unsigned char *inp, size_t len,
                    const unsigned int key[8], const unsigned int counter[4]);
int  poly1305_init(void *ctx, const unsigned char key[16], void *func[2]);
void poly1305_blocks(void *ctx, const unsigned char *inp, size_t len, unsigned int padbit);
void poly1305_emit(void *ctx, unsigned char mac[16], const unsigned int nonce[4]);

typedef void (*poly_blocks_fn)(void *, const unsigned char *, size_t, unsigned int);
typedef void (*poly_emit_fn)(void *, unsigned char *, const unsigned int *);

/* The assembly dispatches on this; it is CPUID the way OpenSSL lays it out. */
static void cap_init(void)
{
    int regs[4];
    unsigned __int64 xcr0 = 0;

    __cpuid(regs, 0);
    if (regs[0] < 1)
        return;

    __cpuid(regs, 1);
    OPENSSL_ia32cap_P[0] = (unsigned int)regs[3];
    OPENSSL_ia32cap_P[1] = (unsigned int)regs[2];

    if (OPENSSL_ia32cap_P[1] & (1u << 27))   /* OSXSAVE */
        xcr0 = _xgetbv(0);

    __cpuidex(regs, 7, 0);
    OPENSSL_ia32cap_P[2] = (unsigned int)regs[1];
    OPENSSL_ia32cap_P[3] = (unsigned int)regs[2];

    /* Without OS support for the wider state, hide the features that would use it. */
    if ((xcr0 & 0x6) != 0x6) {
        OPENSSL_ia32cap_P[1] &= ~(1u << 28);                        /* AVX  */
        OPENSSL_ia32cap_P[2] &= ~(1u << 5);                         /* AVX2 */
    }
    if ((xcr0 & 0xe6) != 0xe6) {
        OPENSSL_ia32cap_P[2] &= ~((1u << 16) | (1u << 17) | (1u << 21) |
                                  (1u << 28) | (1u << 30) | (1u << 31));
    }
}

static void cg_poly(const unsigned char *poly_key,
                    const unsigned char *aad, size_t aad_len,
                    const unsigned char *ct, size_t ct_len,
                    unsigned char *tag)
{
    __declspec(align(64)) unsigned char ctx[320];
    unsigned char block[16];
    void *fn[2] = { NULL, NULL };
    poly_blocks_fn blocks = poly1305_blocks;
    poly_emit_fn emit = poly1305_emit;
    size_t whole;

    memset(ctx, 0, sizeof(ctx));
    if (poly1305_init(ctx, poly_key, fn) && fn[0] && fn[1]) {
        blocks = (poly_blocks_fn)fn[0];
        emit = (poly_emit_fn)fn[1];
    }

    if (aad_len) {
        whole = aad_len & ~(size_t)15;
        if (whole)
            blocks(ctx, aad, whole, 1);
        if (aad_len != whole) {
            memset(block, 0, sizeof(block));
            memcpy(block, aad + whole, aad_len - whole);
            blocks(ctx, block, sizeof(block), 1);
        }
    }

    if (ct_len) {
        whole = ct_len & ~(size_t)15;
        if (whole)
            blocks(ctx, ct, whole, 1);
        if (ct_len != whole) {
            memset(block, 0, sizeof(block));
            memcpy(block, ct + whole, ct_len - whole);
            blocks(ctx, block, sizeof(block), 1);
        }
    }

    memset(block, 0, sizeof(block));
    *(unsigned __int64 *)(block) = (unsigned __int64)aad_len;
    *(unsigned __int64 *)(block + 8) = (unsigned __int64)ct_len;
    blocks(ctx, block, sizeof(block), 1);

    emit(ctx, tag, (const unsigned int *)(poly_key + 16));
}

/* RFC 8439 seal, in place. */
static void cg_seal(const unsigned char key[32], const unsigned char nonce[NONCE_LEN],
                    const unsigned char *aad, size_t aad_len,
                    unsigned char *data, size_t len, unsigned char *tag)
{
    unsigned int counter[4];
    unsigned char poly_key[32] = { 0 };

    counter[0] = 0;
    memcpy(counter + 1, nonce, NONCE_LEN);
    ChaCha20_ctr32(poly_key, poly_key, sizeof(poly_key), (const unsigned int *)key, counter);

    counter[0] = 1;
    ChaCha20_ctr32(data, data, len, (const unsigned int *)key, counter);

    cg_poly(poly_key, aad, aad_len, data, len, tag);
}

/* ---------------------------------------------------------------------- CNG */

typedef struct {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_KEY_HANDLE key;
} cng_t;

static int cng_open(cng_t *c, LPCWSTR algid, LPCWSTR chain,
                    const unsigned char *key, ULONG key_len)
{
    memset(c, 0, sizeof(*c));

    if (BCryptOpenAlgorithmProvider(&c->alg, algid, NULL, 0) != 0)
        return 0;

    if (chain && BCryptSetProperty(c->alg, BCRYPT_CHAINING_MODE,
                                   (PUCHAR)chain,
                                   (ULONG)((wcslen(chain) + 1) * sizeof(WCHAR)), 0) != 0)
        return 0;

    if (BCryptGenerateSymmetricKey(c->alg, &c->key, NULL, 0,
                                   (PUCHAR)key, key_len, 0) != 0)
        return 0;

    return 1;
}

static int cng_seal(cng_t *c, const unsigned char *nonce,
                    const unsigned char *aad, ULONG aad_len,
                    unsigned char *data, ULONG len, unsigned char *tag)
{
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    ULONG done = 0;

    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = (PUCHAR)nonce;
    info.cbNonce = NONCE_LEN;
    info.pbAuthData = (PUCHAR)aad;
    info.cbAuthData = aad_len;
    info.pbTag = tag;
    info.cbTag = TAG_LEN;

    return BCryptEncrypt(c->key, data, len, &info, NULL, 0,
                         data, len, &done, 0) == 0;
}

/* -------------------------------------------------------------------- timing */

static double g_freq;

static double now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / g_freq;
}

typedef enum { RUN_CNG, RUN_CG } run_kind;

typedef struct {
    double gbits;
    double ns_per_op;
    unsigned __int64 ops;
} result_t;

static result_t measure(run_kind kind, cng_t *c,
                        const unsigned char *key, const unsigned char *nonce,
                        const unsigned char *aad, size_t aad_len,
                        unsigned char *buf, size_t len)
{
    unsigned char tag[TAG_LEN];
    unsigned __int64 ops = 0;
    double start, elapsed;
    result_t r;
    int i;

    for (i = 0; i < 64; i++) {       /* warm up: page the buffer in, settle the clocks */
        if (kind == RUN_CNG)
            cng_seal(c, nonce, aad, (ULONG)aad_len, buf, (ULONG)len, tag);
        else
            cg_seal(key, nonce, aad, aad_len, buf, len, tag);
    }

    start = now();
    do {
        for (i = 0; i < 16; i++) {
            if (kind == RUN_CNG)
                cng_seal(c, nonce, aad, (ULONG)aad_len, buf, (ULONG)len, tag);
            else
                cg_seal(key, nonce, aad, aad_len, buf, len, tag);
        }
        ops += 16;
        elapsed = now() - start;
    } while (elapsed < MIN_SECONDS);

    r.ops = ops;
    r.ns_per_op = elapsed * 1e9 / (double)ops;
    r.gbits = ((double)ops * (double)len * 8.0) / elapsed / 1e9;
    return r;
}

/* ---------------------------------------------------------------------- main */

static void print_cpu(void)
{
    int regs[4];
    char brand[49] = { 0 };
    unsigned i;

    __cpuid(regs, (int)0x80000000u);
    if ((unsigned)regs[0] >= 0x80000004u) {
        for (i = 0; i < 3; i++) {
            __cpuid(regs, (int)(0x80000002u + i));
            memcpy(brand + i * 16, regs, 16);
        }
        printf("CPU       : %s\n", brand);
    }

    printf("AVX2      : %s\n", (OPENSSL_ia32cap_P[2] & (1u << 5)) ? "yes" : "no");
    printf("AVX-512F  : %s\n", (OPENSSL_ia32cap_P[2] & (1u << 16)) ? "yes" : "no");
    printf("VPMADD52  : %s\n", (OPENSSL_ia32cap_P[3] & (1u << 21)) ? "yes" : "no");
    printf("AES-NI    : %s\n", (OPENSSL_ia32cap_P[1] & (1u << 25)) ? "yes" : "no");
}

/* Same key, nonce and AAD through both paths must give the same ciphertext and tag. */
static int cross_check(cng_t *chacha, const unsigned char *key,
                       const unsigned char *nonce, const unsigned char *aad)
{
    unsigned char a[333], b[333], ta[TAG_LEN], tb[TAG_LEN];
    unsigned i;

    for (i = 0; i < sizeof(a); i++)
        a[i] = b[i] = (unsigned char)(i * 7 + 1);

    if (!cng_seal(chacha, nonce, aad, AAD_LEN, a, sizeof(a), ta)) {
        printf("cross-check: CNG seal failed\n");
        return 0;
    }
    cg_seal(key, nonce, aad, AAD_LEN, b, sizeof(b), tb);

    if (memcmp(a, b, sizeof(a)) != 0 || memcmp(ta, tb, TAG_LEN) != 0) {
        printf("cross-check: MISMATCH - the two paths do not agree\n");
        return 0;
    }
    return 1;
}

int main(void)
{
    static const size_t sizes[] = { 1400, 8192, 65536 };
    unsigned char key[32], nonce[NONCE_LEN], aad[AAD_LEN];
    unsigned char *buf;
    cng_t aes, chacha;
    LARGE_INTEGER f;
    unsigned i;

    QueryPerformanceFrequency(&f);
    g_freq = (double)f.QuadPart;

    cap_init();

    for (i = 0; i < sizeof(key); i++)   key[i]   = (unsigned char)i;
    for (i = 0; i < sizeof(nonce); i++) nonce[i] = (unsigned char)(0x40 + i);
    for (i = 0; i < sizeof(aad); i++)   aad[i]   = (unsigned char)(0x80 + i);

    SetThreadAffinityMask(GetCurrentThread(), 1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    if (!cng_open(&aes, BCRYPT_AES_ALGORITHM, BCRYPT_CHAIN_MODE_GCM, key, 32)) {
        printf("could not open AES-256-GCM\n");
        return 1;
    }
    if (!cng_open(&chacha, BCRYPT_CHACHA20_POLY1305_ALGORITHM, NULL, key, 32)) {
        printf("could not open CHACHA20_POLY1305 - needs Windows 10 1903 or later\n");
        return 1;
    }

    print_cpu();
    printf("\n");

    if (!cross_check(&chacha, key, nonce, aad))
        return 1;
    printf("cross-check: CNG and CRYPTOGAMS agree, byte for byte\n\n");

    buf = (unsigned char *)VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_READWRITE);
    if (!buf)
        return 1;

    printf("%-28s %10s %12s %12s\n", "", "bytes", "Gbit/s", "ns/op");
    printf("%-28s %10s %12s %12s\n", "----------------------------",
           "----------", "------------", "------------");

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t len = sizes[i];
        result_t a, c, g;

        memset(buf, 0x5a, len);
        a = measure(RUN_CNG, &aes, key, nonce, aad, AAD_LEN, buf, len);
        memset(buf, 0x5a, len);
        c = measure(RUN_CNG, &chacha, key, nonce, aad, AAD_LEN, buf, len);
        memset(buf, 0x5a, len);
        g = measure(RUN_CG, NULL, key, nonce, aad, AAD_LEN, buf, len);

        printf("%-28s %10zu %12.2f %12.0f\n", "CNG AES-256-GCM", len, a.gbits, a.ns_per_op);
        printf("%-28s %10zu %12.2f %12.0f\n", "CNG ChaCha20-Poly1305", len, c.gbits, c.ns_per_op);
        printf("%-28s %10zu %12.2f %12.0f\n", "CRYPTOGAMS ChaCha20-Poly1305", len, g.gbits, g.ns_per_op);
        printf("%-28s %10s %11.1fx %12s\n", "  CRYPTOGAMS vs CNG ChaCha", "",
               c.gbits > 0.0 ? g.gbits / c.gbits : 0.0, "");
        printf("%-28s %10s %11.1fx %12s\n", "  CNG AES vs CNG ChaCha", "",
               c.gbits > 0.0 ? a.gbits / c.gbits : 0.0, "");
        printf("\n");
    }

    VirtualFree(buf, 0, MEM_RELEASE);
    return 0;
}
