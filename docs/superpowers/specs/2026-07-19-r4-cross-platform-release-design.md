# R4 Cross-Platform Commercial Release Design

## Goal

Ship Creator Studio as a commercial product for Windows, macOS, and Android
without creating a reduced mobile companion. The three applications share the
same project, editing, AI, cursor-intelligence, and avatar semantics. Platform
capture, media acceleration, signing, and distribution remain behind explicit
adapters because their operating-system contracts differ.

## Product boundary

Android is a first-class Creator Studio client: recording, project recovery,
timeline editing, captions/transcription, cursor intelligence where the OS
provides the input, audio cleanup, avatar use, and export are product features.
It is not a remote control or upload-only application.

The operating system remains authoritative for protected content. DRM video,
secure surfaces, and audio that Android does not expose through a
user-approved MediaProjection session are reported as unavailable rather than
bypassed or misrepresented.

The initial commercial desktop form remains a closed-source Windows/macOS
desktop product. Third-party notices, relinking material, source offers where
applicable, and platform-store obligations are release gates, not deferred UI
text.

## Architecture

The existing Qt-free C++ core stays common across every platform: `core`,
`domain`, project storage, recorder contracts, timeline/edit contracts,
transcription, audio DSP, cursor analysis, and avatar pipeline. Qt Quick
continues as the application and adaptive UI layer.

Each platform supplies a narrow native adapter set:

| Capability | Windows | macOS | Android |
| --- | --- | --- | --- |
| Screen capture | Windows Graphics Capture | ScreenCaptureKit | MediaProjection |
| Camera/microphone | existing Windows capture backend | AVFoundation | Camera + AudioRecord |
| System audio | platform-approved loopback | ScreenCaptureKit audio | MediaProjection playback capture when OS/user policy permits |
| Video encode | audited FFmpeg/Media Foundation selection | VideoToolbox/approved FFmpeg path | MediaCodec plus audited portable fallback where required |
| Distribution | signed installer | signed/notarized DMG or PKG | signed AAB and Play-compatible asset delivery |

Platform code must implement existing ports or a new port introduced in the
shared layer. Shared layers do not include Android, Apple, Windows, Play, or
store SDK headers.

## R4 delivery sequence

R4 is too broad for one unsafe change set, so it is delivered as five
independently releasable work packages. None removes the Android full-product
goal.

### R4-01: Cross-platform release foundation

Define a versioned platform capability manifest, supported-version policy,
build presets, deterministic release metadata, SBOM/notices bundle, and a CI
matrix that produces non-publishable artifacts for Windows, macOS, and Android.
The application must show a clear reason when a requested capability is absent
on the current device. This package has no store credentials and publishes
nothing externally.

### R4-02: Android capture and media adapters

Add Android native bridges for MediaProjection, camera, microphone, permitted
playback capture, MediaCodec export, lifecycle interruption, and scoped
storage. Recording/recovery state uses the same project package contract as
desktop. Every native lifecycle callback is generation-bound and idempotent.

### R4-03: Adaptive mobile editing and performance

Make the existing Qt Quick Studio and Editor flows usable by touch without
changing edit semantics. Add device-class preview budgets, background export
constraints, model download/storage controls, and deterministic low-resource
fallbacks. A project saved on Android opens unchanged on desktop and vice versa.

### R4-04: Trusted distribution and updates

Create signed release artifacts, platform-specific installer/AAB validation,
crash diagnostics with explicit consent, update metadata with signed hashes,
and reproducible release evidence. macOS notarization and Google Play upload
remain manual release actions until the account credentials and publishing
authority are explicitly provided.

### R4-05: Commercial controls

Add an entitlement boundary, offline grace policy, receipt/provider adapters,
and account/privacy controls. The implementation deliberately waits for the
business decisions that materially define it: pricing, subscription versus
perpetual purchase, identity provider, and regional tax/store policy.

## R4-01 interfaces

`platform_release::PlatformCapability` is a stable shared value object for a
feature identifier, availability state, supported OS/version range, and
human-readable unavailable reason. `PlatformCapabilityRegistry` exposes the
current device's immutable capability snapshot to the app layer.

`release::ReleaseManifest` contains the product version, source revision,
build target, artifact hashes, and the generated SBOM/notices references. The
release process writes this manifest only after all artifact files are final;
publication is atomic.

The app layer reads these objects to render capability/status UI. It never
guesses feature support from OS names in QML.

## Failure and privacy rules

- A denied permission, unavailable encoder, unsupported OS version, or missing
  model is a recoverable capability state with a user-facing reason.
- Native capture, encoder, and callback errors never corrupt a project or
  silently fall back to a lower-quality format.
- Diagnostics are local by default. Upload requires an explicit user action;
  recordings, transcripts, cursor telemetry, and project content are excluded
  unless a later consented support-bundle feature explicitly includes a selected
  artifact.
- Release builds contain no development signing keys, store credentials, or
  unrestricted telemetry endpoints.

## Verification

R4-01 must have Qt-free unit tests for capability and release-manifest
validation, application/QML tests proving unavailable states are visible, and
script tests proving every generated artifact has a matching hash and notice
bundle. CI must build each enabled platform target; the physical device gates
remain explicit rather than simulated.

R4-02 through R4-04 add platform-device acceptance matrices. A successful
desktop test never substitutes for Android or macOS permission, lifecycle,
thermal, or store-signing evidence.

## Explicit non-goals of R4-01

R4-01 does not choose a payment provider, collect payments, upload to a store,
perform notarization, use Play credentials, or claim physical-device acceptance.
Those actions require commercial account authority or platform hardware and are
covered by later R4 work packages.
