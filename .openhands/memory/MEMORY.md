# cwebserv (C web server + reverse/forward proxy)

- Build/test: `make test` (unit + functional + edge), `make asan` then `./tests/functional.sh build-asan`, `make memcheck` (macOS `leaks --atExit`; LeakSanitizer unsupported on Apple Silicon), `make load`.
- Not a git repo — `git status` hangs here; don't run it.
- Long shell one-liners garble in this terminal; put complex commands in a /tmp script.

# 구성도 (diagram-atlas)

- The skill's engine/build.mjs were NOT available (only SKILL.md + 아키텍쳐.zip). Engine was extracted verbatim from the zip's built HTML into `docs/atlas/engine/`; `docs/atlas/build.mjs` replaces the skill build (same `AtlasLayout.layout()` checks).
- Rebuild: `node docs/atlas/build.mjs docs/웹서버_구성도_원본 -o docs/웹서버_구성도.html`; verify quotes: `node docs/atlas/check_refs.mjs docs/웹서버_구성도_원본 .` — rerun after any src edit (line numbers drift).
- Glyph tiles (`=TEXT`) overflow above 5 chars.

# Environment quirks

- `~/Documents`, `~/Downloads`, `~/Desktop` hang on access (macOS TCC) — avoid.
- Headless Chrome screenshots hang; browser tool blocks `file://` and can't reach `127.0.0.1` — serve with `python3 -m http.server` and open `http://localhost:PORT`.
