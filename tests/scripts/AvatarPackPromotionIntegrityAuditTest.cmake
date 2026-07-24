cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED RepositoryRoot OR RepositoryRoot STREQUAL "")
    message(FATAL_ERROR "RepositoryRoot is required")
endif()

file(REAL_PATH "${RepositoryRoot}" RepositoryRoot)
set(StagingSource
    "${RepositoryRoot}/src/avatar_pack_adapter/AvatarPackStaging.cpp")
if(NOT EXISTS "${StagingSource}")
    message(FATAL_ERROR "AvatarPackStaging.cpp is missing")
endif()

file(READ "${StagingSource}" Source)
set(Problems)

string(FIND "${Source}"
    "auto verified = verifyPosixTreeForPromotion();" VerifyIndex)
string(FIND "${Source}"
    "::fsync(directories_.front().descriptor)" RootFlushIndex)
if(VerifyIndex EQUAL -1)
    list(APPEND Problems "POSIX promotion verification call is missing")
else()
    string(SUBSTRING "${Source}" ${VerifyIndex} -1 PosixPromotion)
    string(FIND "${PosixPromotion}"
        "::fsync(directories_.front().descriptor)" RelativeRootFlushIndex)
    string(FIND "${PosixPromotion}" "SYS_renameat2" LinuxRenameIndex)
    string(FIND "${PosixPromotion}" "active_ = false;" LeaseConsumeIndex)
    if(RelativeRootFlushIndex EQUAL -1
        OR LinuxRenameIndex EQUAL -1
        OR LeaseConsumeIndex EQUAL -1
        OR RelativeRootFlushIndex GREATER LinuxRenameIndex
        OR LinuxRenameIndex GREATER LeaseConsumeIndex)
        list(APPEND Problems
            "staging root fsync must succeed after tree verification and before rename/lease consumption")
    endif()
endif()

string(FIND "${Source}"
    "::fdopendir(current.release())" EarlyReleaseIndex)
string(FIND "${Source}"
    "::fdopendir(current.get())" BorrowedOpenIndex)
string(FIND "${Source}"
    "if (stream.get() == nullptr) return false;" OpenFailureIndex)
string(FIND "${Source}"
    "(void)current.release();" SuccessfulReleaseIndex)
if(NOT EarlyReleaseIndex EQUAL -1)
    list(APPEND Problems
        "fdopendir must not receive a descriptor released before success")
endif()
if(BorrowedOpenIndex EQUAL -1
    OR OpenFailureIndex EQUAL -1
    OR SuccessfulReleaseIndex EQUAL -1
    OR BorrowedOpenIndex GREATER OpenFailureIndex
    OR OpenFailureIndex GREATER SuccessfulReleaseIndex)
    list(APPEND Problems
        "fdopendir failure must leave the descriptor in ScopedDescriptor ownership")
endif()

string(FIND "${PosixPromotion}"
    "#elif defined(__APPLE__)" AppleBranchIndex)
string(FIND "${PosixPromotion}" "::renameatx_np(" AppleRenameIndex)
string(FIND "${PosixPromotion}" "RENAME_EXCL" AppleExclusiveIndex)
string(FIND "${PosixPromotion}" "errno = ENOTSUP;" UnsupportedIndex)
if(AppleBranchIndex EQUAL -1
    OR AppleRenameIndex EQUAL -1
    OR AppleExclusiveIndex EQUAL -1
    OR UnsupportedIndex EQUAL -1
    OR LinuxRenameIndex GREATER AppleBranchIndex
    OR AppleBranchIndex GREATER AppleRenameIndex
    OR AppleRenameIndex GREATER AppleExclusiveIndex
    OR AppleExclusiveIndex GREATER UnsupportedIndex)
    list(APPEND Problems
        "macOS promotion must use renameatx_np with RENAME_EXCL before the unsupported POSIX fallback")
endif()

if(Problems)
    list(JOIN Problems "; " ProblemText)
    message(FATAL_ERROR
        "Avatar pack promotion integrity audit failed: ${ProblemText}")
endif()

message(STATUS
    "Avatar pack promotion ordering, descriptor ownership, and platform branches are fail-closed")
