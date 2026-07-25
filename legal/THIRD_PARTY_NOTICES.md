# Third-party notices: signed avatar packages

This file records the exact third-party versions used by the signed avatar
package feature. `legal/OSS_BOM.csv` remains the component inventory.

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

## Source availability

The exact upstream source locations above are the reproducible source
locations for these pinned components. MIT and ISC do not impose a copyleft
source-offer requirement; this record does not replace any notices or source
delivery terms required by a particular distribution channel.
