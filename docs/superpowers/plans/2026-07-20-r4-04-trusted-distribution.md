# R4-04 Trusted Distribution, Updates, and Diagnostics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce verifiable release artifacts, signed update metadata, and privacy-preserving local diagnostics without publishing or using account credentials.

**Architecture:** Qt-free release value objects validate canonical metadata and hashes. Cryptographic signature verification is a narrow injected port so platform/package crypto can be audited independently. Diagnostics copy only an explicit allowlist into an atomic local bundle; release scripts reject unsigned or incomplete evidence before any manual publication.

**Tech Stack:** C++20, nlohmann/json, SHA-256 core, PowerShell, GitHub Actions, CTest.

## Global Constraints

- No signing key, store credential, telemetry endpoint, recording, transcript, cursor event, or project content enters the repository or default diagnostics.
- Update metadata is accepted only after schema, channel, version, artifact hash, and detached signature verification.
- macOS notarization and Play upload remain manual authority-dependent actions.
- Every material file publication is same-directory and atomic.

---

### Task 1: Signed update metadata contract

**Files:**
- Create: `src/platform_release/UpdateManifest.h`, `src/platform_release/UpdateManifest.cpp`
- Create: `src/platform_release/UpdateManifestStore.h`, `src/platform_release/UpdateManifestStore.cpp`
- Test: `tests/platform_release/UpdateManifestTest.cpp`
- Modify: `src/platform_release/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `IUpdateSignatureVerifier::verify(payload, signature)` and `UpdateManifestStore::loadVerified(path, verifier)`.

- [x] Write tests for canonical valid metadata, unknown fields, path traversal, non-HTTPS URLs, duplicate targets, wrong SHA-256, invalid signature, and atomic replacement.
- [x] Run the target and verify RED on missing `UpdateManifest.h`.
- [x] Implement a strict parser and canonical signing payload:

```cpp
class IUpdateSignatureVerifier {
public:
    virtual ~IUpdateSignatureVerifier() = default;
    [[nodiscard]] virtual core::Result<void> verify(
        std::string_view canonicalPayload,
        std::span<const std::byte> signature) const = 0;
};
```

- [x] Run `ctest --test-dir build/windows-debug -R UpdateManifestTest --output-on-failure` and expect all tests to pass.
- [x] Commit with `git commit -m "feat(r4): verify signed update metadata"`.

### Task 2: Consent-bound local diagnostic bundle

**Files:**
- Create: `src/platform_release/DiagnosticBundle.h`, `src/platform_release/DiagnosticBundle.cpp`
- Create: `tests/platform_release/DiagnosticBundleTest.cpp`
- Modify: `src/platform_release/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `DiagnosticBundle::create(DiagnosticBundleRequest)` returning the atomically published bundle directory.

- [x] Write tests proving explicit consent is required, symlinks and out-of-root paths are rejected, only named log/build-manifest files are copied, SHA-256 values are recorded, and forbidden extensions/content names are rejected.
- [x] Run RED before the class exists.
- [x] Implement an allowlist of `application.log`, `release-manifest.json`, and `system-summary.json`; use a sibling `.part` directory and rename only after the bundle manifest is durable.
- [x] Run the focused tests and a repository search ensuring diagnostic production code contains no upload URL.
- [x] Commit with `git commit -m "feat(r4): create consented local diagnostics"`.

### Task 3: Artifact signing and store validation gates

**Files:**
- Create: `scripts/verify_release_artifacts.ps1`
- Create: `tests/scripts/ReleaseArtifactPolicyTest.ps1`
- Modify: `.github/workflows/r4-release-foundation.yml`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: release manifest, SBOM/notices, Windows Authenticode status, macOS codesign/notary evidence, Android APK/AAB signing certificate output.
- Produces: a non-zero exit unless every requested platform gate has evidence.

- [ ] Write a script test with unsigned, hash-mismatched, missing-notice, and valid synthetic evidence cases; run RED.
- [ ] Implement `verify_release_artifacts.ps1` with explicit `-Platform`, `-Artifact`, `-Manifest`, and `-EvidenceRoot` parameters and no network side effects.
- [ ] Add CI artifact jobs that call validation in non-publishable mode and upload evidence only; keep store upload absent.
- [ ] Run `ctest --test-dir build/windows-debug -R ReleaseArtifactPolicy --output-on-failure` and validate both Android APKs.
- [ ] Commit with `git commit -m "build(r4): gate signed distribution evidence"`.

## Self-review

- Update authenticity, local diagnostics privacy, and platform artifact evidence are separate rejectable tasks.
- The plan does not invent credentials or claim notarization/Play publication.
- Signature verification cannot be bypassed by a successful hash check alone.
