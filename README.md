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

Three CPUs, single thread pinned to one core, keys and algorithm handles created once and
reused, buffer resident. Gbit/s.

| CPU | payload | CNG AES-256-GCM | CNG ChaCha20-Poly1305 | CRYPTOGAMS ChaCha20-Poly1305 |
| --- | --- | --- | --- | --- |
| Core i5-1335U (AVX2) | 1 400 B | 43.3 | **3.9** | 15.0 |
| | 8 192 B | 76.8 | **4.0** | 20.4 |
| | 65 536 B | 82.8 | **4.1** | 20.5 |
| EPYC 7763 (AVX2) | 1 400 B | 26.8 | **2.7** | 12.3 |
| | 8 192 B | 42.0 | **2.9** | 16.9 |
| | 65 536 B | 45.3 | **2.9** | 17.6 |
| Xeon Platinum 8573C (AVX-512) | 1 400 B | 39.9 | **3.4** | 15.8 |
| | 8 192 B | 58.9 | **3.6** | 26.6 |
| | 65 536 B | 64.1 | **3.6** | 28.7 |

On every one of them CNG's ChaCha20-Poly1305 is **4.6x to 8.0x slower than the same AEAD in
portable assembly**, and **10x to 20x slower than AES-GCM on the same provider**.

The two cloud CPUs are GitHub-hosted Actions runners, so the numbers can be reproduced
without our hardware: **Actions -> benchmark -> Run workflow**. The runner pool mixes Intel
and AMD, and the output names the CPU it ran on.

## Why this looks like a missing optimisation rather than call overhead

**CNG's ChaCha throughput does not change with payload size.** On the i5 it is 3.9 Gbit/s at
1 400 bytes and 4.1 Gbit/s at 65 536 bytes — a 47x larger buffer for the same rate — and the
other two CPUs behave identically. Per-call overhead would amortise away across that range,
and for AES-GCM on the same provider it plainly does, rising from 43 to 83 Gbit/s on the
same machine. ChaCha stays flat, which is the signature of a per-byte cost, not a fixed one.

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
