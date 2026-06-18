# SGE List Performance Refactor Plan

This plan isolates the scatter/gather list work from the broader
read/write-performance plan. The goal is to keep the current public buffer
sequence semantics while reducing hot-path overhead in the backend-specific
conversion from `asio::rdma::{const_buffer,mutable_buffer}` sequences to native
SGEs.

## Goals

- Use the current implementation and benchmark numbers as the baseline.
- Optimize SGE construction for the common small-buffer-count path.
- Keep small SGE lists entirely inline, with heap allocation only for unusually
  large buffer sequences.
- Remove the thread-specific scratch SGE list from the data path without
  reintroducing the old data-race/corruption problem.
- Compare optimized RDMA-on-Asio SGE construction against native SGE construction
  in direct ND benchmarks, then use profiling to drive further iterations.
- Apply the design symmetrically to ND and IBV where possible.

## Pre-Refactor Implementation Baseline

The pre-refactor SGE path had three layers:

- Portable buffer elements live in `include/rdma/rdma_buffer.hpp`.
  `const_buffer` and `mutable_buffer` carry `{addr, length, lkey}` and can act as
  single-element buffer sequences through ADL `buffer_sequence_begin/end`.
- Backend converters live in:
  - `include/rdma/nd/nd_buffer.hpp`: `buffers2sglist()` fills `ND2_SGE`.
  - `include/rdma/ibv/ibv_buffer.hpp`: `buffers2sglist()` fills `ibv_sge`.
- Backend SGE containers live in:
  - `include/rdma/nd/detail/nd_impl_types.hpp`: `nd_sglist_t`.
  - `include/rdma/ibv/detail/ibv_impl_types.hpp`: `ibv_sglist_t`.

Both `nd_sglist_t` and `ibv_sglist_t` already had an inline capacity of 8 native
SGEs. However, every `resize()` called `reset()`, and a heap-spilled list was
deleted and reallocated on every post. The post path also calls `all_empty()`
before `buffers2sglist()`, and `buffers2sglist()` then calls `std::distance()`
before doing a second traversal to fill the native SGE array.

The service post paths used a thread-specific scratch list:

- `nd_verbs_service::get_sglist()` returns `static thread_local nd_sglist_t`.
- `ibv_verbs_service::get_sglist()` returns `static thread_local ibv_sglist_t`.

This is used by `do_post_send`, `do_post_recv`, `do_post_read`, and
`do_post_write` before calling the backend `verbs_ops::post_*` wrappers.

## Implementation Progress

Initial implementation has landed locally:

- Added `include/rdma/detail/small_sglist.hpp` with inline storage and heap spill
  support.
- Replaced backend-specific SGE list implementations with
  `small_sglist<native_sge_t, 8>` while preserving `nd_sglist_t` and
  `ibv_sglist_t` names.
- Added backend `build_native_sglist()` helpers that build SGE metadata in one
  pass and keep `buffers2sglist()` as a compatibility wrapper.
- Removed `get_sglist()` from ND and IBV verbs-service post paths.
- Added stack single-SGE fast paths for `const_buffer` / `mutable_buffer`.
- Added unit coverage for heap spill, heap-capacity reuse, all-empty metadata,
  and too-many-SGE detection.
- Verified on Windows/ND:
  - targeted Release build for SGE-related tests;
  - full `ALL_BUILD` Release;
  - default `ctest -C Release` (`16/16`);
  - RDMA-on-Asio ND hardware smoke for send/recv poll/callback and read/write
    poll/use_future single-buffer posts;
  - RDMA-on-Asio ND `test_rdma_sgl` hardware run covering multi-SGE
    gather/scatter and too-many-SGE recovery.

Remaining work:

- Compile and run the IBV side on Linux.
- Add focused microbenchmarks and before/after flame graphs.
- Keep the SGE post path no-TLS; any future optimization must preserve per-call
  descriptor ownership.

## IBV Remaining Work

The Windows/ND side has been built, unit-tested, and smoke-tested locally. The
IBV code has been updated in parallel, but still needs backend-specific
validation on Linux hardware before this refactor is considered complete for both
platforms.

IBV follow-up checklist:

- Build the IBV-enabled tree on Linux and run the default unit-test suite.
- Run `unit_ibv_buffer` and any IBV public-header/compile tests to verify the
  shared `small_sglist` aliases and backend field mapping compile cleanly with
  real `ibv_sge`.
- Add or run an IBV multi-SGE hardware smoke test covering send/recv, read, and
  write with SGE counts `1, 2, 4, 8, 9`.
- Confirm the IBV post paths for send, recv, read, and write no longer call
  `ibv_verbs_service::get_sglist()` and do not use TLS.
- Compare IBV RDMA-on-Asio one-SGE results against `perftest` for send, write,
  and read. `perftest` remains the native IBV baseline; this repo should not add
  a separate native IBV benchmark unless a later plan justifies it.
- Capture before/after flame graphs for small-message IBV read/write if SGE
  construction remains visible after the no-TLS and inline-storage changes.

IBV acceptance is intentionally stricter than "it compiles": the no-TLS design
must be validated under real post/completion traffic, because the whole point of
this refactor is to prove that per-call SGE descriptors are enough for both ND
and IBV.

## Thread-Local Review

The pre-refactor thread-local scratch list was the wrong design for SGE lists.
It made the data path depend on hidden per-thread mutable state even though native
SGE descriptors only need to live through the synchronous native post call.
Empirical testing on the IBV backend showed that
`ibv_verbs_service::get_sglist()` is unnecessary when descriptors are built as
per-call local state. Windows/ND hardware smoke testing also verified stack/local
SGE descriptors for the single-buffer send/recv/read/write paths.

The design rule is now strict: do not use TLS for SGE lists. TLS lookup is extra
work, complicates profiling, and hides state that should be local to the post
operation.

The real correctness requirement is not "use thread local"; it is "do not share
one mutable SGE array across concurrent posts." The data-plane initiation methods
can be called concurrently from multiple user threads, including on the same QP.
A process-global, service-member, or QP-owned scratch array would allow one
thread to overwrite descriptors while another thread is still building or
posting them.

The important lifetime rule is:

- User memory described by the SGE must remain valid until the CQ completion.
- The native SGE descriptor array itself only needs to remain valid for the
  duration of the native post call.

This is consistent with the direct ND benchmark, which posts stack-created
`ND2_SGE` descriptors in `tests/benchmark/native/nd/nd_perftest.cpp`. It also
matches the verbs style where the WR/SGE descriptors are consumed synchronously
by `ibv_post_*`.

Therefore, the optimized design should use a per-call stack/local object for the
normal path. That is different from the earlier unsafe "no thread local" design:
per-call local state is private to the initiating call, while a shared static or
QP-owned scratch state is not. If the earlier ND implementation failed without
TLS, the likely root cause was shared scratch reuse or descriptor lifetime
confusion, not an ND requirement for TLS. Current Windows/ND smoke tests confirm
the single-buffer no-TLS post path, and `test_rdma_sgl` confirms the multi-SGE
gather/scatter no-TLS path.

Recommended policy:

- Remove TLS from SGE list construction.
- IBV must not use TLS for SGE descriptors.
- ND must not use TLS for SGE descriptors; current Windows/ND hardware tests
  prove per-call descriptors are sufficient for single-buffer and multi-SGE
  paths.
- Use per-call stack/local storage for single-buffer and inline multi-buffer
  paths.
- Do not add locks around a shared scratch list; that would serialize the data
  path.
- Do not use TLS as a large-list heap-capacity cache. If large SGL allocation is
  hot, optimize the explicit local container or add an explicit reusable object
  owned by the caller, not hidden thread state.

## Target Design

Introduce a small-vector-like native SGE builder. It is closer to a small vector
than `std::inplace_vector`: it uses inline storage for the normal case and can
spill to heap when the buffer count exceeds the inline capacity.

The container can be backend-independent over the native SGE type:

```cpp
template <class NativeSge, std::size_t InlineCount = 8>
class small_sglist {
public:
  void clear() noexcept;
  bool reserve(std::size_t count, asio::error_code& ec);
  NativeSge* data() noexcept;
  std::size_t size() const noexcept;
  std::size_t capacity() const noexcept;
  bool uses_heap() const noexcept;
  NativeSge& append_uninitialized(asio::error_code& ec);
};
```

Backend-specific code should still own the native field mapping:

```cpp
inline void fill_native_sge(ND2_SGE& out, rdma::mutable_buffer const& b);
inline void fill_native_sge(ibv_sge& out, rdma::mutable_buffer const& b);
```

The builder should return all metadata the post path needs in one pass:

```cpp
template <class NativeSge>
struct built_sglist {
  NativeSge* data = nullptr;
  std::size_t count = 0;
  std::size_t total_bytes = 0;
  bool all_empty = true;
  bool too_many_sge = false;
  bool heap_spilled = false;
};
```

`built_sglist` is not another SGE container. It is a short-lived, non-owning
view plus summary of a build operation:

- `data/count` are passed directly to `verbs_ops::post_*`.
- `total_bytes` can be reused by send/write completion accounting so we do not
  rescan the buffer sequence with `buffer_size()`.
- `all_empty` preserves the current immediate-completion behavior.
- `too_many_sge` lets the builder stop early and return `rdma_errc::too_many_sge`
  before any native post.
- `heap_spilled` is instrumentation for validating that the small path stays
  allocation-free.

The storage behind `data` is owned by the caller's stack/local builder, not by
`built_sglist` itself. This keeps the descriptor lifetime explicit: the pointer
is valid until the native post call returns.

The build operation should:

- Traverse the buffer sequence once.
- Accumulate `total_bytes`.
- Preserve current semantics for mixed zero-length and non-zero buffers in the
  first implementation. That means an all-empty sequence still completes
  immediately, while mixed sequences keep their existing SGE mapping behavior.
- Check `max_sge` during construction and stop early with
  `rdma_errc::too_many_sge`.
- Avoid `std::distance()` in the hot path.

Example generic post path after the refactor:

```cpp
template <typename SendOpType>
static bool do_post_send(implementation_type& impl, SendOpType* op) {
  auto const& buffers = op->get_buffer_sequence();

  small_sglist<native_sge_t, 8> storage; // per-call local, no TLS
  auto built = build_native_sglist(buffers, storage,
                                   impl.config_.max_send_sge_);

  if (built.all_empty) {
    return true; // existing immediate-completion path
  }
  if (built.too_many_sge) {
    op->ec_ = make_error_code(rdma_errc::too_many_sge);
    return true;
  }

  verbs_ops::post_send(impl.qp_.Get(), op, built.data, built.count, 0, op->ec_);
  return static_cast<bool>(op->ec_);
}
```

Example single-buffer fast path:

```cpp
template <typename SendOpType>
static bool do_post_send_one(implementation_type& impl, SendOpType* op,
                             rdma::const_buffer const& buffer) {
  if (buffer.length() == 0) {
    return true;
  }
  if (impl.config_.max_send_sge_ != 0 && impl.config_.max_send_sge_ < 1) {
    op->ec_ = make_error_code(rdma_errc::too_many_sge);
    return true;
  }

  native_sge_t one{};
  fill_native_sge(one, buffer);
  verbs_ops::post_send(impl.qp_.Get(), op, &one, 1, 0, op->ec_);
  return static_cast<bool>(op->ec_);
}
```

For IBV the same shape applies, except `impl.qp_` is passed directly. For ND,
`impl.qp_.Get()` is used. The important property is the same in both backends:
no thread-local lookup is needed for the common one-SGE path.

### Single-Buffer Fast Path

`rdma::const_buffer` and `rdma::mutable_buffer` are first-class single-buffer
sequences. They should not go through the generic sequence builder.

For these types, the post path should:

- Check `length() == 0`.
- Check `max_sge >= 1`.
- Build one native SGE on the stack.
- Call `verbs_ops::post_*` with `&one_sge, 1`.
- Avoid `thread_local`, `small_sglist`, `std::distance()`, and `all_empty()`.

This path should become the reference hot path for `read64`, `write64`, and
single-buffer send/recv.

### Small Sequence Fast Path

For fixed-size or cheaply-sized sequences such as `std::array<rdma_buffer, N>`,
add a path that reserves inline capacity up front when `N <= InlineCount`.

For generic forward ranges, build incrementally:

- Start in the local inline array.
- Spill only if the count exceeds the inline capacity.
- If the count exceeds the configured device `max_sge`, fail before posting.

### Large Sequence Policy

Large SGLs are expected to be uncommon compared with 1-4 segment operations.
Initial implementation should prefer a simple per-call local `small_sglist`:

- No TLS access on any SGE-list path.
- Heap allocation only when count exceeds `InlineCount`.
- Heap memory released at the end of the call.

If benchmark data shows repeated large SGLs are important, add a second iteration
that still avoids TLS:

- Preserve per-call inline objects for `count <= InlineCount`.
- Improve the explicit local container's heap growth/capacity policy.
- Consider an explicit caller-owned reusable builder for expert paths if
  benchmarks prove per-call heap allocation dominates large SGL workloads.
- Add tests or instrumentation proving SGE list construction does not touch TLS.

## Implementation Stages

### Stage 0: Baseline And Instrumentation

- Capture current Release numbers for:
  - RDMA-on-Asio ND poll/callback send/recv, read, and write.
  - Native ND poll through `nd_perftest`.
  - IBV poll/callback against the existing perftest-constrained baseline where
    available.
- Add or run a focused SGE builder microbenchmark for 1, 2, 4, 8, 9, 16, 32, and
  64 segments.
- Record:
  - ns/op for SGE construction.
  - allocations/op.
  - bytes/op.
  - proof that TLS was not touched.
  - end-to-end Gbit/s and Mops/s for small-message hardware runs.
- Generate a flame graph for `read64` and `write64` with the current code.

### Stage 1: Common Small SGE Container

- Add a backend-neutral `small_sglist<NativeSge, InlineCount>` helper under
  `include/rdma/detail/`.
- Preserve current `nd_sglist_t` / `ibv_sglist_t` names initially as aliases or
  thin wrappers if that keeps the diff smaller.
- Change heap spill behavior to keep capacity when the same object is reused.
- Add unit tests for:
  - empty list.
  - inline boundary (`InlineCount`).
  - first heap spill (`InlineCount + 1`).
  - capacity reuse.
  - move-disabled/copy-disabled behavior if needed.

### Stage 2: One-Pass Builder

- Replace `all_empty() + std::distance() + fill` with a one-pass builder.
- Return `built_sglist` metadata.
- Convert `too_many_sge` inside the builder, not after a complete fill.
- Keep backend field mapping local to `nd_buffer.hpp` and `ibv_buffer.hpp`.
- Add ND/IBV unit tests for:
  - single element.
  - multi element.
  - all-empty.
  - mixed zero and non-zero lengths.
  - too many SGEs.
  - heap spill.

### Stage 3: Single-Buffer Stack Fast Path

- Specialize the post path for `rdma::const_buffer` and `rdma::mutable_buffer`.
- Build one native SGE as a local stack variable.
- Skip `get_sglist()` entirely for this path.
- Remove the `ibv_verbs_service::get_sglist()` dependency from the IBV
  single-buffer path; the measured expectation is that IBV does not need TLS.
- For ND, review `IND2QueuePair::{Send,Receive,Read,Write}` descriptor lifetime
  and validate stack `ND2_SGE` descriptors under stress. This is validation of
  the no-TLS design, not a reason to keep TLS.
- Verify `send`, `recv`, `read`, and `write` all preserve completion behavior.
- Add tests that prove the single-buffer conversion produces exactly one native
  SGE and reports the same errors as the generic path.

### Stage 4: Service Integration And Byte Accounting

- Update `do_post_send`, `do_post_recv`, `do_post_read`, and `do_post_write` in
  both backends.
- Use the builder's `total_bytes` where it can avoid a later `buffer_size()` scan
  without changing semantics.
- Be careful with receive/read byte counts:
  - recv/read should still report provider completion bytes.
  - send/write can report submitted bytes on successful completion.
- Keep immediate completion behavior for all-empty buffers unchanged.

### Stage 5: TLS Removal Validation

- Measure large SGL workloads after Stage 3/4.
- Keep SGE list construction no-TLS.
- If large SGL construction is still hot, optimize local container growth or add
  explicit caller-owned reuse; do not reintroduce hidden thread state.
- Remove the old thread-local scratch list completely.

Decision criteria:

- No SGE-list path may touch TLS.
- Large-list heap allocation rate should be visible in the microbenchmark before
  adding any explicit reuse API.
- The design must remain safe under concurrent posts from multiple threads.
- IBV must be no-TLS.
- ND must be no-TLS; API review and hardware tests validate that per-call local
  descriptors are consumed synchronously by the post call.

### Stage 6: Native Comparison And Iteration

- Compare RDMA-on-Asio against native ND direct SGE construction:
  - `nd_perftest` one-SGE stack descriptor.
  - optional native ND multi-SGE mode using a direct `std::array<ND2_SGE, N>` for
    small N and heap allocation for large N.
- Run the same message-size and queue-depth matrix as the current Windows
  README performance snapshot.
- Add an SGE-count dimension for microbenchmarks and optional hardware runs:
  `1, 2, 4, 8, 9, 16`.
- Generate before/after flame graphs.
- Iterate on the next visible hotspot:
  - operation allocation.
  - associated allocator behavior.
  - provider-side lock cost.
  - event scheduler re-arm/drain behavior.

## Benchmark Matrix

Minimum hardware matrix:

| Path | Backend | Mode | Token | Ops | SGE counts |
|---|---|---|---|---|---|
| RDMA-on-Asio | ND | poll | callback | send/recv, write, read | 1, 2, 4, 8, 9 |
| Native ND | ND | poll | n/a | send/recv, write, read | 1, optional 2/4/8/9 |
| RDMA-on-Asio | ND | event | callback | write, read | 1 |
| RDMA-on-Asio | IBV | poll | callback | send/recv, write, read | 1, 2, 4, 8, 9 |
| perftest | IBV | poll | n/a | send, write, read | 1-SGE equivalent |

Microbenchmark matrix:

| SGE count | Expected path |
|---:|---|
| 0 | immediate completion / no native post |
| 1 | stack single-SGE fast path |
| 2, 4, 8 | inline small-sglist path |
| 9 | first heap spill |
| 16, 32, 64 | large-list behavior |

Primary metrics:

- SGE construction ns/op.
- allocations/op.
- hardware throughput in Mops/s and Gbit/s.
- CPU cycles/op.
- flame-graph inclusive samples in SGE builder functions.

## Guardrails

- Do not change the public buffer API.
- Do not remove support for forward-iterator buffer sequences.
- Do not add locks to the hot data path.
- Do not use a shared static or QP-owned scratch SGE array without synchronization.
- Do not use TLS for SGE lists. The old TLS scratch list was a workaround for
  shared mutable state, not a valid long-term design.
- Do not conflate local SGE lifetime with registered memory lifetime. SGE
  descriptors are short-lived; the memory they describe is still user-owned and
  pinned until completion.
- Do not change all-empty immediate-completion semantics in this refactor.
- Do not add a native IBV baseline inside the repo; perftest remains the direct
  IBV baseline.

## Acceptance Criteria

- Existing unit tests and default `ctest` pass for both available backends.
- ND and IBV unit tests cover inline, heap-spill, all-empty, mixed-zero, and
  too-many-SGE cases.
- Single-buffer send/recv/read/write no longer call `std::distance()`,
  `all_empty()`, or `get_sglist()`.
- Small SGE sequences up to the inline capacity do not allocate.
- Large SGE sequences allocate only when they exceed inline capacity.
- The IBV post path no longer depends on `ibv_verbs_service::get_sglist()`.
- The ND post path no longer depends on `nd_verbs_service::get_sglist()`.
- No SGE-list code uses TLS.
- Before/after results show whether SGE construction remains a visible hotspot.
- Results are documented alongside the broader read/write optimization work.
