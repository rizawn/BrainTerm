# BrainTerm Release Checklist

Complete every item below before tagging a public pre-beta or beta release.

## 1. Repository & Legal Hygiene

- [ ] Add chosen open-source `LICENSE` file at repository root and update `README.md` License section + `docs/THIRD_PARTY_LICENSES.md`.
- [x] Verify `docs/THIRD_PARTY_LICENSES.md` accurately lists all bundled font (`OFL-1.1`) and runtime DLL licenses.
- [x] Ensure `git status` contains zero temporary test vaults, accidental credentials, or personal machine paths in tracked files.

## 2. Clean Build & Automated Verification (`-Dwerror=true`)

- [x] Configure clean build or verify `meson configure build` matches `-Dwerror=true`.
- [ ] Run `meson compile -C build` with 0 warnings on Windows MSYS2 UCRT64 (verified) and Linux CI (pending push/CI trigger).
- [x] Run `meson test -C build --suite unit --suite crash-rig --suite vtcorpus --print-errorlogs -j 1` (0 failures, 0 skipped assertions on desktop Windows).
- [x] Run `meson test -C build --suite bench --print-errorlogs -j 1` and verify all latency/frame-time budgets pass.
- [x] Run `build/src/bt.exe --version` and `build/src/bt.exe --selftest-links vault-template`.
- [ ] Confirm GitHub Actions `.github/workflows/ci.yml` (`windows` and `ubuntu` jobs) passes on target commit.

## 3. Portable Bundle & Isolated Smoke Verification

- [x] Run `powershell -ExecutionPolicy Bypass -File scripts/package-win.ps1 -KillRunning -BuildDir build -OutDir dist\brainterm -SmokeTest`.
- [x] Confirm matching SHA-256 hashes between `build\src\bt.exe` / `dist\brainterm\bt.exe` and `build\src\bt-mcp.exe` / `dist\brainterm\bt-mcp.exe`.
- [x] Confirm `BT_SMOKE ok ...` AND `BT_MCP_SMOKE ok readOnly=true` pass under isolated `PATH` (only Windows System32).
- [x] Verify `dist\brainterm\licenses\` contains `THIRD_PARTY_LICENSES.md` and font `OFL` texts.

## 4. Manual Interactive QA (Required Before General Availability)

- [ ] Open a real multi-folder vault on Windows: test Starter Vault creation, `Ctrl+F`/`Ctrl+H` Find & Replace, math `$...$`/`$$...$$` live compile, clickable links, and external file conflict banner (`Review diff` / `Keep mine` / `Reload`).
- [ ] Test Brain Graph 2D (`Clean` & `Glow`), drag-to-pin, `Re-layout` (`R`), `Highlight Orphans` (`O`), folder collapse, `3D`, `Galaxy`, and `Timeline` (`Organic`/`Kept`) under both GL and `BRAINTERM_GRAPH_BACKEND=cairo`.
- [ ] Test interactive ConPTY TUI resizing (`claude`, `vim`) and Windows IME / dead-key input in editor & terminal.
- [ ] Test interactive Linux X11/Wayland run (`./build/src/bt --vault ...`) for `pty_unix.c`, file dialogs, and OpenGL rendering.
- [ ] Update `CHANGELOG.md`, `docs/DEVELOPMENT_STATUS.md`, and release notes with final tag, build stamp, and SHA-256 checksums.
