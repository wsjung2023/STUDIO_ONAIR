# R2-07 Product Integration and Physical Closure Plan

## Goal

Close R2 on the already verified R1 product base. The enabled Windows product
must use the audited RNNoise and whisper.cpp runtimes, keep generated edits as
proposals until explicit approval, allow cancellation without publishing a
partial result, and preserve the original project on every analysis failure.

## Existing evidence and gaps

- R2-01 through R2-06 provide tested cursor, emphasis, auto-zoom,
  transcription, cut-suggestion, audio-cleanup, and loudness primitives.
- The audited RNNoise 0.1.1 and whisper.cpp 1.7.6/tiny.en bootstraps now build
  and verify on this Windows machine.
- The product composition root did not create a transcription provider, export
  did not receive a fresh audio-cleanup chain, and export loudness analysis was
  not wired into the render path.
- The prior R2 acceptance suite uses fakes and does not prove approval,
  cancellation, product composition, or physical output loudness.

## Task 1: Audited dependency gate

1. Keep RNNoise's official archive hash authoritative.
2. Apply the pinned v0.1.1 MSVC VLA-bounds patch only after archive validation,
   fail closed unless every expected declaration matches exactly once, and
   record the patch identity in build evidence.
3. Build and verify RNNoise, whisper.cpp, ggml, and the pinned model.
4. Configure and build one FFmpeg + MLT + RNNoise + Whisper product tree.

## Task 2: Export cleanup and streaming two-pass loudness

1. Add failing tests proving each preview/export graph obtains an independent
   audio processor and export output reaches the configured LUFS/true-peak
   target.
2. Replace the one shared stateful processor with a factory that creates a fresh
   RNNoise -> compressor -> limiter chain for every MLT graph.
3. Perform pass-one measurement as bounded streaming 48 kHz stereo blocks; do
   not retain 30 minutes of PCM in memory.
4. Build pass two with a fresh cleanup chain followed by the decided static gain
   and a final true-peak limiter. Keep progress monotonic and cancellation live
   during both passes.
5. Preserve the existing partial-file, durable render-job, and atomic
   publication boundary.

## Task 3: Local-AI proposal controller

1. Add failing controller tests for real-provider composition, pending proposal
   state, explicit approval, cancellation, stale-revision rejection, and injected
   provider/store failures.
2. Decode a bounded user-selected/project range off the UI thread and invoke the
   configured transcription provider locally.
3. Derive cut suggestions and expose transcript/cut/zoom/emphasis proposals to
   QML without changing the timeline.
4. Approval applies one revision-checked editor transaction or atomically writes
   a derived artifact; rejection/cancellation discards the proposal.
5. Any generation, decoder, provider, serializer, or persistence error leaves
   the original project revision and files unchanged except discardable partials.

## Task 4: Enabled physical R2-07 acceptance

1. Run real whisper.cpp inference using the pinned model and validate timestamps
   and transcript schema.
2. Exercise real Windows Raw Input registration/unregistration and the cursor ->
   auto-zoom -> emphasis product plan without synthesizing user input.
3. Reopen the retained 30-minute R1 package, run bounded local analysis, reject
   one proposal, approve one proposal, save/reopen, cancel one normalized export,
   then retry successfully.
4. Probe H.264/AAC, duration, A/V sync, integrated LUFS, true peak, render-job
   terminal states, partial-file absence, handles, and memory bounds.
5. Run focused enabled tests, complete sequential CTest, source/audit checks,
   `git diff --check`, update the roadmap/report, and commit only after all gates
   pass.

## Completion rule

R2-07 is complete only when the enabled product path and the retained 30-minute
physical package pass. A fake-only suite, successful compilation, or a generated
model/runtime manifest by itself is not completion evidence.
