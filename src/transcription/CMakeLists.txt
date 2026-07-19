# R2-04 local captions/transcription core. Qt-free (CLAUDE.md 3/5): it holds the
# domain, the port, a deterministic fake, and JSON serialization/storage, and
# links no Qt, FFmpeg, or MLT. No real speech engine (whisper.cpp) is pulled in
# yet, so no new third-party dependency and legal/OSS_BOM.csv is untouched.
cs_add_qtfree_library(cs_transcription
    SOURCES
        AudioInput.h
        TranscriptWord.h
        TranscriptSegment.h
        TranscriptSegment.cpp
        Transcript.h
        Transcript.cpp
        ITranscriptionProvider.h
        FakeTranscriptionProvider.h
        FakeTranscriptionProvider.cpp
        TranscriptSerializer.h
        TranscriptSerializer.cpp
        TranscriptStore.h
        TranscriptStore.cpp
    DEPENDS
        cs_core
        cs_domain
        nlohmann_json::nlohmann_json
)
