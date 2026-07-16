CREATE TABLE media_assets(
    asset_id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL REFERENCES projects(project_id) ON DELETE CASCADE,
    kind TEXT NOT NULL CHECK(kind IN ('VIDEO','AUDIO','IMAGE')),
    relative_path TEXT NOT NULL CHECK(length(relative_path) > 0),
    duration_ns INTEGER NOT NULL CHECK(duration_ns > 0),
    width INTEGER,
    height INTEGER,
    frame_rate_numerator INTEGER,
    frame_rate_denominator INTEGER,
    sample_rate INTEGER,
    channels INTEGER,
    file_size INTEGER NOT NULL CHECK(file_size > 0),
    fingerprint TEXT NOT NULL CHECK(length(fingerprint) > 0),
    availability TEXT NOT NULL CHECK(availability IN ('AVAILABLE','OFFLINE')),
    CHECK(
        (kind = 'VIDEO' AND width > 0 AND height > 0 AND
         frame_rate_numerator > 0 AND frame_rate_denominator > 0 AND
         ((sample_rate IS NULL AND channels IS NULL) OR
          (sample_rate > 0 AND channels > 0))) OR
        (kind = 'AUDIO' AND width IS NULL AND height IS NULL AND
         frame_rate_numerator IS NULL AND frame_rate_denominator IS NULL AND
         sample_rate > 0 AND channels > 0) OR
        (kind = 'IMAGE' AND width > 0 AND height > 0 AND
         frame_rate_numerator > 0 AND frame_rate_denominator > 0 AND
         sample_rate IS NULL AND channels IS NULL)
    ),
    UNIQUE(project_id, relative_path)
);

CREATE TABLE timelines(
    timeline_id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL REFERENCES projects(project_id) ON DELETE CASCADE,
    name TEXT NOT NULL CHECK(length(name) > 0),
    frame_rate_numerator INTEGER NOT NULL CHECK(frame_rate_numerator > 0),
    frame_rate_denominator INTEGER NOT NULL CHECK(frame_rate_denominator > 0),
    revision INTEGER NOT NULL DEFAULT 0 CHECK(revision >= 0),
    is_primary INTEGER NOT NULL DEFAULT 0 CHECK(is_primary IN (0,1))
);

CREATE UNIQUE INDEX timelines_one_primary_per_project
    ON timelines(project_id) WHERE is_primary = 1;

CREATE TABLE tracks(
    track_id TEXT PRIMARY KEY,
    timeline_id TEXT NOT NULL REFERENCES timelines(timeline_id) ON DELETE CASCADE,
    kind TEXT NOT NULL CHECK(kind IN ('VIDEO','AUDIO','TITLE','CAPTION')),
    name TEXT NOT NULL CHECK(length(name) > 0),
    position INTEGER NOT NULL CHECK(position >= 0),
    enabled INTEGER NOT NULL CHECK(enabled IN (0,1)),
    locked INTEGER NOT NULL CHECK(locked IN (0,1)),
    UNIQUE(timeline_id, position)
);

CREATE TABLE clips(
    clip_id TEXT PRIMARY KEY,
    track_id TEXT NOT NULL REFERENCES tracks(track_id) ON DELETE CASCADE,
    clip_kind TEXT NOT NULL CHECK(clip_kind IN ('ASSET','TITLE','CAPTION')),
    asset_id TEXT REFERENCES media_assets(asset_id),
    media_kind TEXT CHECK(media_kind IN ('VIDEO','AUDIO','IMAGE')),
    source_start_ns INTEGER NOT NULL CHECK(source_start_ns >= 0),
    source_duration_ns INTEGER NOT NULL CHECK(source_duration_ns > 0),
    timeline_start_ns INTEGER NOT NULL CHECK(timeline_start_ns >= 0),
    timeline_duration_ns INTEGER NOT NULL CHECK(timeline_duration_ns > 0),
    enabled INTEGER NOT NULL CHECK(enabled IN (0,1)),
    CHECK(source_duration_ns = timeline_duration_ns),
    CHECK(
        (clip_kind = 'ASSET' AND asset_id IS NOT NULL AND media_kind IS NOT NULL) OR
        (clip_kind IN ('TITLE','CAPTION') AND asset_id IS NULL AND media_kind IS NULL)
    )
);

CREATE TRIGGER clips_validate_insert
BEFORE INSERT ON clips
BEGIN
    SELECT CASE WHEN NEW.clip_kind = 'ASSET' AND
        (SELECT project_id FROM media_assets WHERE asset_id = NEW.asset_id) !=
        (SELECT timelines.project_id FROM tracks
         JOIN timelines ON timelines.timeline_id = tracks.timeline_id
         WHERE tracks.track_id = NEW.track_id)
        THEN RAISE(ABORT, 'clip asset must belong to the timeline project') END;
    SELECT CASE WHEN NEW.clip_kind = 'ASSET' AND
        NEW.media_kind != (SELECT kind FROM media_assets WHERE asset_id = NEW.asset_id)
        THEN RAISE(ABORT, 'clip media kind must match asset') END;
    SELECT CASE WHEN NEW.clip_kind = 'ASSET' AND
        NEW.source_start_ns >
        (SELECT duration_ns FROM media_assets WHERE asset_id = NEW.asset_id) -
            NEW.source_duration_ns
        THEN RAISE(ABORT, 'clip source range must stay inside asset') END;
    SELECT CASE WHEN
        (NEW.clip_kind = 'ASSET' AND NEW.media_kind IN ('VIDEO','IMAGE') AND
         (SELECT kind FROM tracks WHERE track_id = NEW.track_id) != 'VIDEO') OR
        (NEW.clip_kind = 'ASSET' AND NEW.media_kind = 'AUDIO' AND
         (SELECT kind FROM tracks WHERE track_id = NEW.track_id) != 'AUDIO') OR
        (NEW.clip_kind = 'TITLE' AND
         (SELECT kind FROM tracks WHERE track_id = NEW.track_id) != 'TITLE') OR
        (NEW.clip_kind = 'CAPTION' AND
         (SELECT kind FROM tracks WHERE track_id = NEW.track_id) != 'CAPTION')
        THEN RAISE(ABORT, 'clip kind must match track') END;
END;

CREATE TRIGGER clips_validate_update
BEFORE UPDATE ON clips
BEGIN
    SELECT CASE WHEN NEW.clip_kind = 'ASSET' AND
        (SELECT project_id FROM media_assets WHERE asset_id = NEW.asset_id) !=
        (SELECT timelines.project_id FROM tracks
         JOIN timelines ON timelines.timeline_id = tracks.timeline_id
         WHERE tracks.track_id = NEW.track_id)
        THEN RAISE(ABORT, 'clip asset must belong to the timeline project') END;
    SELECT CASE WHEN NEW.clip_kind = 'ASSET' AND
        NEW.media_kind != (SELECT kind FROM media_assets WHERE asset_id = NEW.asset_id)
        THEN RAISE(ABORT, 'clip media kind must match asset') END;
    SELECT CASE WHEN NEW.clip_kind = 'ASSET' AND
        NEW.source_start_ns >
        (SELECT duration_ns FROM media_assets WHERE asset_id = NEW.asset_id) -
            NEW.source_duration_ns
        THEN RAISE(ABORT, 'clip source range must stay inside asset') END;
    SELECT CASE WHEN
        (NEW.clip_kind = 'ASSET' AND NEW.media_kind IN ('VIDEO','IMAGE') AND
         (SELECT kind FROM tracks WHERE track_id = NEW.track_id) != 'VIDEO') OR
        (NEW.clip_kind = 'ASSET' AND NEW.media_kind = 'AUDIO' AND
         (SELECT kind FROM tracks WHERE track_id = NEW.track_id) != 'AUDIO') OR
        (NEW.clip_kind = 'TITLE' AND
         (SELECT kind FROM tracks WHERE track_id = NEW.track_id) != 'TITLE') OR
        (NEW.clip_kind = 'CAPTION' AND
         (SELECT kind FROM tracks WHERE track_id = NEW.track_id) != 'CAPTION')
        THEN RAISE(ABORT, 'clip kind must match track') END;
END;

CREATE INDEX clips_track_start ON clips(track_id, timeline_start_ns, clip_id);

CREATE TABLE clip_visual_transforms(
    clip_id TEXT PRIMARY KEY REFERENCES clips(clip_id) ON DELETE CASCADE,
    x REAL NOT NULL CHECK(x >= 0.0 AND x <= 1.0),
    y REAL NOT NULL CHECK(y >= 0.0 AND y <= 1.0),
    width REAL NOT NULL CHECK(width > 0.0 AND width <= 1.0),
    height REAL NOT NULL CHECK(height > 0.0 AND height <= 1.0),
    scale_x REAL NOT NULL CHECK(scale_x > 0.0),
    scale_y REAL NOT NULL CHECK(scale_y > 0.0),
    rotation_degrees REAL NOT NULL,
    crop_left REAL NOT NULL CHECK(crop_left >= 0.0 AND crop_left <= 1.0),
    crop_top REAL NOT NULL CHECK(crop_top >= 0.0 AND crop_top <= 1.0),
    crop_right REAL NOT NULL CHECK(crop_right >= 0.0 AND crop_right <= 1.0),
    crop_bottom REAL NOT NULL CHECK(crop_bottom >= 0.0 AND crop_bottom <= 1.0),
    opacity REAL NOT NULL CHECK(opacity >= 0.0 AND opacity <= 1.0),
    z_order INTEGER NOT NULL,
    CHECK(crop_left + crop_right < 1.0),
    CHECK(crop_top + crop_bottom < 1.0)
);

CREATE TABLE clip_audio_envelopes(
    clip_id TEXT PRIMARY KEY REFERENCES clips(clip_id) ON DELETE CASCADE,
    gain_db REAL NOT NULL CHECK(gain_db >= -96.0 AND gain_db <= 24.0),
    fade_in_ns INTEGER NOT NULL CHECK(fade_in_ns >= 0),
    fade_out_ns INTEGER NOT NULL CHECK(fade_out_ns >= 0),
    clip_duration_ns INTEGER NOT NULL CHECK(clip_duration_ns > 0),
    CHECK(fade_in_ns <= clip_duration_ns),
    CHECK(fade_out_ns <= clip_duration_ns - fade_in_ns)
);

CREATE TRIGGER clip_audio_envelopes_duration_insert
BEFORE INSERT ON clip_audio_envelopes
BEGIN
    SELECT CASE WHEN NEW.clip_duration_ns !=
        (SELECT timeline_duration_ns FROM clips WHERE clip_id = NEW.clip_id)
        THEN RAISE(ABORT, 'audio envelope duration must match clip') END;
END;

CREATE TRIGGER clip_audio_envelopes_duration_update
BEFORE UPDATE ON clip_audio_envelopes
BEGIN
    SELECT CASE WHEN NEW.clip_duration_ns !=
        (SELECT timeline_duration_ns FROM clips WHERE clip_id = NEW.clip_id)
        THEN RAISE(ABORT, 'audio envelope duration must match clip') END;
END;

CREATE TABLE titles(
    clip_id TEXT PRIMARY KEY REFERENCES clips(clip_id) ON DELETE CASCADE,
    text TEXT NOT NULL CHECK(length(text) > 0),
    font_family TEXT NOT NULL CHECK(length(font_family) > 0),
    x REAL NOT NULL CHECK(x >= 0.0 AND x <= 1.0),
    y REAL NOT NULL CHECK(y >= 0.0 AND y <= 1.0),
    foreground_rgba TEXT NOT NULL CHECK(length(foreground_rgba) > 0),
    background_rgba TEXT NOT NULL CHECK(length(background_rgba) > 0),
    alignment TEXT NOT NULL CHECK(alignment IN ('LEFT','CENTER','RIGHT'))
);

CREATE TABLE caption_cues(
    cue_id TEXT PRIMARY KEY,
    clip_id TEXT NOT NULL REFERENCES clips(clip_id) ON DELETE CASCADE,
    start_offset_ns INTEGER NOT NULL CHECK(start_offset_ns >= 0),
    duration_ns INTEGER NOT NULL CHECK(duration_ns > 0),
    text TEXT NOT NULL CHECK(length(text) > 0),
    UNIQUE(clip_id, start_offset_ns, cue_id)
);

CREATE TABLE markers(
    marker_id TEXT PRIMARY KEY,
    timeline_id TEXT NOT NULL REFERENCES timelines(timeline_id) ON DELETE CASCADE,
    position_ns INTEGER NOT NULL CHECK(position_ns >= 0),
    label TEXT NOT NULL DEFAULT '',
    UNIQUE(timeline_id, position_ns, marker_id)
);

CREATE TABLE edit_commands(
    event_id TEXT PRIMARY KEY,
    timeline_id TEXT NOT NULL REFERENCES timelines(timeline_id) ON DELETE CASCADE,
    sequence INTEGER NOT NULL CHECK(sequence >= 0),
    command_id TEXT NOT NULL CHECK(length(command_id) > 0),
    event_kind TEXT NOT NULL CHECK(event_kind IN ('APPLY','UNDO','REDO')),
    command_type TEXT NOT NULL CHECK(length(command_type) > 0),
    payload_json TEXT NOT NULL CHECK(length(payload_json) > 0),
    undo_payload_json TEXT NOT NULL CHECK(length(undo_payload_json) > 0),
    created_at_utc TEXT NOT NULL CHECK(length(created_at_utc) > 0),
    UNIQUE(timeline_id, sequence)
);

CREATE INDEX edit_commands_timeline_command
    ON edit_commands(timeline_id, command_id, sequence);

CREATE TABLE edit_checkpoints(
    timeline_id TEXT PRIMARY KEY REFERENCES timelines(timeline_id) ON DELETE CASCADE,
    revision INTEGER NOT NULL CHECK(revision >= 0),
    history_count INTEGER NOT NULL CHECK(history_count >= 0),
    history_cursor INTEGER NOT NULL CHECK(history_cursor >= 0 AND history_cursor <= history_count),
    clean_cursor INTEGER CHECK(clean_cursor IS NULL OR
                               (clean_cursor >= 0 AND clean_cursor <= history_count)),
    explicit_saved_revision INTEGER NOT NULL CHECK(explicit_saved_revision >= 0 AND
                                                    explicit_saved_revision <= revision)
);

CREATE INDEX media_assets_project_kind ON media_assets(project_id, kind, asset_id);
CREATE INDEX tracks_timeline_position ON tracks(timeline_id, position);
CREATE INDEX markers_timeline_position ON markers(timeline_id, position_ns);
