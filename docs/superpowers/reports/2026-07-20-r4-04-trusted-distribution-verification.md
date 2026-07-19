# R4-04 Trusted Distribution Verification

Date: 2026-07-20 (Asia/Seoul)

## Automated evidence

- `UpdateManifestTest`: 6/6 passed. Strict schema, canonical signing payload,
  HTTPS-only targets, canonical hashes, unique platforms, detached signature
  verification, and atomic replacement are covered.
- `DiagnosticBundleTest`: 5/5 passed. Explicit consent, allowlisted local files,
  link/root containment, private-content rejection, durable manifest write, and
  atomic directory publication are covered.
- `ReleaseManifestScript` and `ReleaseArtifactPolicy`: 2/2 passed. Unsigned,
  hash-mismatched, and missing-notice cases fail; synthetic Windows, macOS, and
  Android signing evidence passes only with the platform-specific contract.
- Diagnostic production sources contain no HTTP or HTTPS endpoint.

## Built Android artifacts checked in non-publishable mode

| ABI preset | Artifact SHA-256 | Result |
| --- | --- | --- |
| `android-arm64-debug` | `ef9073a990c22c6c475db7e74bc1db215c6bf18fc702f253097fd9dd20cc8355` | PASS, evidence-only |
| `android-x86_64-debug` | `c2ba95e9e20d5f0f60809f9e0a2a22ad2dc26a69b7918cb1843c840680c550b2` | PASS, evidence-only |

`evidence-only` is intentionally not a publishable signature claim. The CI
workflow generates the same explicit `distribution-mode.json`, validates it,
and uploads build evidence only. It contains no store upload step.

## Authority-dependent release gates

No signing key, certificate, notarization credential, or Play Console authority
was available or added. A real release remains rejected by default until the
corresponding Authenticode, Apple codesign/notarization, or Android certificate
evidence file is supplied and matches the final artifact hash. macOS store
submission and Play upload remain manual external actions.
