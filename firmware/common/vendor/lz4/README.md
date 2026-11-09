# Vendored LZ4 1.11.0

Source: <https://github.com/lz4/lz4/tree/v1.11.0/lib> (tag commit
`c72c7c0ec132ad2ce2fe9fad7b0c728d9faee2fc`), retrieved 2026-11-09.
The implementation and header are byte for byte unmodified. The
BSD-2-Clause terms are retained in [LICENSE](LICENSE) and the source headers.
These third-party files are not relicensed under the repository's CC BY license.

| File | SHA-256 of vendored bytes |
| --- | --- |
| lz4.c | 45061782ff932eb8cf71c3aea9495cec0d392f012395272e0550e30c8593c740 |
| lz4.h | 6fee8f913ce3bc65bf716060f65ce890d83bea905445256c7a56da5bf4824d73 |
| LICENSE | 8b58c446121a109ccf32edc094bba3010a3d85e4ee3702950db55e4d3e87736c |

`firmware/common/src/lz4_codec.cpp` compiles this implementation with `LZ4_MEMORY_USAGE=12`.
The adapter uses `LZ4_compress_fast_extState` with caller-owned, aligned state;
no codec heap allocation is used. Raw blocks map to Parquet enum 7 (`LZ4_RAW`),
not LZ4 Frame or deprecated enum 5. Default acceleration is 1.

Vendored source is excluded from formatting and project lint diagnostics; the
adapter/writer are linted and the actual linked codec runs in ASan/UBSan tests.
Recheck upstream and update this pin/hash record deliberately before upgrades.
