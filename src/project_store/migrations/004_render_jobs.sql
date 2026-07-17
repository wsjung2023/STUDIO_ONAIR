CREATE TABLE render_jobs(
    job_id TEXT PRIMARY KEY NOT NULL CHECK(length(job_id) > 0),
    project_id TEXT NOT NULL REFERENCES projects(project_id) ON DELETE CASCADE,
    timeline_id TEXT NOT NULL REFERENCES timelines(timeline_id) ON DELETE CASCADE,
    timeline_revision INTEGER NOT NULL CHECK(timeline_revision >= 0),
    preset_id TEXT NOT NULL CHECK(length(preset_id) BETWEEN 1 AND 64),
    width INTEGER NOT NULL CHECK(width BETWEEN 1 AND 16384),
    height INTEGER NOT NULL CHECK(height BETWEEN 1 AND 16384),
    frame_rate_numerator INTEGER NOT NULL CHECK(frame_rate_numerator > 0),
    frame_rate_denominator INTEGER NOT NULL CHECK(frame_rate_denominator > 0),
    video_bitrate INTEGER NOT NULL CHECK(video_bitrate > 0),
    audio_bitrate INTEGER NOT NULL CHECK(audio_bitrate > 0),
    fallback_policy TEXT NOT NULL
        CHECK(fallback_policy IN ('HARDWARE_THEN_SOFTWARE','SOFTWARE_ONLY')),
    overwrite_policy TEXT NOT NULL
        CHECK(overwrite_policy IN ('FAIL_IF_EXISTS','REPLACE_EXISTING')),
    destination_path TEXT NOT NULL CHECK(length(destination_path) > 0),
    partial_path TEXT NOT NULL CHECK(length(partial_path) > 0),
    state TEXT NOT NULL CHECK(state IN (
        'PENDING','RUNNING','PUBLISHING','CANCELLING',
        'COMPLETED','FAILED','CANCELLED')),
    rendered_through_ns INTEGER NOT NULL CHECK(rendered_through_ns >= 0),
    total_duration_ns INTEGER NOT NULL CHECK(total_duration_ns > 0),
    fraction REAL NOT NULL CHECK(fraction >= 0.0 AND fraction <= 1.0),
    attempted_encoder TEXT,
    selected_encoder TEXT,
    fallback_reason TEXT,
    diagnostic TEXT,
    output_sha256 TEXT CHECK(
        output_sha256 IS NULL OR length(output_sha256) = 64),
    destination_identity TEXT,
    created_at_utc TEXT NOT NULL CHECK(length(created_at_utc) > 0),
    started_at_utc TEXT,
    updated_at_utc TEXT NOT NULL CHECK(length(updated_at_utc) > 0),
    finished_at_utc TEXT,
    CHECK(
        (state = 'PENDING' AND fraction = 0.0 AND rendered_through_ns = 0 AND
         started_at_utc IS NULL AND finished_at_utc IS NULL AND
         output_sha256 IS NULL AND destination_identity IS NULL) OR
        (state = 'RUNNING' AND fraction < 1.0 AND
         started_at_utc IS NOT NULL AND finished_at_utc IS NULL AND
         output_sha256 IS NULL AND destination_identity IS NULL) OR
        (state = 'PUBLISHING' AND fraction < 1.0 AND
         rendered_through_ns = total_duration_ns AND
         started_at_utc IS NOT NULL AND finished_at_utc IS NULL AND
         output_sha256 IS NOT NULL AND destination_identity IS NOT NULL) OR
        (state = 'CANCELLING' AND fraction < 1.0 AND
         finished_at_utc IS NULL) OR
        (state = 'COMPLETED' AND fraction = 1.0 AND
         rendered_through_ns = total_duration_ns AND
         output_sha256 IS NOT NULL AND destination_identity IS NOT NULL AND
         finished_at_utc IS NOT NULL) OR
        (state = 'FAILED' AND fraction < 1.0 AND
         diagnostic IS NOT NULL AND finished_at_utc IS NOT NULL) OR
        (state = 'CANCELLED' AND fraction < 1.0 AND
         finished_at_utc IS NOT NULL)
    )
);

CREATE TRIGGER render_jobs_same_project
BEFORE INSERT ON render_jobs
WHEN (SELECT project_id FROM timelines WHERE timeline_id = NEW.timeline_id) !=
     NEW.project_id
BEGIN
    SELECT RAISE(ABORT, 'render timeline must belong to the render project');
END;

CREATE TRIGGER render_jobs_identity_immutable
BEFORE UPDATE ON render_jobs
WHEN NEW.job_id != OLD.job_id OR NEW.project_id != OLD.project_id OR
     NEW.timeline_id != OLD.timeline_id OR
     NEW.timeline_revision != OLD.timeline_revision OR
     NEW.preset_id != OLD.preset_id OR NEW.width != OLD.width OR
     NEW.height != OLD.height OR
     NEW.frame_rate_numerator != OLD.frame_rate_numerator OR
     NEW.frame_rate_denominator != OLD.frame_rate_denominator OR
     NEW.video_bitrate != OLD.video_bitrate OR
     NEW.audio_bitrate != OLD.audio_bitrate OR
     NEW.fallback_policy != OLD.fallback_policy OR
     NEW.overwrite_policy != OLD.overwrite_policy OR
     NEW.destination_path != OLD.destination_path OR
     NEW.partial_path != OLD.partial_path OR
     NEW.total_duration_ns != OLD.total_duration_ns OR
     NEW.created_at_utc != OLD.created_at_utc
BEGIN
    SELECT RAISE(ABORT, 'render job identity is immutable');
END;

CREATE TRIGGER render_jobs_progress_monotonic
BEFORE UPDATE ON render_jobs
WHEN NEW.rendered_through_ns < OLD.rendered_through_ns OR
     NEW.fraction < OLD.fraction OR NEW.updated_at_utc < OLD.updated_at_utc
BEGIN
    SELECT RAISE(ABORT, 'render job progress must be monotonic');
END;

CREATE TRIGGER render_jobs_state_transition
BEFORE UPDATE OF state ON render_jobs
WHEN NOT (
    (OLD.state = NEW.state AND OLD.state NOT IN
        ('COMPLETED','FAILED','CANCELLED')) OR
    (OLD.state = 'PENDING' AND NEW.state IN
        ('RUNNING','CANCELLING','FAILED')) OR
    (OLD.state = 'RUNNING' AND NEW.state IN
        ('PUBLISHING','CANCELLING','FAILED')) OR
    (OLD.state = 'PUBLISHING' AND NEW.state IN
        ('COMPLETED','CANCELLED','FAILED')) OR
    (OLD.state = 'CANCELLING' AND NEW.state IN
        ('CANCELLED','COMPLETED','FAILED'))
)
BEGIN
    SELECT RAISE(ABORT, 'render job state transition is invalid');
END;

CREATE INDEX render_jobs_project_state
    ON render_jobs(project_id, state, created_at_utc, job_id);
