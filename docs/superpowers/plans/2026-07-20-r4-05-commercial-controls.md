# R4-05 Commercial Controls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add provider-neutral entitlement, offline grace, receipt verification, and account/privacy controls without inventing pricing or identity policy.

**Architecture:** A Qt-free entitlement policy evaluates signed provider assertions against a trusted clock and persisted last-online check. Provider verification and account actions are ports; the app controller exposes only effective access, reason, and user-authorized privacy actions. With no configured provider, the build runs in an explicit community/development entitlement rather than pretending a purchase occurred.

**Tech Stack:** C++20, nlohmann/json, Qt 6.8/QML, GoogleTest, CTest.

## Global Constraints

- Pricing, purchase type, identity provider, tax, store, and refund policy are configuration inputs, never hard-coded guesses.
- Clock rollback never extends offline grace.
- Receipts and account identifiers are excluded from diagnostics by default.
- Sign-out and local-data deletion are distinct, explicit actions.

---

### Task 1: Entitlement and offline grace policy

**Files:**
- Create: `src/platform_release/EntitlementPolicy.h`, `src/platform_release/EntitlementPolicy.cpp`
- Create: `tests/platform_release/EntitlementPolicyTest.cpp`
- Modify: `src/platform_release/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `EntitlementPolicy::evaluate(EntitlementAssertion, EntitlementClockState)` returning `EntitlementDecision`.

- [x] Write tests for active, expired, grace-active, grace-expired, revoked, product mismatch, clock rollback, and community-build decisions.
- [x] Run RED before the header exists.
- [x] Implement explicit states:

```cpp
enum class EntitlementState { Active, OfflineGrace, Unavailable };
struct EntitlementDecision final {
    EntitlementState state;
    bool commercialFeaturesAllowed;
    std::string reason;
};
```

- [x] Run `ctest --test-dir build/windows-debug -R EntitlementPolicyTest --output-on-failure`.
- [x] Commit with `git commit -m "feat(r4): enforce offline entitlement policy"`.

### Task 2: Receipt/provider boundary and durable state

**Files:**
- Create: `src/platform_release/IReceiptVerifier.h`
- Create: `src/platform_release/EntitlementStore.h`, `src/platform_release/EntitlementStore.cpp`
- Create: `tests/platform_release/EntitlementStoreTest.cpp`
- Modify: `src/platform_release/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: provider-specific opaque receipt bytes through `IReceiptVerifier`.
- Produces: a validated assertion and atomically stored last-online/grace state.

- [ ] Write tests that reject an empty provider id, oversized receipt, invalid signature result, unknown JSON fields, rollback of last-online time, and torn replacement.
- [ ] Run RED.
- [ ] Implement a 1 MiB receipt bound, injected verifier, strict JSON state, and same-directory atomic replace.
- [ ] Run focused tests and commit with `git commit -m "feat(r4): add receipt verification boundary"`.

### Task 3: Account and privacy controls

**Files:**
- Create: `src/app/CommercialControlsController.h`, `src/app/CommercialControlsController.cpp`
- Create: `qml/SettingsPage.qml`
- Create: `tests/app/CommercialControlsControllerTest.cpp`
- Modify: `src/app/CMakeLists.txt`, `qml/Main.qml`, `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/app/QmlSmokeTest.cpp`, `src/main.cpp`

**Interfaces:**
- Produces: QML properties `entitlementState`, `entitlementReason`, `diagnosticsConsent`, plus invokables `setDiagnosticsConsent(bool)`, `signOut()`, and `deleteLocalAccountData(bool confirmed)`.

- [ ] Write controller tests proving consent defaults false, sign-out preserves projects, deletion without confirmation is rejected, and confirmed deletion targets only the configured account-state root.
- [ ] Write a QML smoke test proving Settings exposes status, consent, sign-out, and a two-step deletion confirmation at 360x640.
- [ ] Run both tests RED.
- [ ] Implement the controller and adaptive Settings page; add it to navigation without enabling any provider-specific purchase button.
- [ ] Run focused QML/controller tests, full Windows CTest, and both Android APK builds.
- [ ] Commit with `git commit -m "feat(r4): expose account and privacy controls"`.

## Self-review

- Every commercial state has a reason and test; no price or vendor is assumed.
- Destructive local deletion requires explicit confirmation and a validated narrow root.
- Provider selection and store transactions remain blocked on actual business/account authority, while the internal product boundary is complete and testable.
