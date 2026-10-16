// CPU benchmark for comparing translators: the same source built for macOS, Linux and Windows,
// arm64 and x86-64. Prints "<test> <seconds>" lines; "nop" exits at once (for startup timing).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart; }
static HANDLE nul;
static void os_call(void) { DWORD w; WriteFile(nul, "x", 1, &w, NULL); }
static void os_init(void) { nul = CreateFileA("NUL", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL); }
#else
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int nul;
static void os_call(void) { if (write(nul, "x", 1) != 1) abort(); }
static void os_init(void) { nul = open("/dev/null", O_WRONLY); }
#endif

static volatile uint64_t sink;

/* integer: SHA-256 over a buffer */
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, hh;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
static void t_sha256(uint8_t *buf, size_t n)
{
    uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    for (int r = 0; r < 4; r++) for (size_t i = 0; i + 64 <= n; i += 64) sha256_block(h, buf + i);
    sink += h[0];
}

/* floating point: n-body */
static void t_nbody(void)
{
    double x[5] = {0, 4.84, 8.34, 12.89, 15.38}, y[5] = {0, -1.16, 4.12, -15.11, -25.92}, z[5] = {0, -0.10, -0.40, -0.22, 0.18};
    double vx[5] = {0, 0.606, -1.01, 1.08, 0.979}, vy[5] = {0, 2.81, 1.82, 0.868, 0.594}, vz[5] = {0, -0.02, 0.008, -0.01, -0.034};
    double m[5] = {39.47, 0.037, 0.011, 0.0017, 0.002}, dt = 0.01;
    for (int s = 0; s < 30000000; s++) {
        for (int i = 0; i < 5; i++) for (int j = i + 1; j < 5; j++) {
            double dx = x[i] - x[j], dy = y[i] - y[j], dz = z[i] - z[j];
            double d2 = dx*dx + dy*dy + dz*dz, mag = dt / (d2 * sqrt(d2));
            vx[i] -= dx * m[j] * mag; vy[i] -= dy * m[j] * mag; vz[i] -= dz * m[j] * mag;
            vx[j] += dx * m[i] * mag; vy[j] += dy * m[i] * mag; vz[j] += dz * m[i] * mag;
        }
        for (int i = 0; i < 5; i++) { x[i] += dt * vx[i]; y[i] += dt * vy[i]; z[i] += dt * vz[i]; }
    }
    sink += (uint64_t)(x[1] * 1e6);
}

/* memory: large copies */
static void t_memcpy(uint8_t *a, uint8_t *b, size_t n)
{
    for (int r = 0; r < 400; r++) { memcpy(r & 1 ? a : b, r & 1 ? b : a, n); }
    sink += a[n / 2];
}

/* branchy code + indirect calls: qsort */
static int cmp(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
static void t_qsort(void)
{
    size_t n = 10000000;
    int *v = malloc(n * sizeof *v);
    uint32_t s = 12345;
    for (size_t i = 0; i < n; i++) { s = s * 1103515245 + 12345; v[i] = (int)(s >> 1); }
    qsort(v, n, sizeof *v, cmp);
    sink += v[n / 2];
    free(v);
}

/* the OS boundary: one-byte writes to the null device */
static void t_oscall(void) { for (int i = 0; i < 1000000; i++) os_call(); }

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "nop")) return 0;
    const char *only = argc > 1 ? argv[1] : NULL;
    size_t n = 64 << 20;
    uint8_t *a = malloc(n), *b = malloc(n);
    for (size_t i = 0; i < n; i++) a[i] = (uint8_t)(i * 31 + 7);
    memcpy(b, a, n);
    os_init();
    struct { const char *name; int id; } tests[] = { {"sha256", 0}, {"nbody", 1}, {"memcpy", 2}, {"qsort", 3}, {"oscall", 4} };
    for (int k = 0; k < 5; k++) {
        if (only && strcmp(only, tests[k].name)) continue;
        double t0 = now();
        switch (tests[k].id) {
        case 0: t_sha256(a, n); break;
        case 1: t_nbody(); break;
        case 2: t_memcpy(a, b, n); break;
        case 3: t_qsort(); break;
        case 4: t_oscall(); break;
        }
        printf("%s %.4f\n", tests[k].name, now() - t0);
        fflush(stdout);
    }
    return (int)(sink & 0);
}
