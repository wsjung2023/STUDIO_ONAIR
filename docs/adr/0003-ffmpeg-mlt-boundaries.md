# ADR-0003: FFmpeg and MLT Boundaries

## Status
Accepted

## Decision
Use FFmpeg for capture encoding, media I/O, remuxing, and conversion. Use MLT for edit preview and final timeline rendering. Keep both behind adapters.

## Rationale
Using MLT as the product data model would couple project migrations and UX to an external engine. Using FFmpeg alone for a full NLE would require reimplementing too much timeline behavior.

## Consequences
- Domain timeline conversion code is required.
- MLT plugin loading must be controlled.
- FFmpeg build configuration is a release artifact.

## Audited R1-03 baseline

- MLT is pinned to v7.40.0, source commit
  `bef9d89c0c279e558d9625dac3399c2aa3d961bc`, and source archive SHA-256
  `49070c3aa84af719de77875d44a62a1c115aff923aff60657fe6dbaaef877601`.
- The build uses dynamic libraries with `GPL=OFF`, `GPL3=OFF`, and only the
  `core` and `avformat` modules. `avformat` reuses Creator Studio's audited
  dynamic LGPL FFmpeg 8.1.2 build.
- `melt`, MLT XML as project state, uncontrolled module discovery, optional
  modules, GPL-enabled/nonfree FFmpeg, and unmanifested runtime files are not
  permitted in the closed-core package.
- The selected runtime is copied instead of running upstream's broad install
  target. A complete SHA-256 manifest is verified before MLT factory
  initialization.
- Shipping builds stage only runtime roles under the application-local
  `mlt-runtime` directory. On Windows the two MLT imports are delay-loaded,
  the adapter verifies and preloads them before its first imported MLT call,
  and the verified runtime `bin` directory remains registered for the native
  factory lifetime. Missing, corrupt, or wrong-architecture libraries become
  product `AppError` values instead of delay-load process termination. No
  development-machine MLT path is compiled into the application.
- Every manifest file records its component, version, immutable source
  identity, and SPDX license expression. The verifier independently enforces
  exact MLT, FFmpeg, PThreads4W, GNU libiconv, and dlfcn-win32 identities in
  addition to the exact file set and SHA-256 hashes.
- Video layering uses MLT's LGPL core `composite` transition because the other
  compositors belong to excluded optional modules. Audio layering uses the
  LGPL core `mix` transition. Service identifiers are selected only by the
  product-owned graph builder; project data cannot choose arbitrary services.
- MLT's native factory is process-global. A named process-lifetime owner closes
  the native factory during orderly process teardown and releases the verified
  Windows DLL-directory registration. The tiny C++ `Repository` wrapper itself
  is intentionally not deleted across the Release-DLL/Debug-application CRT
  boundary on Windows; doing so crosses heaps. Per-project graphs and media
  services remain deterministically owned and destroyed by each edit engine.
- This is an engineering distribution boundary, not a conclusion about codec
  patent or royalty obligations; those remain an R4 release gate.
