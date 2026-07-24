# Avatar Pack Capability Hardening Design

## Goal

Make archive validation operate on one immutable byte source and make a
successful staged pack a move-only filesystem capability rather than a bare
path.

## Immutable archive input

`AvatarPackArchive::open` opens the supplied path before consulting metadata.
On Windows it uses one documented Win32 `CreateFileW` handle with
`FILE_FLAG_OPEN_REPARSE_POINT`, permits only read sharing, and therefore denies
concurrent write and delete access. Handle metadata must describe a
non-reparse regular file within the archive size cap. That exact handle is
converted to a CRT `FILE*`; raw ZIP preflight and miniz share it until archive
destruction. Windows deliberately does not copy archives up to 2 GiB because
the retained sharing contract provides immutability without doubling I/O and
temporary storage.

On POSIX the source is opened with `O_NOFOLLOW`, checked with `fstat`, and copied
with a bounded streaming loop into a `0600` private temporary file. The
temporary directory entry is unlinked immediately. The source descriptor is
closed after the copy, and the snapshot's single `FILE*` is rewound and shared
by raw preflight and miniz. Copying, rather than advisory locking, makes later
source writes and renames irrelevant to validation.

All archive allocation, filesystem, and library exceptions become
`core::Result` errors. No path reopen occurs after the initial no-follow open.

## EOCD parity with miniz

The bounded tail scan rejects any archive whose searchable tail contains more
than one EOCD signature. This strict policy makes the only accepted signature
the same last signature miniz will select. That candidate must end exactly at
the immutable source boundary and pass the complete raw ZIP structural
contract, including comment, disk/count, central-directory, ZIP64,
local-header, overlap, and configured-cap checks. A later invalid or oversized
signature is never ignored in favor of an earlier valid candidate.

## Sealed staging capability

`AvatarPackStaging` remains move-only and becomes the sealed lease returned in
`ValidatedAvatarPack{manifest, staging}`. Creation, writing, extraction, and
sealing are private operations available only to `AvatarPackValidator`.
Sealing retains only the staging parent and root handles or descriptors.
Construction may open child directories transiently, but closes them before
the lease becomes public. The previous bare `stagingRoot` field is removed.

The public lease contract is:

- no filesystem path or path-like diagnostic is exposed;
- bounded `exists(relativePath)` and `read(relativePath, maximumBytes)` access
  files through retained OS capabilities with no-follow and identity checks;
- `cleanup()` explicitly removes owned content and returns a stable
  `avatar.pack.staging.cleanup` I/O error when ownership cannot be proved;
- `promoteTo(finalPath) &&` returns `Result<PromotionOutcome>`. Before rename
  it enumerates the actual tree, requires exact equality with sealed topology,
  revalidates every stored identity/size/SHA-256, flushes the tree, validates
  the destination parent, and performs a no-replace atomic rename through
  retained source authority;
- pre-rename failure is an ordinary error: no destination is published and the
  lease remains active for retry;
- rename success consumes the lease irrevocably. Both source and destination
  parents are flushed. `PromotionOutcome::Durable` means both confirmations
  succeeded; `PromotionOutcome::Indeterminate` means at least one failed, the
  destination may already exist, rollback is forbidden, and startup catalog
  reconciliation is required;
- move construction and move assignment transfer ownership, allowing Task 6
  to accept and extend the capability for secure promotion without converting
  it back to a path.

On Windows, retained root and parent handles include delete authority.
Lease reads reopen files beneath the verified root and require exact
final-handle identity and containment. Cleanup opens each recorded object with
delete authority, verifies its stored identity, and applies handle-relative
disposition deepest-first. Each locally opened cleanup handle is owned by a
scope guard, so identity or disposition failure cannot leak it. Promotion
accepts only current-user-owned staging and destination parents whose DACL
grants dangerous mutation rights only to that user, LocalSystem, or
Administrators. It enumerates the full tree and holds every verified expected
file handle without write/delete sharing through content verification, then
performs a final topology pass. Windows cannot rename an ancestor while those
child handles deny delete sharing, so they are closed immediately before the
root-handle rename. A malicious process running as the same user is explicitly
outside this trusted-private boundary.

On POSIX, the supplied staging parent is accepted only when it is owned by the
effective user, is a directory, and has no group/other access bits. Reads
reopen with `openat` and `O_NOFOLLOW`, then verify identity and content hash.
Cleanup uses retained root/parent descriptors and refuses to unlink the root
name if the trusted-private parent or root identity no longer matches.
Construction retains only parent/root descriptors; child traversal is reopened
with `openat`/`O_NOFOLLOW`, identity-checked, and closed per operation.
Promotion enumerates and hashes relative to the retained root descriptor,
opens and validates a trusted destination parent, and uses
`renameat2(..., RENAME_NOREPLACE)` where available. A platform without an
atomic no-replace primitive fails closed. The trusted-private same-euid
contract is the POSIX exclusion boundary while enumeration and hashing run;
a malicious process with the same effective uid is explicitly outside it.

Before cleanup touches content, the current `parent/name` object must have the
same Windows file identity or POSIX device/inode as the retained root. A
missing, renamed, or replaced root produces cleanup failure. Cleanup must never
delete a replacement at the former root name. The destructor performs only
best-effort fallback cleanup; callers that need the result call `cleanup()`.

Sealing does not retain one handle per archive entry. It stores bounded entry
metadata (relative name, identity, byte count, and authenticated SHA-256) and
keeps only the root/parent capability after construction. Public reads reopen
the requested file and verify identity, size, and hash before returning bytes.
This prevents a valid-shape 10,000-entry package from exhausting the process
descriptor table.

## Exception and ownership boundaries

`AvatarPackValidator::validateAndExtract` is `noexcept`. Exceptions before
staging become stable allocation or I/O results. Exceptions after staging first
attempt explicit cleanup; cleanup failure takes precedence. Archive reads and
streams and staging public operations use the same result-only boundary.
Manifest payload processing uses sorted references instead of copying payload
records.

The `noexcept` API prevents exceptions from crossing the pack boundary and
translates ordinary allocation failures into `AppError` results. As with any
result type that must allocate its fallback error text, process-wide memory
exhaustion can still make construction of that fallback fail and cause C++
termination.

## Tests

Real local fixtures, without production hooks or filesystem mocks, cover:

- a pre-open writable Windows handle;
- concurrent central-directory/EOCD mutation and package replacement;
- the POSIX private-snapshot behavior where supported;
- two structurally valid EOCD candidates;
- a valid EOCD whose comment contains a later invalid, oversized fake EOCD
  signature, proving preflight rejects the signature miniz would select;
- a successful lease subjected to root rename/replacement, proving capability
  reads return the original object or fail closed and never read replacement;
- cleanup identity mismatch preserving replacement and returning the stable
  cleanup error;
- repeated cleanup replacement races preserving every replacement;
- successful no-replace promotion, existing-destination rejection, unsafe
  destination-parent rejection, and retry after failed promotion;
- known-file mutation, unknown file/directory topology, and a concurrent
  mutation handle preventing promotion without publishing a destination;
- deterministic source/destination durability state-machine coverage proving
  both flushes are attempted and any failure yields `Indeterminate`;
- compile-time absence of `displayPath()` and rvalue-only promotion;
- a valid-shape 10,000-entry staging tree without one retained descriptor per
  entry and successful Windows promotion at the 10,000-entry upper bound;
- cleanup identity and disposition failure preserving replacements/content
  while leaving process handle count unchanged;
- compile-time and runtime no-exception public contracts;
- all 45 validator/fuzz tests, repeated focused gates, MSVC ASan, the enabled
  full build, and the complete CTest matrix.

No test-only production hook, fake filesystem, path-authority fallback, or
Windows whole-archive snapshot is introduced.
