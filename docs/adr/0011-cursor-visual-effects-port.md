# ADR-0011: Cursor Visual Effects Render Port

## Status

Accepted as a pure, Qt-free frame effect in `cs_mlt_graph_plan`.

## Decision

`applyCursorVisualEffects` consumes an editable R2-02/R2-03 plan and one BGRA
frame timestamp. An active `ZoomCandidate` is converted to the existing typed
`VisualTransform`; active `ClickEmphasis` directives are composited as
button-colored ripple or highlight rings with deterministic source-over alpha.
No effect mutates the input buffer unless the returned frame owns a copy.

Cursor hiding/replacement is not silently approximated. It requires either a
clean plate or an explicit replacement asset and remains a separate editor/MLT
integration port.

