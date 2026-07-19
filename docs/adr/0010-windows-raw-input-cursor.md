# ADR-0010: Windows Raw Input Cursor Source

## Status

Accepted as the platform source for the existing Qt-free `ICursorSource` port.

## Decision

`WindowsRawInputCursorSource` owns one message-only window on a dedicated
thread, registers `RIDEV_INPUTSINK | RIDEV_DEVNOTIFY` for the generic mouse
usage, and samples `GetPhysicalCursorPos` for each input packet. It emits only
`RawCursorMoveSample` and button-down `RawCursorClickSample` values, so Win32
types never cross the cursor module boundary. A bounded queue protects the
message thread; click samples evict movement samples first when full.

The source is constructed through `create()`, which reports registration and
geometry failures as `core::Result` errors. `poll()` remains non-blocking and
`error()` exposes the first runtime read failure for the application layer.

## Scope boundary

The durable recorder, project-package metadata, and editor/render consumers
remain separate integration work. The source does not capture keyboard input,
text, clipboard contents, window titles, or application identity.

