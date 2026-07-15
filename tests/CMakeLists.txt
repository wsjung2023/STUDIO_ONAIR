include(FetchContent)

# GoogleTest is BSD-3-Clause and test-only: it is never linked into a shipped
# binary. Recorded as such in legal/OSS_BOM.csv (CLAUDE.md 7).
# The tag is pinned so the BOM entry keeps describing what we actually build.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)

# Without this, MSVC links gtest against the static CRT while our targets use
# the dynamic CRT, and the link fails with mismatched runtime library errors.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(cs_tests
    core/ResultTest.cpp
    core/TimebaseTest.cpp
)

target_link_libraries(cs_tests
    PRIVATE
        cs_core
        GTest::gtest_main
)

cs_apply_warnings(cs_tests)

gtest_discover_tests(cs_tests)
