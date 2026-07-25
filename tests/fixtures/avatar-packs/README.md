# Signed avatar pack acceptance fixtures

There is deliberately no prebuilt `.csavatarpack` in this directory.
`AvatarFoundationAcceptanceTest.cpp` writes a source payload at test runtime,
hashes it through the production manifest codec, creates a real ZIP archive
with pinned miniz, and signs the production validator's message with
libsodium Ed25519. The archive then passes through the production validator
and immutable catalog; no validator bypass or mock catalog is used.

The 32-byte seed in that acceptance source is deterministic test data. It is
not a vendor key, release key, or example key suitable for any other use. CMake
compiles the seed only into `cs_avatar_foundation_acceptance_tests`; the
acceptance suite also scans the Creator Studio executable and the relevant
production libraries to reject accidental inclusion of those bytes.

The fixture manifest grants the commercial-broadcast right solely to exercise
the resolver. It is synthetic test evidence, not a model license, legal
opinion, or approval for any real asset. Imported models remain
user-confirmed.

Run the gate from a Developer PowerShell after bootstrapping the pinned
libsodium prefix:

```powershell
cmake --preset windows-avatar-packs-debug
cmake --build --preset windows-avatar-packs-debug `
  --target cs_avatar_foundation_acceptance_tests
$env:PATH = "$PWD/build/sodium/prefix/bin;$env:PATH"
ctest --test-dir build/windows-avatar-packs-debug `
  -R "AvatarFoundationAcceptance" --output-on-failure
```
