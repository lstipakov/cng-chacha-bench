# CNG ChaCha20-Poly1305 is an order of magnitude slower than it should be

A small self-contained benchmark for Windows CNG (`bcrypt.dll`). It measures
`BCRYPT_CHACHA20_POLY1305_ALGORITHM` against two references on the same machine, in the
same process, over the same buffers:

* **AES-256-GCM through CNG**, which shows what the same provider achieves when the
  algorithm is well optimised;
* **ChaCha20-Poly1305 through the CRYPTOGAMS assembly**, which shows what the algorithm
  itself achieves on this CPU.

Both AEADs are RFC-compliant and produce identical output, which the program checks before
it measures anything.

## Result

13th Gen Intel Core i5-1335U, AVX2, no AVX-512. Single thread pinned to one core, keys and
algorithm handles created once and reused, buffer resident:

| payload | CNG AES-256-GCM | CNG ChaCha20-Poly1305 | CRYPTOGAMS ChaCha20-Poly1305 |
| --- | --- | --- | --- |
| 1 400 B | 40.80 Gbit/s | **3.89 Gbit/s** | 15.28 Gbit/s |
| 8 192 B | 73.66 Gbit/s | **4.16 Gbit/s** | 19.85 Gbit/s |
| 65 536 B | 79.78 Gbit/s | **4.03 Gbit/s** | 20.86 Gbit/s |

So on this CPU, CNG's ChaCha20-Poly1305 is **3.9x to 5.2x slower than the same algorithm in
portable assembly**, and **10x to 20x slower than AES-GCM on the same provider**.

## Why this looks like a missing optimisation rather than call overhead

**CNG's ChaCha throughput does not change with payload size.** It is 3.89 Gbit/s at 1 400
bytes and 4.03 Gbit/s at 65 536 bytes — a 47x larger buffer for the same rate. Per-call
overhead would amortise away across that range, and for AES-GCM on the same provider it
plainly does: 40.8 Gbit/s at 1 400 bytes rising to 79.8 Gbit/s at 64 KB. ChaCha stays flat,
which is the signature of a per-byte cost, not a fixed one.

The CRYPTOGAMS figures on the same CPU show the per-byte cost is not inherent to the
algorithm. That implementation uses AVX2 to run several ChaCha blocks in parallel and a
vectorised Poly1305. The measured gap is consistent with CNG running a scalar,
one-block-at-a-time implementation.

ChaCha20-Poly1305 exists largely for machines *without* AES-NI, where it should be the fast
choice. On this provider it is the slow one even on a machine that has AES-NI, so there is
no configuration in which choosing it pays off.

## Where we ran into this

[ovpn-dco-win](https://github.com/OpenVPN/ovpn-dco-win), OpenVPN's Windows kernel data-channel
driver, encrypts every packet in the kernel through CNG. Between two 16 vCPU cloud instances
a single tunnel carried 7.1-7.4 Gbit/s on AES-256-GCM but only 4.9-5.5 Gbit/s on
ChaCha20-Poly1305. Changing nothing but the cipher implementation took the same tunnel to
7.3 Gbit/s in the direction that had been furthest behind — the per-flow network ceiling on
those instances — and from 4.9 to 5.9 Gbit/s in the other.

Users who select ChaCha20-Poly1305 by policy, or who run on hardware without AES-NI, pay
this directly.

## Building and running

Needs Visual Studio with the x64 toolchain — `cl.exe` and `ml64.exe`. From an **x64 Native
Tools Command Prompt**:

```
.\build.cmd
.\bench.exe
```

No dependencies beyond `bcrypt.lib`. Nothing is installed and nothing is written to disk.

## What the program does

1. Reads CPUID and fills the capability vector the assembly dispatches on, then prints which
   vector extensions the CPU has.
2. Seals the same 333-byte buffer with the same key, nonce and AAD through CNG and through
   CRYPTOGAMS, and compares ciphertext and tag byte for byte. It stops if they differ, so a
   reported number is never from a path that computed the wrong answer.
3. For each payload size, warms up 64 iterations and then loops for at least half a second,
   reporting Gbit/s and ns per operation.

Timing is `QueryPerformanceCounter`. The thread is pinned to CPU 0 and run at high priority.
Keys and algorithm handles are created once, outside the loop — the measurement covers
`BCryptEncrypt` and the equivalent CRYPTOGAMS calls, nothing else.

## Licence

2-clause-style BSD, see [LICENSE](LICENSE). The two `.asm` files are CRYPTOGAMS code by Andy
Polyakov under 3-clause BSD; see [LICENSE.cryptogams](LICENSE.cryptogams) and
[NOTICE.md](NOTICE.md), which also records the one local modification and why it is there.
