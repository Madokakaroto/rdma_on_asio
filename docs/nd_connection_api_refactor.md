# nd backend — connection API refactor (Windows handoff)

Bring the **nd (NetworkDirect)** connector/listener to parity with the new asio-aligned
connection API that has already been implemented and **verified on Linux/ibv**. After this,
the cross-platform `tcp::{connector,listener}` / `rdma_*` aliases and
`tests/rdma/test_rdma_echo.cpp` compile and run on Windows too.

**The ibv side is the reference implementation — diff against these files:**
- `include/ibv/ibv_connector.hpp`, `include/ibv/ibv_listener.hpp`
- `include/ibv/detail/ibv_connector_service.hpp`, `include/ibv/detail/ibv_listener_service.hpp`
- `include/ibv/detail/ibv_op_connect.hpp` (server-reply private-data capture on ESTABLISHED)
- `include/ibv/detail/ibv_impl_types.hpp` (`max_cm_private_data`, `ibv_pd_sink`)
- `tests/rdma/test_rdma_echo.cpp`, `tests/ibv/test_ibv_echo.cpp` (new flow)

## Target API (identical to ibv; both reach it via the `rdma_*` aliases / `tcp::`)

```cpp
// server
rdma_listener<tcp> lis(io); lis.open(tcp::v4()); lis.bind(ep); lis.listen();
auto [ec, conn] = co_await lis.async_get_connection(use_nothrow);   // -> rdma_connector<tcp>
auto cpd = conn.remote_private_data();                              // client's request private data
rdma_queue_pair qp(io);
co_await conn.async_accept(qp, reply_pd, use_nothrow);              // Accept(qp, …)

// client
rdma_connector<tcp> conn(io);
conn.open(tcp::v4());                                               // optional; async_connect auto-opens
co_await conn.async_connect(qp, endpoint, pd, use_nothrow);        // Connect(qp, …)
auto spd = conn.remote_private_data();                             // server's reply private data
```

## Key nd-vs-ibv differences to keep in mind

- **QP ownership**: nd's QP is created by `nd_queue_pair` from the adapter and **owned by the
  queue_pair** (it predates the connection). The connector only *borrows* `qp.native_handle()`
  (an `IND2QueuePair*`) for `Connect`/`Accept`. So there is **no create-qp callback** like ibv's
  `make_create_qp_fn` — the connector just receives the qp at `async_connect`/`async_accept` and
  passes `qp.native_handle()` to the ND2 call. (ibv had to create the QP on the cm_id; nd does not.)
- **No address/route resolution**: ND uses sockaddr directly (no `resolve_addr`/`resolve_route`).
  `async_connect` is `Bind(addr)` + `Connect(qp, addr, …)` + `CompleteConnect` (existing flow).
- **Private data**: read via `IND2Connector::GetPrivateData(&ptr,&len)`:
  - server: at `async_get_connection` (the listener op already creates the connector COM object;
    call `GetPrivateData` after the request arrives) → store the client's request data.
  - client: after `CompleteConnect` (ESTABLISHED) → store the server's reply data.
- **`open(ps)` ignores the port space value** for ND (no RDMA port space; the adapter is fixed by
  `use_device`). Accept the `PortSpace const&` only for API parity (you may use `ps.family()` if you
  ever select among v4/v6 adapter lists, but the io-completion-service already holds one adapter).

## File-by-file changes

### 1. `include/nd/detail/nd_impl_types.hpp`
- These already exist on ibv in `ibv_impl_types.hpp`; add the same to nd's detail (or a shared
  spot): `max_cm_private_data` is shared — it lives in `include/ibv/detail/ibv_impl_types.hpp`
  today; if nd can't include that, add an equivalent `inline constexpr std::size_t
  nd_max_private_data = 256;` near the nd types. (No `ibv_pd_sink` needed — nd has no create-qp
  callback.)

### 2. `include/nd/detail/nd_connector_service.hpp`
`implementation_type`: keep `connector_` (IND2Connector ComPtr), `connector_handle_` (overlapped
HANDLE), `adapter_`, `config_`. **Change**: `qp_` is now set at `async_connect`/`async_accept`
time (not at `open`). **Add**: `std::array<std::byte, max_private_data> remote_pd_{}` +
`std::size_t remote_pd_len_ = 0`.

Replace the two `open(...)` overloads (currently `open(impl, qp_handle, config, ec)` and
`open(impl, handle&&, qp_handle, config, ec)`) with:
- `void open(impl, /*PortSpace value or its rdma-type stand-in — ignored for nd*/, config, ec)`
  — create the IND2Connector from `use_service<nd_io_completion_service>().get_adapter()`, create
  the overlapped file, register the handle with the IOCP scheduler. **No qp.** (Mirror ibv
  `ibv_connector_service::open`, minus cm_id/event-channel specifics.)
- `void assign(impl, native_connector_type&& handle, std::span<const std::byte> remote_pd, config, ec)`
  — adopt `connector_` / `connector_handle_` / `adapter_` from the handle, register the overlapped
  handle with IOCP, and copy `remote_pd` into `impl.remote_pd_` (the client's request data).
- `std::span<const std::byte> remote_private_data(impl) const` — `{remote_pd_.data(), remote_pd_len_}`.

`async_connect(impl, native_qp_t* qp, endpoint, pd, handler, io_ex)`:
- auto-open if not open (create the IND2Connector as in `open`);
- `impl.qp_ = qp;` then `start_connect_op` (existing `Bind` + `Connect(qp, addr, ibr, obr, pd,
  pd_size, overlapped)`); on completion (in the connect op's `do_complete`/CompleteConnect path)
  call `impl.connector_->GetPrivateData(...)` and copy the server's reply into `impl.remote_pd_`.

`async_accept(impl, native_qp_t* qp, pd, handler, io_ex)`:
- `impl.qp_ = qp;` then `start_accept_op` (existing `Accept(qp, ibr, obr, pd, pd_size, overlapped)`).

Keep `async_disconnect`, `cancel`, `is_open`, the IOCP `nd_op_*` machinery unchanged.

### 3. `include/nd/detail/nd_op_connect.hpp`
In the connect op's completion (after `CompleteConnect` succeeds → connected), capture the
server's reply private data: `connector->GetPrivateData(&p,&len)` and copy into the connector
impl's `remote_pd_` (pass a pointer/sink to the op at construction, analogous to ibv's
`ibv_pd_sink` in `ibv_op_connect.hpp`). If simpler in nd, do the `GetPrivateData` copy in the
service right after the op reports success, before the user handler runs.

### 4. `include/nd/nd_connector.hpp`
Mirror `include/ibv/ibv_connector.hpp` exactly:
- empty ctor + opening ctor `nd_connector(io, PortSpace const& ps, config = {})`;
- `open(PortSpace const& ps, config = {})` / `open(ps, config, ec)` (calls
  `service.open(impl, /*ps or stand-in*/, config, ec)`);
- `assign(native_connector_type&&, config = {})` / `assign(…, ec)` →
  `service.assign(impl, std::move(handle), {}, config, ec)`;
- `async_connect(nd_queue_pair& qp, endpoint, pd, token)` → `service.async_connect(impl,
  qp.native_handle(), endpoint, pd, handler, io_ex)`;
- `async_accept(nd_queue_pair& qp, pd, token)` → `service.async_accept(impl,
  qp.native_handle(), pd, handler, io_ex)`;
- `async_disconnect(token)`; `remote_private_data()`; `is_open()`; `cancel()`;
- internal `assign_with_private_data(handle&&, remote_pd, config, ec)` (used by the listener).
- Remove `open(qp,…)` and `open(handle&&, qp,…)`.

### 5. `include/nd/detail/nd_listener_service.hpp`
- `open` takes the port-space value/stand-in for parity (ND ignores it); otherwise unchanged.
- Keep the `async_get_connection_request` op delivering `(ec, nd_connector_handle_t, span pd)` —
  the op already creates the IND2Connector for the inbound request; have it `GetPrivateData` and
  deliver the client's request private data span (copy into a stack local for the upcall, like ibv).

### 6. `include/nd/nd_listener.hpp`
Mirror `include/ibv/ibv_listener.hpp`:
- store `asio::io_context* io_ctx_` (set in ctor) to build the returned connector;
- `open(PortSpace const& ps, config = {})`;
- `async_get_connection(token)` → `void(ec, nd_connector<PortSpace>)`: wrap the service
  `async_get_connection_request` op; in the wrapper handler build `nd_connector<PortSpace>(*io_ctx_)`,
  `assign_with_private_data(std::move(handle), pd, {}, aec)`, then upcall `(ec, std::move(conn))`.
  **Bind the wrapper to a named local** before passing to the service op (the service takes
  `Handler&` and moves it — a temporary won't bind; this exact bug was hit on ibv).
- `async_get_connection(nd_connector<PortSpace>& conn, token)` → `void(ec)`: assign into the
  pre-built connector.
- Drop `async_get_connection_request` from the public IO object.

### 7. `include/rdma/tcp.hpp`
No change needed — it already aliases `tcp::{queue_pair,connector,listener}` and the `rdma_*`
names pick them up. Just confirm nd branch still compiles.

### 8. Tests
- `tests/nd/test_nd_echo.cpp`: already updated to the `rdma_*`-style flow on Linux? It currently
  uses `nd_*` types with the OLD API (`conn.open(qp)`, `async_connect(endpoint, …)`,
  `async_get_connection_request`). Rewrite to the new flow — mirror `tests/ibv/test_ibv_echo.cpp`
  (replace `ibv_` → `nd_`): `lis.open(tcp::v4())`, `async_get_connection` → `conn.async_accept(qp,…)`,
  client `conn.open(tcp::v4())` + `conn.async_connect(qp, ep, pd,…)`, `remote_private_data()`.
- `tests/nd/test_nd_refactored_compile.cpp` and other nd compile tests: update any
  `connector.open(qp,…)` / `async_get_connection_request` usages.
- The cross-platform `tests/rdma/test_rdma_echo.cpp` should now compile on Windows unchanged.

## Verification (on Windows)
```
cmake -B build -DRDMA_BACKEND=nd
cmake --build build
# run the nd echo (needs an ND-capable adapter / two endpoints):
build\tests\nd\test_nd_echo.exe --server --port 5000
build\tests\nd\test_nd_echo.exe --client <ip> --port 5000
# and the cross-platform one:
build\tests\rdma\test_rdma_echo.exe --server --port 5001
build\tests\rdma\test_rdma_echo.exe --client <ip> --port 5001
```
Expect 10 echo round-trips + clean disconnect on both, and the private-data lines to print
"client-hello" (server) / "server-hello" (client).

## Notes
- Keep LF line endings (`.gitattributes`); `.vcxproj`/`.sln` stay CRLF.
- The connector owns the IND2Connector; the queue_pair owns the QP — so unlike ibv, after the
  connector dies the QP object is still alive (just disconnected). No ordering hazard.
- If `async_get_connection`'s connector construction needs the io_context and only an executor is
  available, store `asio::io_context*` in the listener (as ibv does) rather than querying.
