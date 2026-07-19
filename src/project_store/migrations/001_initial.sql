CREATE TABLE schema_migrations(
    version INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    checksum TEXT NOT NULL,
    applied_at_utc TEXT NOT NULL
);

CREATE TABLE projects(
    project_id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    manifest_schema_version INTEGER NOT NULL,
    created_at_utc TEXT NOT NULL,
    updated_at_utc TEXT NOT NULL
);

CREATE TABLE recording_sessions(
    session_id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL REFERENCES projects(project_id),
    state TEXT NOT NULL CHECK(state IN ('RECORDING','COMPLETED','RECOVERED','ABORTED')),
    started_ns INTEGER NOT NULL CHECK(started_ns >= 0),
    stopped_ns INTEGER CHECK(stopped_ns IS NULL OR stopped_ns >= started_ns),
    created_at_utc TEXT NOT NULL,
    finished_at_utc TEXT,
    failure_reason TEXT
);

CREATE TABLE segments(
    session_id TEXT NOT NULL REFERENCES recording_sessions(session_id),
    source_id TEXT NOT NULL,
    segment_index INTEGER NOT NULL CHECK(segment_index >= 0),
    start_ns INTEGER NOT NULL CHECK(start_ns >= 0),
    duration_ns INTEGER CHECK(duration_ns >= 0),
    status TEXT NOT NULL CHECK(status IN ('WRITING','READY','FAILED')),
    relative_path TEXT NOT NULL,
    CHECK(status != 'READY' OR duration_ns IS NOT NULL),
    PRIMARY KEY(session_id, source_id, segment_index)
);

CREATE INDEX recording_sessions_project_state
    ON recording_sessions(project_id, state);
CREATE INDEX segments_session_status
    ON segments(session_id, status);
