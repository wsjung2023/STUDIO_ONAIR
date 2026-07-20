# R4-05 Commercial Controls Verification

Date: 2026-07-20 (Asia/Seoul)

## Implemented boundaries

- Provider-neutral entitlement decisions cover active, expired, offline grace,
  grace expiry, revocation, product mismatch, trusted-clock rollback, and the
  explicit community/development build mode.
- Opaque receipts are bounded to 1 MiB, passed only to an injected verifier,
  and never persisted. Durable state contains only the verified assertion,
  provider id, and monotonic online/observed timestamps.
- Diagnostics reject receipt and account-id fields in addition to recording,
  transcript, project, cursor, caption, credential, and media-path fields.
- Account/privacy filesystem work runs on a dedicated worker thread. Sign-out
  removes only local session and entitlement files; confirmed deletion is
  restricted to the configured account-state child root and preserves sibling
  project paths.
- Settings exposes entitlement status/reason, opt-in diagnostics consent,
  local sign-out, and two-step local deletion at 360x640. No price, purchase
  button, store, tax, refund, or identity provider is invented.

## Verification

| Check | Result |
| --- | --- |
| `EntitlementPolicyTest` | 8/8 PASS |
| `EntitlementStoreTest` | 6/6 PASS |
| `CommercialControlsControllerTest` | 4/4 PASS |
| Compact Settings QML smoke | 1/1 PASS |
| Final Windows CTest | 1,169/1,169 PASS |
| Android x86_64 debug APK | BUILD SUCCESSFUL |
| Android arm64-v8a debug APK | BUILD SUCCESSFUL |

## Authority boundary

The current application selects the explicit `community-development-build`
decision. Connecting a real receipt verifier, product configuration, remote
sign-out, pricing, or a store transaction remains blocked on actual business
and account-provider authority; none is claimed by this implementation.
