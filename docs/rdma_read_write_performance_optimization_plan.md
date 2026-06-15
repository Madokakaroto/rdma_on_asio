# RDMA Read/Write Performance Optimization Plan

This plan tracks the performance work derived from the 2026-06-15 read/write
benchmark and ETW flame graph results.

The current conclusion is:

- `read/write event/callback` measures RDMA-on-Asio wrapper cost plus Asio/IOCP
  scheduler cost.
- `read/write poll/use_future` is not a clean wrapper-cost measurement because
  `use_future` adds promise/future allocation, heap traffic, and locking.
- `send/recv poll/callback` already shows that RDMA-on-Asio can approach native
  ND poll for the large-message data path. The missing fair comparison is
  `read/write poll/callback`.

## Goals

- Separate Asio token/scheduler overhead from RDMA-on-Asio wrapper overhead.
- Reduce per-operation overhead on small `read`/`write` messages.
- Keep ordinary `async_read` / `async_write` Asio semantics intact.
- Add lower-level performance-oriented APIs only after measurement proves they
  are needed.
- Apply shared changes to both ND and IBV where the abstraction is common.

## Non-Goals

- Do not optimize by removing completion semantics from existing `async_*`
  operations. Asio async operations must complete so users can safely manage
  buffer lifetime, error handling, cancellation, and flow control.
- Do not treat `use_future` throughput as the library's best possible data-path
  result. It is a valid API mode, but not the right performance baseline.
- Do not hide correctness validation cost inside throughput results. Validation
  should be explicit and configurable.

## Stage 0: Measurement Hygiene

Add fair benchmark cases before changing library internals.

Tasks:

- Add `rdma_read_write_bench --mode poll --token-type callback`.
- Keep existing `poll/use_future` as a token-overhead benchmark.
- Add `--validate full|sample|none` for read/write benchmarks.
  - `full`: current behavior, every completed read is checked.
  - `sample`: check first/last/N periodic slots.
  - `none`: pure throughput path.
- Run the same matrix for:
  - RDMA-on-Asio event/callback.
  - RDMA-on-Asio poll/callback.
  - RDMA-on-Asio poll/use_future.
  - Native ND poll.
- Generate ETW flame graphs for `read64` poll/callback and compare against:
  - event/callback flame graph.
  - poll/use_future flame graph.

Acceptance criteria:

- Results document can show which cost belongs to scheduler, token, wrapper, and
  benchmark validation.
- `poll/callback` read/write becomes the main wrapper-overhead baseline.

## Stage 1: SGE Construction Fast Path

The current SGE path is generic and correct, but it still does work on every
small operation:

- `all_empty()` scans the buffer sequence.
- `buffers2sglist()` calls `std::distance()`.
- `nd_sglist_t` / `ibv_sglist_t` are resized every post.
- Single-buffer operations still go through the generic sequence path.

Tasks:

- Add a single-buffer fast path for `rdma::mutable_buffer` and
  `rdma::const_buffer`.
  - Build one native SGE on the stack.
  - Skip `nd_sglist_t` / `ibv_sglist_t`.
  - Skip `std::distance()`.
  - Skip the second full traversal.
- Add a fixed-size small-sequence fast path for `std::array` or contiguous
  buffer containers when size is cheaply known.
- Refactor `buffers2sglist()` into a builder that returns:
  - `data`.
  - `count`.
  - `empty`.
  - `too_many_sge`.
- Change `nd_sglist_t` and `ibv_sglist_t` to keep capacity for heap spill
  instead of deleting/reallocating on every `resize(count > inline_sge_count)`.
- Avoid double scanning:
  - combine empty detection and SGE fill;
  - for one-sided write, reuse the filled byte count instead of calling
    `buffer_size()` again where possible.

Acceptance criteria:

- `read64` and `write64` poll/callback flame graphs show reduced
  `buffers2sglist` / `nd_sglist_t::resize` / allocation cost.
- Existing SGL correctness tests still pass for single-buffer, multi-buffer,
  empty-buffer, and too-many-SGE cases.
- ND and IBV have the same public behavior.

## Stage 2: Operation Allocation And Handler Cost

Every `async_read` / `async_write` currently allocates an operation object and
stores handler/work state. This is correct Asio design, but it is visible in
small-message throughput.

Tasks:

- Benchmark callback tokens with a custom associated allocator.
- Add benchmark support for an operation recycling allocator.
- Document recommended performance tokens:
  - `callback` for throughput.
  - `use_future` for convenience, not hot paths.
  - `use_awaitable` only after separate coroutine profiling.
- Review whether internal operation recycling can be added without breaking
  Asio associated allocator semantics.
- Avoid extra state in read/write op objects when the completion path can derive
  bytes cheaply.

Acceptance criteria:

- We know how much of the hot path is default heap allocation vs provider cost.
- Any allocator optimization keeps Asio handler allocator customization working.

## Stage 3: Completion-Lite / Native-Like Post API

This is the "fire-and-forget" discussion.

Do not add a fire-and-forget overload to `async_read` / `async_write` directly.
Those names imply Asio async completion semantics. A true fire-and-forget read
or write is unsafe unless the user has another way to know when the buffer and
remote operation are complete.

Instead, consider a separate low-level API if Stage 0-2 still show meaningful
wrapper overhead:

```cpp
asio::error_code ec;
qp.post_read(buffer, remote, user_context, ec);
qp.post_write(buffer, remote, user_context, ec);

completion_queue.poll_one(completion, ec);
```

Design notes:

- The operation is submitted synchronously.
- The user owns the context and lifetime.
- Completion is observed by polling the CQ, not by invoking an Asio handler.
- This API is for benchmarks and expert users; it is not a replacement for
  `async_read` / `async_write`.
- It should be named `post_*`, `try_post_*`, or `submit_*`, not `async_*`.
- The completion record should include operation type, status, byte count, and
  user context.

Risks:

- Buffer lifetime becomes user-managed.
- Cancellation semantics are weaker or unavailable.
- Error propagation is no longer handler-based.
- It may duplicate native ND/IBV concepts inside the portable API.

Acceptance criteria:

- Only implement this if `poll/callback` still has large wrapper overhead after
  SGE and allocation improvements.
- Unit tests must cover lifetime-independent completion records and post errors.

## Stage 4: Event Scheduler Tuning

Event/callback mode will always include IOCP / `io_context` dispatch cost, but
we can still reduce avoidable overhead.

Tasks:

- Add the event CQ drain batch size to the device-level configuration:
  - target field: `device_config_t::event_cq_drain_batch_size`;
  - current code maps this concept through `rdma_config_t`, with backend aliases
    `nd_config_t` / `ibv_config_t`;
  - `0` keeps the current auto/default behavior, matching the existing config
    convention for capability-derived values;
  - explicit values such as `16`, `32`, and `64` override the backend default.
- Wire the configured batch size through `use_device(io, dev, config)`,
  service/device state, and event-mode CQ poller construction.
- Keep standalone poll-mode CQs independent from this event scheduler knob; poll
  mode should continue to drain according to the caller's explicit `poll()`
  loop.
- Measure `event_cq_drain_batch_size=16`, `32`, `64` for read64/write64.
- Ensure event poller drains multiple CQEs per notification before re-arming.
- Check whether `arm_poller` / `DeviceIoControl` frequency can be reduced.
- Keep event-mode correctness and cancellation behavior unchanged.

Acceptance criteria:

- The batch setting is visible in the effective device config and reported by
  benchmarks/results when non-default.
- Both ND and IBV either honor the field or document a backend-specific no-op
  with the same public config surface.
- Event/callback improves without regressing cancellation tests.
- Poll mode remains the pure throughput baseline.

## Stage 5: RDMA Read Queue Depth And Provider Limits

The previous QD sweep showed `ND_NO_MORE_ENTRIES` at high outstanding read
depth. This must be handled explicitly.

Tasks:

- Query or derive max outbound RDMA read capacity from ND/IBV device/QP config.
- Cap benchmark queue depth to the supported read depth or report a structured
  skip/error.
- Add result fields for requested queue depth vs effective queue depth.
- Add tests for "requested QD exceeds device capability" behavior where it can
  be simulated.

Acceptance criteria:

- Benchmark failures from unsupported read depth are classified instead of
  looking like random post failures.

## Stage 6: Benchmark Validation Cost

The flame graph shows `verify_pattern` / `pattern_byte` is visible for small
reads. This is benchmark cost, not library cost.

Tasks:

- Implement `--validate full|sample|none`.
- Default smoke/regression scenarios to `full`.
- Default throughput profiling scenarios to `sample` or `none`, with a clear
  result field.
- Keep a separate correctness scenario that validates every byte.

Acceptance criteria:

- Results tables explicitly say how validation was configured.
- Throughput numbers no longer hide full validation overhead unless requested.

## Stage 7: Documentation And Test Updates

Tasks:

- Update `rdma_stress_performance_results.md` with:
  - poll/callback read/write results;
  - validation mode;
  - flame graph links;
  - optimization-before/after tables.
- Add focused unit tests for:
  - single-buffer SGE fast path;
  - multi-buffer SGE path;
  - too-many-SGE rejection;
  - empty buffer immediate completion behavior;
  - completion-lite post API if implemented.
- Keep performance and stress tests opt-in, not default CTest.

## Priority Order

1. Add read/write `poll/callback` benchmark and validation mode.
2. Generate read64/write64 `poll/callback` flame graphs.
3. Implement single-buffer SGE fast path.
4. Re-run Release and RelWithDebInfo comparisons.
5. Add allocator/recycling benchmark support.
6. Decide whether a low-level `post_read` / `post_write` API is justified.
7. Tune event scheduler batch/arming behavior.

## Current Working Hypotheses

- `use_future` is the largest avoidable cost in current read/write poll results.
- Full read validation is large enough to distort small-message profiling.
- Single-buffer SGE construction is worth optimizing, but it is probably not the
  largest remaining cost after `use_future` is removed.
- Provider-side `mlx5nd.dll` and critical-section cost will remain visible even
  in native-like paths.
- Large RDMA read bandwidth appears provider/platform limited on this single
  host, because RDMA-on-Asio and native ND both plateau around the same range.
