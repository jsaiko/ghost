# UI font

`DejaVuSans-subset.ttf` is DejaVu Sans (https://dejavu-fonts.github.io/),
subset to Basic Latin + Latin-1 (`U+0020-007E`, `U+00A0-00FF`) with
`pyftsubset --layout-features='' --no-hinting`, so the ~760KB face becomes
~16KB to embed. It is compiled into the `spectre` binary as a C array
(`ui_font_ttf.h`, see `client/spectre/CMakeLists.txt`) and rasterized at runtime by
the vendored `third_party/stb_truetype.h` -- `src/ui/font.cpp`.

License: Bitstream Vera (permissive; DejaVu's own changes are public domain)
-- `LICENSE-DejaVu.txt` here, which must ship with any redistribution.
