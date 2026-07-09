# Zydis

Vendored amalgamated Zydis 4.1.1 distribution.

- Upstream: https://github.com/zyantific/zydis
- License: MIT, see `LICENSE`
- Local configuration: static build via `ZYDIS_STATIC_BUILD`

`Zydis.c` and `Zydis.h` are generated upstream artifacts and should not be
edited manually. Regenerate from a tagged upstream tree with:

```powershell
git clone --depth 1 --branch v4.1.1 --recursive https://github.com/zyantific/zydis.git zydis-src
..\..\..\scripts\amalgamate_zydis.ps1 -SourceRoot zydis-src -OutputDir .
```
