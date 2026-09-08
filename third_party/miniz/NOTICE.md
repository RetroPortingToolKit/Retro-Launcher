# miniz

Vendored amalgamation of [miniz](https://github.com/richgel999/miniz) 3.1.2 —
`miniz.h` and `miniz.c` exactly as published in that release, unmodified.

Used by the Windows portable stub (`src/portable/win_portable_main.cpp`) to
unpack its appended zip payload in-process. The stub cannot link a shared zlib:
the DLLs it would need are inside the payload it is extracting, so the unzip has
to be statically linked and dependency-free.

Compression is compiled out (`MINIZ_NO_DEFLATE_APIS`, `MINIZ_NO_ZLIB_APIS`,
`MINIZ_NO_STDIO`); only the inflate + zip-reading paths are built.

Licensed under the MIT License — see `LICENSE`.
