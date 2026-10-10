# Third-party notices

This file records the third-party components shipped with Creator Studio,
with the notice each license requires. `legal/OSS_BOM.csv` remains the
component inventory. The first sections cover the signed avatar package
feature; "Application binary" covers everything the installed app links.

## miniz 3.1.2

- Use: static ZIP reader/writer in `cs_avatar_pack_adapter` and tests.
- License: MIT.
- Pinned release archive:
  <https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip>
- Required archive SHA-256:
  `f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a`
- Corresponding source and license:
  <https://github.com/richgel999/miniz/tree/3.1.2>

Copyright 2013-2014 RAD Game Tools and Valve Software

Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC

All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

## libsodium 1.0.22

- Use: dynamically linked Ed25519 verification in
  `cs_avatar_pack_adapter`; signing is test-only.
- License: ISC.
- Audited Windows package:
  <https://download.libsodium.org/libsodium/releases/libsodium-1.0.22-msvc.zip>
- Required package SHA-256:
  `3e03a726fac4bc09cb61d8f29d658ef7a5eca0811de59082130414f7ca2e4279`
- Corresponding exact source and license:
  <https://github.com/jedisct1/libsodium/tree/1.0.22>

Copyright (c) 2013-2026 Frank Denis

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.

## Application binary: shipped third-party components

The installed application (`cmake --install`) places the texts below under
`legal/` next to the executable. The license texts are copied from the exact
dependency builds the binary links against, so the notice always matches
what ships.

### Qt 6.8.3 (LGPL-3.0)

- Use: UI, QML, multimedia device enumeration; every Qt module is linked
  **dynamically** (`Qt6*.dll`), none is modified.
- License: GNU LGPL v3 (`legal/licenses/LGPL-3.0.txt`, which supplements the
  GNU GPL v3 in `legal/licenses/GPL-3.0.txt`).
- Relinking: because Qt is dynamically linked, a user may replace the
  installed `Qt6*.dll` files with their own build of the same Qt version.
  Qt source: <https://download.qt.io/official_releases/qt/6.8/6.8.3/>.

### FFmpeg 8.1.2 (LGPL-2.1-or-later), audited build

- Use: recording encode/mux, probing, resampling, via `cs_ffmpeg_adapter`;
  dynamic libraries (`avcodec`, `avformat`, `avutil`, `swscale`, `swresample`).
- Built with **no** `--enable-gpl` and **no** `--enable-nonfree`
  (`scripts/bootstrap_ffmpeg.ps1` records the configure line, the official
  release archive and its SHA-256). Not modified.
- Text: `legal/third_party/ffmpeg/copyright` (LGPL v2.1). zlib notice:
  `legal/third_party/zlib/copyright`.

### MLT Framework 7.40.0 (LGPL-2.1), audited build

- Use: editor preview and export graph via `cs_mlt_adapter`; dynamic
  libraries, only the `core` and `avformat` modules are staged (GPL/GPL3
  module groups are built OFF; `melt` is not packaged).
- Pinned source commit `bef9d89c0c279e558d9625dac3399c2aa3d961bc`
  (`build/mlt/prefix/creator-studio-mlt-build.txt`). Not modified.
- Text: `legal/third_party/mlt/COPYING` (LGPL v2.1).

### GNU libiconv (LGPL-2.1-or-later)

- Use: character-set conversion required by MLT; dynamic library staged with
  the MLT runtime. Not modified.
- Text: `legal/third_party/libiconv/copyright`.

### PThreads4W 3.0.0 (Apache-2.0) — MODIFIED

- Use: POSIX threads shim required by MLT on Windows; dynamic library staged
  with the MLT runtime.
- **Modification notice (Apache-2.0 §4(b)):** Creator Studio builds this
  library with
  `build/mlt/vcpkg-overlay-ports/pthreads/creator-studio-lazy-mutex-events.patch`
  (mutex event objects are created lazily instead of at mutex init). The
  patch file is the complete record of the change.
- Text: `legal/third_party/pthreads/copyright`.

### dlfcn-win32 (MIT)

- Use: `dlopen`/`dlsym` shim for the staged MLT runtime. Not modified.
- Text: `legal/third_party/dlfcn-win32/copyright`.

### nlohmann/json 3.11.3 (MIT) and pboettch/json-schema-validator 2.4.0 (MIT)

- Use: project manifest read/write and JSON Schema validation, compiled into
  `cs_project_store`. Not modified.
- Texts: `legal/third_party/nlohmann_json/LICENSE.MIT`,
  `legal/third_party/json-schema-validator/LICENSE`.

### SQLite (public domain)

- Use: project database; the amalgamation is compiled into
  `cs_project_store`. No notice is required; recorded here for completeness.

### Pretendard (SIL Open Font License 1.1)

- Use: application UI typeface; four unmodified static weights embedded as
  Qt resources (`resources/fonts/Pretendard-*.otf`). Reserved Font Name
  "Pretendard".
- Text: `legal/licenses/OFL-1.1-Pretendard.txt`.

### whisper.cpp + ggml-tiny.en (MIT), only when `CS_ENABLE_WHISPER=ON`

- Use: local transcription via `cs_whisper_adapter`. Not modified.
- Text: installed from the whisper prefix as
  `legal/third_party/whisper/LICENSE` when the feature is built.

## Source availability

The exact upstream source locations above are the reproducible source
locations for these pinned components. MIT and ISC do not impose a copyleft
source-offer requirement; this record does not replace any notices or source
delivery terms required by a particular distribution channel.
