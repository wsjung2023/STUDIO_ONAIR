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

## EOCD candidate selection

The bounded tail scan collects every EOCD signature whose declared comment
ends exactly at the snapshot boundary. Each candidate is independently checked
against the complete raw ZIP structural contract, including disk/count,
central-directory, ZIP64, local-header, and overlap rules. Validation succeeds
only when exactly one candidate is structurally valid. Zero or multiple valid
candidates return the stable archive-envelope error.

## Sealed staging capability

`AvatarPackStaging` remains move-only and becomes the sealed lease returned in
`ValidatedAvatarPack{manifest, staging}`. Creation, writing, extraction, and
sealing are private operations available only to `AvatarPackValidator`.
Sealing retains the staging parent, root, and created-directory handles or
descriptors. The previous bare `stagingRoot` field is removed.

The public lease contract is:

- `displayPath()` returns a non-authoritative path for diagnostics and UI only;
- bounded `exists(relativePath)` and `read(relativePath, maximumBytes)` access
  files through retained OS capabilities with no-follow and identity checks;
- `cleanup()` explicitly removes owned content and returns a stable
  `avatar.pack.staging.cleanup` I/O error when ownership cannot be proved;
- move construction and move assignment transfer ownership, allowing Task 6
  to accept and extend the capability for secure promotion without converting
  it back to a path.

On Windows, retained no-delete-share handles prevent root/directory rename.
Lease reads open beneath verified final paths and require exact final-handle
identity and containment. On POSIX, reads use `openat` from the retained root
descriptor with `O_NOFOLLOW`; a root rename does not redirect the descriptor.

Before cleanup touches content, the current `parent/name` object must have the
same Windows file identity or POSIX device/inode as the retained root. A
missing, renamed, or replaced root produces cleanup failure. Cleanup must never
delete a replacement at the display path. The destructor performs only
best-effort fallback cleanup; callers that need the result call `cleanup()`.

## Exception and ownership boundaries

`AvatarPackValidator::validateAndExtract` is `noexcept`. Exceptions before
staging become stable allocation or I/O results. Exceptions after staging first
attempt explicit cleanup; cleanup failure takes precedence. Archive reads and
streams and staging public operations use the same result-only boundary.
Manifest payload processing uses sorted references instead of copying payload
records.

## Tests

Real local fixtures, without production hooks or filesystem mocks, cover:

- a pre-open writable Windows handle;
- concurrent central-directory/EOCD mutation and package replacement;
- the POSIX private-snapshot behavior where supported;
- two structurally valid EOCD candidates;
- a successful lease subjected to root rename/replacement, proving capability
  reads return the original object or fail closed and never read replacement;
- cleanup identity mismatch preserving replacement and returning the stable
  cleanup error;
- compile-time and runtime no-exception public contracts;
- the existing 27 validator/fuzz tests, repeated focused gates, MSVC ASan, the
  enabled full build, and the complete CTest matrix.

No test-only production hook, fake filesystem, path-authority fallback, or
Windows whole-archive snapshot is introduced.
