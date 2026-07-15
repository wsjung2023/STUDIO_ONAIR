# Separate target on purpose: when the real capture and recorder land in R0-03,
# removing the fakes from the shipped binary is deleting one line from cs_app's
# link list, not hunting for fake code mixed into production files.
cs_add_qtfree_library(cs_fakes
    SOURCES
        FakeCaptureSource.h
        FakeCaptureSource.cpp
        FakeRecorder.h
        FakeRecorder.cpp
    DEPENDS
        cs_capture
        cs_recorder
)
