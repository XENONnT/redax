# Chunk overwrite / mistiming fix (relative to `new_libs`)

## Problem summary

With per-formatter buffering/queues enabled, datapackets can reach a formatter out of order
(especially with round-robin dispatch). This can cause a formatter to:

- flush and finalize chunk `N` to disk (and create placeholder empty files up to some point), then
- later receive “late” fragments whose timestamps place them in chunk `N`,
- attempt to write chunk `N` again, producing warnings like:
  `Chunk 00000N from thread ... already exists? ...`

This is fatal for data integrity in live mode: rewriting/overwriting chunk files after downstream
has potentially started reading the chunk leads to malformed chunks and broken time ordering.

The solution is to avoid finallizing a chunk until all data is present via the mechanism of a watermark

## Changes made

### `StraxFormatter.cc` / `StraxFormatter.hh` — conservative chunk finalization (watermark)

- Track per-board progress inside each formatter:
  - `max_chunk_seen` per `bid`
  - `last_seen` timestamp per `bid`
- Replace the old “average-chunk” based flushing in `WriteOutChunks()` with a **watermark** rule:
  - compute `watermark = min(max_chunk_seen)` over non-idle boards
  - only flush chunks with `chunk_id < watermark - strax_buffer_num_chunks`
  - only then advance placeholder generation (`CreateEmpty`) up to the same boundary
- Add `strax_watermark_idle_ms`:
  - boards that have not produced data for this long are excluded from the watermark minimum,
    preventing a quiet board from stalling flushing forever
  - default is `2 * strax_chunk_length` (in ms), clamped to at least 1 s
- Extend `AddFragmentToBuffer(...)` to accept `bid` so the formatter can update per-board tracking.
- Add an ERROR logline if a fragment arrives with `chunk_id < fEmptyVerified`:
  - this indicates “late data for an already placeholder-verified/finalized chunk”, which directly
    explains the “already exists” overwrites and chunk corruption.

Why this is required:
- In live reading, chunk readiness is based on file presence/counts; rewriting a chunk later is
  unsafe because downstream may already have consumed it.
- The watermark guarantees we only finalize chunks that all active boards have progressed beyond,
  with a configurable safety margin (`strax_buffer_num_chunks`).

### `DAQController.cc` — stable board→formatter routing

- Replace round-robin assignment of readout batches to formatter threads with stable routing:
  - each datapacket goes to `target_formatter = bid % n_formatters`
  - the readout thread buffers datapackets per target formatter
  - on transfer, it enqueues at most one batch per formatter

Why this is required:
- Round-robin batching interleaves boards arbitrarily across formatters, amplifying out-of-order
  delivery and making late-fragment chunk rewrites much more likely.
- Stable routing bounds reordering within a formatter and makes the watermark approach effective
  with small margins, without sacrificing read speed (more RAM buffering is used instead).
