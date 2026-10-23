# Vendored third-party code

Both are MIT-licensed single-file libraries, vendored so the core builds with no package manager
(none is available to the C# side on this machine, and the core must also build on macOS for Fusion).

| Library | Version | Source | SHA-256 of download |
|---|---|---|---|
| miniz (`miniz.c`, `miniz.h`, `LICENSE`) | 3.0.2 | https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip | `ADA38DB0B703A56D3DD6D57BF84A9C5D664921D870D8FEA4DB153979FB5332C5` (zip) |
| nlohmann/json (`json.hpp`) | 3.11.3 | https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp | `9BEA4C8066EF4A1C206B2BE5A36302F8926F7FDC6087AF5D20B417D0CF103EA6` |

Fetched 2026-10-21. Do not edit these files; update by replacing them and the table above.
nlohmann/json's MIT licence text is in the header of `json.hpp`.
