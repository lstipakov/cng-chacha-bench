# Provenance of the assembly

`chacha-x86_64.asm` and `poly1305-x86_64.asm` are generated from the CRYPTOGAMS
perlasm sources by Andy Polyakov:

* upstream: https://github.com/dot-asm/cryptogams
  (`x86_64/chacha-x86_64.pl`, `x86_64/poly1305-x86_64.pl`)
* generated for MASM: `perl <script>.pl masm > <name>.asm`
* licence: 3-clause BSD, see `LICENSE.cryptogams`

## One local modification

`poly1305-x86_64.asm`, the `poly1305_blocks_vpmadd52_4x` epilogue: `vzeroall` is
replaced by `vzeroupper`.

`vzeroall` zeroes ymm0-15, and on Win64 the low 128 bits of XMM6-15 belong to the
caller. The routine saves none of them, so on a VPMADD52-capable CPU it silently
destroys the caller's live values. The sibling `poly1305_blocks_avx512` does the
same `vzeroall` but saves and restores xmm6-15 around it, which is why only the
4x path is affected. The routine uses none of ymm6-15, so narrowing the
instruction is sufficient.

This is a known upstream problem, reported against OpenSSL in 2020 and fixed
there by excluding VPMADD52 on Win64 entirely:

* https://github.com/openssl/openssl/issues/12744
* https://github.com/openssl/openssl/issues/21522
* https://github.com/openssl/openssl/pull/21808 (the fix)
* https://github.com/dot-asm/cryptogams/issues/20 (still open upstream)

It is not relevant to the measurement here — the CPU in the sample output has no
VPMADD52 — but the fix is applied so the benchmark cannot corrupt its own caller
on a machine that does.
