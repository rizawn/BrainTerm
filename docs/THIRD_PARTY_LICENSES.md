# Third-Party Licenses & Notices

## Project License Status

No top-level `LICENSE` file is present in the BrainTerm repository at this time. Per [README.md](../README.md), until the repository owner publishes a chosen open-source license at the root, all rights to BrainTerm's original source code are reserved by the author.

---

## Bundled Fonts

All fonts bundled under `data/fonts/` and packaged into `dist/brainterm/share/brainterm/fonts/` are distributed under the **SIL Open Font License, Version 1.1 (OFL-1.1)**:

| Asset | Project | Copyright | License Text |
|---|---|---|---|
| `InterVariable.ttf` | Inter | Copyright (c) 2016 The Inter Project Authors (<https://github.com/rsms/inter>) | [LICENSE-Inter.txt](../data/fonts/LICENSE-Inter.txt) |
| `JetBrainsMono-Regular.ttf`, `JetBrainsMono-Bold.ttf` | JetBrains Mono | Copyright 2020 The JetBrains Mono Project Authors (<https://github.com/JetBrains/JetBrainsMono>) | [OFL-JetBrainsMono.txt](../data/fonts/OFL-JetBrainsMono.txt) |
| `STIXTwoMath-Regular.otf` | STIX Two Math | Copyright 2001-2021 The STIX Fonts Project Authors (<https://github.com/stipub/stixfonts>) | [LICENSE-STIXTwoMath.txt](../data/fonts/LICENSE-STIXTwoMath.txt) |

---

## Dynamically Linked Libraries (MSYS2 UCRT64 Verification Profile)

On Windows portable builds (`scripts/package-win.ps1`), transitive runtime DLLs from `C:\msys64\ucrt64\bin` are bundled alongside `bt.exe` and `bt-mcp.exe` via `objdump -p`. Package versions and licenses below were recorded from MSYS2 UCRT64 (`pacman -Qi`) in the active verification environment:

| Component | Version (UCRT64) | License (per MSYS2 `pacman -Qi`) | Upstream URL |
|---|---|---|---|
| `glib2` (`glib-2.0`, `gio-2.0`) | `2.88.2-1` | `spdx:LGPL-2.1-or-later` | <https://gitlab.gnome.org/GNOME/glib> |
| `gtk4` | `4.22.4-1` | `spdx:LGPL-2.1-or-later` | <https://www.gtk.org> |
| `libadwaita` | `1.9.1-1` | `spdx:LGPL-2.1-or-later` | <https://gnome.pages.gitlab.gnome.org/libadwaita/> |
| `gtksourceview5` | `5.18.0-1` | `spdx:LGPL-2.1-or-later` | <https://wiki.gnome.org/Projects/GtkSourceView> |
| `libepoxy` | `1.5.10-7` | `spdx:MIT` | <https://github.com/anholt/libepoxy> |
| `harfbuzz` | `14.2.1-1` | `spdx:MIT` | <https://harfbuzz.github.io/> |
| `freetype` | `2.14.3-1` | `spdx:GPL-2.0-or-later OR FTL` | <https://www.freetype.org/> |
| `cairo` | `1.18.4-4` | `spdx:LGPL-2.1-or-later OR MPL-1.1` | <https://cairographics.org/> |
| `pango` | `1.58.0-1` | `spdx:LGPL-2.1` | <https://www.pango.org/> |
| `libvterm` | `0.3.3-1` | `spdx:MIT` | <https://www.leonerd.org.uk/code/libvterm/> |
| `libgit2` | `1.9.4-1` | `GPL2 custom` (GPL-2.0 with linking exception) | <https://libgit2.org/> |
| `sqlite3` | `3.53.3-1` | `PublicDomain` | <https://www.sqlite.org/> |
| `md4c` | `0.5.3-1` | `spdx:MIT` | <https://github.com/mity/md4c> |
| `libyaml` | `0.2.5-2` | `MIT` | <https://yaml.org/> |
| `cjson` | `1.7.19-1` | `spdx:MIT` | <https://github.com/DaveGamble/cJSON> |
| `gcc-libs` | `16.1.0-5` | `spdx:GPL-3.0-or-later WITH GCC-exception-3.1 AND LGPL-2.1-or-later` | <https://gcc.gnu.org> |
| `cmocka` (test suite only) | `1.1.8-3` | `spdx:Apache-2.0` | <https://cmocka.org/> |

Note: `scripts/package-win.ps1` copies font license texts and this file into `dist/brainterm/licenses/`. When distributing portable binaries that bundle third-party LGPL/GPL/FTL/MIT shared libraries, keep `licenses/` alongside the executables and preserve upstream attribution notices.
