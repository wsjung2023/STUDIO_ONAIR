# ADR-0001: Qt/QML Native Desktop

## Status
Accepted

## Decision
Use Qt 6/QML and C++20 for the desktop application.

## Rationale
The product requires native device access, GPU-backed previews, large timelines, hardware textures, low latency, and integration with MLT/FFmpeg. A pure WebView UI would make zero-copy preview integration and platform debugging harder.

## Consequences
- Qt LGPL obligations or commercial licensing must be handled.
- UI development is less familiar than React but media integration is more direct.
- Platform adapters remain C++/Objective-C++/C++/WinRT.
