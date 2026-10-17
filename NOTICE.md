# Provenance of the assembly

`chacha-x86_64.asm` and `poly1305-x86_64.asm` are generated from the CRYPTOGAMS
perlasm sources by Andy Polyakov:

* upstream: https://github.com/dot-asm/cryptogams
  (`x86_64/chacha-x86_64.pl`, `x86_64/poly1305-x86_64.pl`)
* generated for MASM: `perl <script>.pl masm > <name>.asm`
* licence: 3-clause BSD, see `LICENSE.cryptogams`

## Local modifications

Three `vzeroall` instructions are narrowed to `vzeroupper`:

| file | routine | line |
| --- | --- | --- |
| `poly1305-x86_64.asm` | `poly1305_blocks_vpmadd52_4x` | 3310 |
| `chacha-x86_64.asm` | `ChaCha20_avx512` | 2582 |
| `chacha-x86_64.asm` | `ChaCha20_avx512vl` | 2750 |

`vzeroall` zeroes ymm0-15, and on Win64 the low 128 bits of XMM6-15 belong to the caller.
Each of these three routines saves and restores only the subset it actually uses - all of
them for the poly1305 path, and just xmm6 and xmm7 for the two ChaCha ones - so `vzeroall`
destroys caller registers the routine never touched. That is correct on System V, where
every XMM register is volatile, and wrong on Windows.

The other `vzeroall` sites in these files are fine: `ChaCha20_avx2`, `ChaCha20_16x`,
`ChaCha20_8xvl` and `poly1305_blocks_avx512` all save and restore the full xmm6-15 range
around theirs. None of the routines uses ymm6-15 above the low 128 bits, so narrowing the
instruction is sufficient.

`ChaCha20_avx512` handles every length up to 512 bytes, which includes the 32-byte
Poly1305 key derivation that each AEAD operation performs. On an AVX-512 machine this fires
on every single call.

This was not theoretical here. The first CI run of this benchmark, on an AVX-512 Xeon,
reported `0.00 Gbit/s` and `-nan` for every result while the correctness cross-check still
passed: the ciphertext was right, and the caller's timing values - doubles, which Win64
keeps in XMM6-15 - had been zeroed underneath it.

This is a known upstream problem, reported against OpenSSL in 2020 and fixed there by
excluding VPMADD52 on Win64 entirely:

* https://github.com/openssl/openssl/issues/12744
* https://github.com/openssl/openssl/issues/21522
* https://github.com/openssl/openssl/pull/21808 (the fix)
* https://github.com/dot-asm/cryptogams/issues/20 (still open upstream)
