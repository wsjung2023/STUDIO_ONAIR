CREATE TABLE scenes(
    scene_id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL REFERENCES projects(project_id) ON DELETE CASCADE,
    name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 200),
    position INTEGER NOT NULL CHECK(position BETWEEN 0 AND 1023),
    created_at_utc TEXT NOT NULL CHECK(length(created_at_utc) > 0),
    UNIQUE(project_id, position),
    UNIQUE(project_id, scene_id)
);

CREATE TABLE scene_sources(
    scene_id TEXT NOT NULL REFERENCES scenes(scene_id) ON DELETE CASCADE,
    source_id TEXT NOT NULL CHECK(length(source_id) > 0),
    role TEXT NOT NULL CHECK(role IN ('screen','camera','microphone','system_audio')),
    name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 200),
    position INTEGER NOT NULL CHECK(position BETWEEN 0 AND 1023),
    enabled INTEGER NOT NULL CHECK(enabled IN (0,1)),
    transform_x REAL,
    transform_y REAL,
    transform_width REAL,
    transform_height REAL,
    scale_x REAL,
    scale_y REAL,
    rotation_degrees REAL,
    crop_left REAL,
    crop_top REAL,
    crop_right REAL,
    crop_bottom REAL,
    opacity REAL,
    z_order INTEGER,
    PRIMARY KEY(scene_id, source_id),
    UNIQUE(scene_id, role),
    UNIQUE(scene_id, position),
    CHECK(
        (role IN ('microphone','system_audio') AND
         transform_x IS NULL AND transform_y IS NULL AND
         transform_width IS NULL AND transform_height IS NULL AND
         scale_x IS NULL AND scale_y IS NULL AND
         rotation_degrees IS NULL AND crop_left IS NULL AND
         crop_top IS NULL AND crop_right IS NULL AND
         crop_bottom IS NULL AND opacity IS NULL AND z_order IS NULL) OR
        (role IN ('screen','camera') AND enabled = 0 AND
         transform_x IS NULL AND transform_y IS NULL AND
         transform_width IS NULL AND transform_height IS NULL AND
         scale_x IS NULL AND scale_y IS NULL AND
         rotation_degrees IS NULL AND crop_left IS NULL AND
         crop_top IS NULL AND crop_right IS NULL AND
         crop_bottom IS NULL AND opacity IS NULL AND z_order IS NULL) OR
        (role IN ('screen','camera') AND
         transform_x IS NOT NULL AND transform_y IS NOT NULL AND
         transform_width IS NOT NULL AND transform_height IS NOT NULL AND
         scale_x IS NOT NULL AND scale_y IS NOT NULL AND
         rotation_degrees IS NOT NULL AND crop_left IS NOT NULL AND
         crop_top IS NOT NULL AND crop_right IS NOT NULL AND
         crop_bottom IS NOT NULL AND opacity IS NOT NULL AND
         z_order IS NOT NULL AND
         transform_x BETWEEN 0.0 AND 1.0 AND
         transform_y BETWEEN 0.0 AND 1.0 AND
         transform_width > 0.0 AND transform_width <= 1.0 AND
         transform_height > 0.0 AND transform_height <= 1.0 AND
         scale_x > 0.0 AND scale_x <= 1.7976931348623157e308 AND
         scale_y > 0.0 AND scale_y <= 1.7976931348623157e308 AND
         rotation_degrees BETWEEN -1.7976931348623157e308 AND
                                  1.7976931348623157e308 AND
         crop_left BETWEEN 0.0 AND 1.0 AND
         crop_top BETWEEN 0.0 AND 1.0 AND
         crop_right BETWEEN 0.0 AND 1.0 AND
         crop_bottom BETWEEN 0.0 AND 1.0 AND
         crop_left + crop_right < 1.0 AND
         crop_top + crop_bottom < 1.0 AND
         opacity BETWEEN 0.0 AND 1.0 AND
         z_order BETWEEN -2147483648 AND 2147483647)
    )
);

CREATE TABLE studio_state(
    project_id TEXT PRIMARY KEY REFERENCES projects(project_id) ON DELETE CASCADE,
    active_scene_id TEXT NOT NULL,
    FOREIGN KEY(project_id, active_scene_id)
        REFERENCES scenes(project_id, scene_id)
);

CREATE TABLE recording_sources(
    session_id TEXT NOT NULL REFERENCES recording_sessions(session_id) ON DELETE CASCADE,
    source_id TEXT NOT NULL CHECK(length(source_id) > 0),
    role TEXT NOT NULL CHECK(role IN ('screen','camera','microphone','system_audio')),
    PRIMARY KEY(session_id, source_id),
    UNIQUE(session_id, role)
);

CREATE TABLE recording_scene_events(
    session_id TEXT NOT NULL REFERENCES recording_sessions(session_id) ON DELETE CASCADE,
    sequence INTEGER NOT NULL CHECK(sequence >= 0),
    scene_id TEXT NOT NULL REFERENCES scenes(scene_id) ON DELETE CASCADE,
    position_ns INTEGER NOT NULL CHECK(position_ns >= 0),
    PRIMARY KEY(session_id, sequence)
);

CREATE TABLE recording_markers(
    marker_id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL REFERENCES recording_sessions(session_id) ON DELETE CASCADE,
    position_ns INTEGER NOT NULL CHECK(position_ns >= 0),
    label TEXT NOT NULL CHECK(length(label) <= 200),
    UNIQUE(session_id, marker_id)
);

CREATE TABLE recording_imports(
    session_id TEXT PRIMARY KEY REFERENCES recording_sessions(session_id) ON DELETE CASCADE,
    timeline_id TEXT NOT NULL REFERENCES timelines(timeline_id) ON DELETE CASCADE,
    base_ns INTEGER NOT NULL CHECK(base_ns >= 0),
    imported_revision INTEGER NOT NULL CHECK(imported_revision >= 0),
    imported_at_utc TEXT NOT NULL CHECK(length(imported_at_utc) > 0)
);

CREATE TRIGGER recording_scene_events_same_project
BEFORE INSERT ON recording_scene_events
BEGIN
    SELECT CASE WHEN
        (SELECT project_id FROM recording_sessions
         WHERE session_id = NEW.session_id) !=
        (SELECT project_id FROM scenes WHERE scene_id = NEW.scene_id)
        THEN RAISE(ABORT, 'recording scene must belong to the session project')
    END;
END;

CREATE TRIGGER recording_sources_require_live_session
BEFORE INSERT ON recording_sources
WHEN (SELECT state FROM recording_sessions
      WHERE session_id = NEW.session_id) != 'RECORDING'
BEGIN
    SELECT RAISE(ABORT, 'recording sources require a live session');
END;

CREATE TRIGGER recording_scene_events_require_live_session
BEFORE INSERT ON recording_scene_events
WHEN (SELECT state FROM recording_sessions
      WHERE session_id = NEW.session_id) != 'RECORDING'
BEGIN
    SELECT RAISE(ABORT, 'recording scene events require a live session');
END;

CREATE TRIGGER recording_markers_require_live_session
BEFORE INSERT ON recording_markers
WHEN (SELECT state FROM recording_sessions
      WHERE session_id = NEW.session_id) != 'RECORDING'
BEGIN
    SELECT RAISE(ABORT, 'recording markers require a live session');
END;

CREATE TRIGGER recording_scene_events_monotonic
BEFORE INSERT ON recording_scene_events
BEGIN
    SELECT CASE WHEN NEW.sequence !=
        COALESCE((SELECT MAX(sequence) + 1 FROM recording_scene_events
                  WHERE session_id = NEW.session_id), 0)
        THEN RAISE(ABORT, 'recording scene sequence must be contiguous')
    END;
    SELECT CASE WHEN NEW.position_ns <
        COALESCE((SELECT MAX(position_ns) FROM recording_scene_events
                  WHERE session_id = NEW.session_id), 0)
        THEN RAISE(ABORT, 'recording scene positions must be monotonic')
    END;
END;

CREATE TRIGGER recording_imports_same_project
BEFORE INSERT ON recording_imports
BEGIN
    SELECT CASE WHEN
        (SELECT project_id FROM recording_sessions
         WHERE session_id = NEW.session_id) !=
        (SELECT project_id FROM timelines WHERE timeline_id = NEW.timeline_id)
        THEN RAISE(ABORT, 'recording import timeline must belong to the session project')
    END;
END;

CREATE TRIGGER recording_imports_require_completed_session
BEFORE INSERT ON recording_imports
WHEN (SELECT state FROM recording_sessions
      WHERE session_id = NEW.session_id) NOT IN ('COMPLETED','RECOVERED')
BEGIN
    SELECT RAISE(ABORT, 'recording import requires a completed session');
END;

CREATE TRIGGER scene_sources_protect_unimported_insert
BEFORE INSERT ON scene_sources
WHEN EXISTS(
    SELECT 1 FROM recording_scene_events event
    LEFT JOIN recording_imports imported
        ON imported.session_id = event.session_id
    WHERE event.scene_id = NEW.scene_id AND imported.session_id IS NULL
)
BEGIN
    SELECT RAISE(ABORT, 'scene sources are locked by an unimported recording');
END;

CREATE TRIGGER scene_sources_protect_unimported_update
BEFORE UPDATE ON scene_sources
WHEN EXISTS(
    SELECT 1 FROM recording_scene_events event
    LEFT JOIN recording_imports imported
        ON imported.session_id = event.session_id
    WHERE event.scene_id IN (OLD.scene_id, NEW.scene_id)
      AND imported.session_id IS NULL
)
BEGIN
    SELECT RAISE(ABORT, 'scene sources are locked by an unimported recording');
END;

CREATE TRIGGER scene_sources_protect_unimported_delete
BEFORE DELETE ON scene_sources
WHEN EXISTS(
    SELECT 1 FROM recording_scene_events event
    LEFT JOIN recording_imports imported
        ON imported.session_id = event.session_id
    WHERE event.scene_id = OLD.scene_id AND imported.session_id IS NULL
)
BEGIN
    SELECT RAISE(ABORT, 'scene sources are locked by an unimported recording');
END;

CREATE TRIGGER scenes_protect_unimported_recordings
BEFORE DELETE ON scenes
WHEN EXISTS(
    SELECT 1 FROM recording_scene_events event
    LEFT JOIN recording_imports imported
        ON imported.session_id = event.session_id
    WHERE event.scene_id = OLD.scene_id AND imported.session_id IS NULL
)
BEGIN
    SELECT RAISE(ABORT, 'scene is referenced by an unimported recording');
END;

CREATE INDEX scenes_project_position ON scenes(project_id, position);
CREATE INDEX recording_scene_events_position
    ON recording_scene_events(session_id, position_ns);
CREATE INDEX recording_markers_position
    ON recording_markers(session_id, position_ns);
