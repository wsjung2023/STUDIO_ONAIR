# ADR-0003: FFmpeg and MLT Boundaries

## Status
Accepted

## Decision
Use FFmpeg for capture encoding, media I/O, remuxing, and conversion. Use MLT for edit preview and final timeline rendering. Keep both behind adapters.

## Rationale
Using MLT as the product data model would couple project migrations and UX to an external engine. Using FFmpeg alone for a full NLE would require reimplementing too much timeline behavior.

## Consequences
- Domain timeline conversion code is required.
- MLT plugin loading must be controlled.
- FFmpeg build configuration is a release artifact.
