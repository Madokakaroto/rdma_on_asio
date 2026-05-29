# nd_connection / nd_listener / nd_queue_pair Refactoring Plan (v4)

## Design Principles

1. **严格遵循 asio 风格** — 所有 IO 对象用 `io_object_impl<Service>` + service 模式管理成员
2. **每个 IO 对象只绑定一个 io_context** — 无歧义的单一完成路径
3. **三个独立 IO 对象，各司其职：**
   - `nd_listener` — GetConnectionRequest，暴露 private data + native connector
   - `nd_connection` — Connect / Accept / Disconnect（控制面）
   - `nd_queue_pair` — Send / Recv / Read / Write（数据面）
4. **CQ 双模式管理：**
   - **IOCP 模式**：`nd_io_completion_service`（per io_context）内部持有共享 CQ
   - **Poll 模式**：用户自建 `nd_completion_queue`，手动 poll
5. **nd_io_completion_service 与 io_context 是 1:1** — 通过 `use_device()` 显式初始化
6. **Private data 作为 callback 入参暴露给用户**
7. **Server 侧 accept 拆成两阶段** — 中间有用户决策窗口
8. **所有 IO 对象支持 default construct + open** — 兼顾 coroutine 和 callback 两种使用模式
9. **所有 IO 对象以 `io_context&` 初始化** — 不用 executor

---

## Architecture Overview

```
io_context
├── win_iocp_io_context (asio internal, the IOCP proactor)
├── nd_io_completion_service (1:1, owns shared CQ for IOCP mode)
│     └── nd2_completion_queue_ptr
│           overlapped_handle registered to IOCP
├── nd_connector_service<PortSpace> (manages connector per-impl)
├── nd_verbs_service<PortSpace> (manages QP per-impl, verbs operations)
└── nd_listener_service<PortSpace> (manages listener per-impl)

nd_connection  ── io_object_impl<nd_connector_service>
                    impl.connector_
                    impl.connector_handle_ (registered to IOCP)

nd_queue_pair  ── io_object_impl<nd_verbs_service>
                    impl.qp_
                    impl.cq_ (from nd_io_completion_service OR external)

nd_listener    ── io_object_impl<nd_listener_service>
                    impl.listener_
                    impl.listener_handle_ (registered to IOCP)

nd_completion_queue (standalone, no service, poll mode only)
    nd2_completion_queue_ptr
    overlapped_handle (NOT registered to IOCP)
    poll() / poll_one()
```

---

## Device Initialization: use_device()

用户不直接传 `nd_device_ptr`，而是传选择条件。`use_device` 内部通过 `nd_device_manager_t` 选出匹配的 device。

```cpp
namespace asio::rdma {

// 通过 config 匹配 device（选第一个满足条件的 adapter）
nd_io_completion_service& use_device(
    asio::io_context& io_ctx,
    nd_config_t const& config = {});

nd_io_completion_service& use_device(
    asio::io_context& io_ctx,
    nd_config_t const& config,
    asio::error_code& ec);

// 通过用户 predicate 选择 device
// 返回 nullopt = 跳过此 device；返回有效 config = 选中并使用该 config
using device_selector = std::function<std::optional<nd_config_t>(nd_device_ptr const&)>;

nd_io_completion_service& use_device(
    asio::io_context& io_ctx,
    device_selector const& selector);

nd_io_completion_service& use_device(
    asio::io_context& io_ctx,
    device_selector const& selector,
    asio::error_code& ec);

} // namespace asio::rdma
```

**行为：**
- 内部访问 `nd_device_manager_t::instance()` 枚举所有 adapter
- config 重载：选第一个 capabilities >= config 各字段的 device，使用该 config
- selector 重载：对每个 device 调 selector，选第一个返回非 nullopt 的，使用返回的 config
- 选中后调用 `use_service<nd_io_completion_service>(io_ctx)` 获取/创建 service
- 用选中的 device + config 初始化 service（创建 CQ + 注册 handle 到 IOCP）
- 同一 io_context 重复调用报错（一个 io_context 只绑一个 device）

**用户代码：**
```cpp
// 方式 1: 用 config 自动匹配（最简单）
rdma::use_device(io_ctx);

// 方式 2: 指定约束条件
nd_config_t config;
config.max_send_wr_ = 128;
rdma::use_device(io_ctx, config);

// 方式 3: 自定义选择逻辑（selector 返回 config 表示选中）
rdma::use_device(io_ctx, [](nd_device_ptr const& dev) -> std::optional<nd_config_t> {
    if (dev->name_ != "192.168.1.10") return std::nullopt;
    nd_config_t config;
    config.cqe_ = dev->info_.MaxCqDepth / 2;  // 根据 device 能力决定
    return config;
});
```

---

## nd_config_t (revised)

统一配置结构，0 值字段表示"从 device capabilities 自动推导"。

```cpp
struct nd_config_t {
    // CQ 配置（use_device 时使用）
    ULONG cqe_ = 0;                // 0 = min(device_max, 4096)

    // QP 配置（nd_queue_pair open 时使用）
    ULONG max_send_wr_ = 0;        // 0 = min(device_max, 128)
    ULONG max_recv_wr_ = 0;        // 0 = min(device_max, 128)
    ULONG max_send_sge_ = 0;       // 0 = min(device_max, 4)
    ULONG max_recv_sge_ = 0;       // 0 = min(device_max, 4)
    ULONG max_inline_data_ = 0;    // 0 = device default

    // Connection 配置
    ULONG inbound_read_limit_ = 0;  // 0 = device default
    ULONG outbound_read_limit_ = 0; // 0 = device default

    // Listener 配置
    int backlog_ = 128;
};
```

**推导规则：**
- 字段值为 0 → 内部从 `ND2_ADAPTER_INFO` 查询 device 能力，取 `min(device_max, reasonable_default)`
- 字段值非 0 → validate 不超过 device max，使用用户指定值
- 新手不传 config 也能工作，高级用户可精确控制

---

## nd_io_completion_service

```cpp
namespace detail {

class nd_io_completion_service
    : public asio::detail::execution_context_service_base<nd_io_completion_service> {
public:
    using base_type =
        asio::detail::execution_context_service_base<nd_io_completion_service>;

    explicit nd_io_completion_service(asio::execution_context& ctx);
    ~nd_io_completion_service();

    void shutdown() override;

    // 初始化（由 use_device 调用，仅一次）
    void initialize(nd_device_ptr const& device, nd_config_t const& config,
                    asio::error_code& ec);
    bool is_initialized() const noexcept;

    // 获取 adapter（供 connector/listener/QP 创建使用）
    nd_adapter_ptr get_adapter() const noexcept;

    // 获取 CQ（供 QP 创建时使用）
    IND2CompletionQueue* get_cq() const noexcept;

    // 获取生效的配置（default 字段已被推导填充）
    nd_config_t const& get_effective_config() const noexcept;

    // 提交 CQ notify 请求（verbs op 提交后调用）
    void arm_notify(nd_verbs_op_base* op, asio::error_code& ec);

private:
    asio::detail::win_iocp_io_context& scheduler_;
    nd_device_ptr device_;
    nd_config_t effective_config_;   // 推导后的实际配置
    nd2_completion_queue_ptr cq_;
    unique_handle_t cq_handle_;
    bool initialized_ = false;
};

} // namespace detail
```

---

## nd_queue_pair

数据面 IO 对象。负责 send / recv / read / write。

```cpp
template <typename PortSpace>
class nd_queue_pair {
public:
    using service_type = detail::nd_verbs_service<PortSpace>;

    // === 构造 / 析构 / 移动 ===

    nd_queue_pair() = default;
    ~nd_queue_pair() = default;
    nd_queue_pair(nd_queue_pair&&) = default;
    nd_queue_pair& operator=(nd_queue_pair&&) = default;
    nd_queue_pair(nd_queue_pair const&) = delete;
    nd_queue_pair& operator=(nd_queue_pair const&) = delete;

    // 一步到位（IOCP 模式，coroutine 友好）
    explicit nd_queue_pair(asio::io_context& io_ctx,
                           nd_config_t const& config = {});

    // 一步到位（Poll 模式，coroutine 友好）
    nd_queue_pair(asio::io_context& io_ctx,
                  nd_completion_queue& cq,
                  nd_config_t const& config = {});

    // === 延迟初始化（callback/session 友好）===

    // IOCP 模式
    void open(asio::io_context& io_ctx, nd_config_t const& config = {});
    void open(asio::io_context& io_ctx, nd_config_t const& config,
              asio::error_code& ec);

    // Poll 模式
    void open(asio::io_context& io_ctx, nd_completion_queue& cq,
              nd_config_t const& config = {});
    void open(asio::io_context& io_ctx, nd_completion_queue& cq,
              nd_config_t const& config, asio::error_code& ec);

    // === 状态 ===
    bool is_open() const noexcept;
    IND2QueuePair* native_handle() const noexcept;

    // === 数据操作 ===

    template <mr_const_buffer_sequence ConstBufferSequence,
              ASIO_COMPLETION_TOKEN_FOR(void(asio::error_code, std::size_t)) WriteToken>
    auto async_send(ConstBufferSequence const& buffers, WriteToken&& token);

    template <mr_mutable_buffer_sequence MutableBufferSequence,
              ASIO_COMPLETION_TOKEN_FOR(void(asio::error_code, std::size_t)) ReadToken>
    auto async_recv(MutableBufferSequence const& buffers, ReadToken&& token);

    template <mr_const_buffer_sequence ConstBufferSequence,
              ASIO_COMPLETION_TOKEN_FOR(void(asio::error_code, std::size_t)) WriteToken>
    auto async_write(ConstBufferSequence const& buffers,
                     nd_remote_addr_t const& remote_addr, WriteToken&& token);

    template <mr_mutable_buffer_sequence MutableBufferSequence,
              ASIO_COMPLETION_TOKEN_FOR(void(asio::error_code, std::size_t)) ReadToken>
    auto async_read(MutableBufferSequence const& buffers,
                    nd_remote_addr_t const& remote_addr, ReadToken&& token);

private:
    std::unique_ptr<asio::detail::io_object_impl<service_type>> pimpl_;
};
```

---

## nd_connection

控制面 IO 对象。负责 Connect / Accept / Disconnect。
与 nd_listener 相同，总是事先绑定 io_context（控制面操作频率极低，无需独立调度）。

```cpp
template <typename PortSpace>
class nd_connection {
public:
    using service_type = detail::nd_connector_service<PortSpace>;
    using endpoint_type = typename PortSpace::endpoint;
    using native_connector_type = detail::nd_connector_handle_t;

    // === 构造 / 析构 / 移动 ===

    explicit nd_connection(asio::io_context& io_ctx);
    ~nd_connection() = default;
    nd_connection(nd_connection&&) = default;
    nd_connection& operator=(nd_connection&&) = default;
    nd_connection(nd_connection const&) = delete;
    nd_connection& operator=(nd_connection const&) = delete;

    // === 初始化 ===

    // client: 创建新 connector
    void open(nd_queue_pair<PortSpace>& qp,
              nd_config_t const& config = {});
    void open(nd_queue_pair<PortSpace>& qp,
              nd_config_t const& config, asio::error_code& ec);

    // server: 从 listener 拿到的 native connector
    void open(native_connector_type&& connector,
              nd_queue_pair<PortSpace>& qp,
              nd_config_t const& config = {});
    void open(native_connector_type&& connector,
              nd_queue_pair<PortSpace>& qp,
              nd_config_t const& config, asio::error_code& ec);

    // === 状态 ===
    bool is_open() const noexcept;
    void cancel();

    // === 连接管理 ===

    // Signature: void(error_code, span<const byte> peer_private_data)
    template <ASIO_COMPLETION_TOKEN_FOR(void(asio::error_code, std::span<const std::byte>))
              ConnectToken>
    auto async_connect(endpoint_type const& endpoint,
                       std::span<const std::byte> outgoing_private_data,
                       ConnectToken&& token);

    // Signature: void(error_code)
    template <ASIO_COMPLETION_TOKEN_FOR(void(asio::error_code)) AcceptToken>
    auto async_accept(std::span<const std::byte> outgoing_private_data,
                      AcceptToken&& token);

    // Signature: void(error_code)
    template <ASIO_COMPLETION_TOKEN_FOR(void(asio::error_code)) DisconnectToken>
    auto async_disconnect(DisconnectToken&& token);

private:
    asio::detail::io_object_impl<service_type> impl_;
};
```

---

## nd_listener

`nd_listener` 总是及时创建，不需要延迟绑定 io_context，直接持有 `io_object_impl` 值成员。

```cpp
template <typename PortSpace>
class nd_listener {
public:
    using service_type = detail::nd_listener_service<PortSpace>;
    using endpoint_type = typename PortSpace::endpoint;
    using native_connector_type = detail::nd_connector_handle_t;

    // === 构造 / 析构 / 移动 ===

    explicit nd_listener(asio::io_context& io_ctx);
    ~nd_listener() = default;
    nd_listener(nd_listener&&) = default;
    nd_listener& operator=(nd_listener&&) = default;
    nd_listener(nd_listener const&) = delete;
    nd_listener& operator=(nd_listener const&) = delete;

    // === 配置 ===
    void open(nd_config_t const& config = {});
    void open(nd_config_t const& config, asio::error_code& ec);
    void bind(uint16_t port);
    void bind(uint16_t port, asio::error_code& ec);
    void listen(int backlog = 128);
    void listen(int backlog, asio::error_code& ec);

    // === 状态 ===
    bool is_open() const noexcept;
    void cancel();

    // === 异步操作 ===

    // Signature: void(error_code, native_connector_type, span<const byte>)
    template <ASIO_COMPLETION_TOKEN_FOR(
        void(asio::error_code, native_connector_type, std::span<const std::byte>))
        AcceptToken>
    auto async_get_connection_request(AcceptToken&& token);

private:
    asio::detail::io_object_impl<service_type> impl_;
};
```

---

## nd_completion_queue (Poll Mode Only)

独立对象，不绑 io_context，不走 IOCP。CQ depth 从 `nd_config_t::cqe_` 取。

```cpp
class nd_completion_queue {
public:
    nd_completion_queue(nd_device_ptr const& device,
                        nd_config_t const& config = {});
    ~nd_completion_queue();

    nd_completion_queue(nd_completion_queue const&) = delete;
    nd_completion_queue& operator=(nd_completion_queue const&) = delete;
    nd_completion_queue(nd_completion_queue&&) = default;
    nd_completion_queue& operator=(nd_completion_queue&&) = default;

    std::size_t poll();
    std::size_t poll(asio::error_code& ec);
    std::size_t poll_one();
    std::size_t poll_one(asio::error_code& ec);

    IND2CompletionQueue* native_handle() const noexcept;

private:
    nd_device_ptr device_;
    nd2_completion_queue_ptr cq_;
    unique_handle_t handle_;
};
```

---

## native_connector_type

```cpp
namespace detail {

struct nd_connector_handle_t {
    nd2_connector_ptr connector_;
    unique_handle_t overlapped_handle_;
    nd_adapter_ptr adapter_;

    nd_connector_handle_t() = default;
    nd_connector_handle_t(nd_connector_handle_t&&) = default;
    nd_connector_handle_t& operator=(nd_connector_handle_t&&) = default;
    nd_connector_handle_t(nd_connector_handle_t const&) = delete;
    nd_connector_handle_t& operator=(nd_connector_handle_t const&) = delete;
};

} // namespace detail
```

---

## Services (Internal)

### nd_verbs_service

```cpp
namespace detail {

template <typename PortSpace>
class nd_verbs_service
    : public asio::detail::execution_context_service_base<
          nd_verbs_service<PortSpace>>
    , public nd_service_base {
public:
    struct implementation_type : nd_service_base::base_implementation_type {
        nd2_queue_pair_ptr qp_;
        IND2CompletionQueue* cq_;
        bool iocp_mode_;
        nd_config_t config_;
    };

    explicit nd_verbs_service(asio::execution_context& ctx);
    void shutdown() override;

    // lifecycle
    void construct(implementation_type& impl);
    void destroy(implementation_type& impl);
    void move_construct(implementation_type& impl, implementation_type& other);
    void move_assign(implementation_type& impl,
                     nd_verbs_service& other_service,
                     implementation_type& other_impl);

    // open (IOCP mode)
    void open(implementation_type& impl, nd_config_t const& config,
              asio::error_code& ec);
    // open (Poll mode)
    void open(implementation_type& impl, IND2CompletionQueue* external_cq,
              nd_config_t const& config, asio::error_code& ec);

    // verbs async operations
    template <typename BufferSequence, typename Handler, typename IoExecutor>
    void async_send(implementation_type& impl, BufferSequence const& buffers,
                    Handler& handler, IoExecutor const& io_ex);

    template <typename BufferSequence, typename Handler, typename IoExecutor>
    void async_recv(implementation_type& impl, BufferSequence const& buffers,
                    Handler& handler, IoExecutor const& io_ex);

    template <typename BufferSequence, typename Handler, typename IoExecutor>
    void async_read(implementation_type& impl, BufferSequence const& buffers,
                    nd_remote_addr_t const& remote_addr,
                    Handler& handler, IoExecutor const& io_ex);

    template <typename BufferSequence, typename Handler, typename IoExecutor>
    void async_write(implementation_type& impl, BufferSequence const& buffers,
                     nd_remote_addr_t const& remote_addr,
                     Handler& handler, IoExecutor const& io_ex);
};

} // namespace detail
```

### nd_connector_service

```cpp
namespace detail {

template <typename PortSpace>
class nd_connector_service
    : public asio::detail::execution_context_service_base<
          nd_connector_service<PortSpace>>
    , public nd_service_base {
public:
    using endpoint_type = typename PortSpace::endpoint;

    struct implementation_type : nd_service_base::base_implementation_type {
        nd2_connector_ptr connector_;
        unique_handle_t connector_handle_;
        IND2QueuePair* qp_;         // borrowed from nd_queue_pair
        nd_adapter_ptr adapter_;
        nd_config_t config_;
    };

    explicit nd_connector_service(asio::execution_context& ctx);
    void shutdown() override;

    // lifecycle
    void construct(implementation_type& impl);
    void destroy(implementation_type& impl);
    void move_construct(implementation_type& impl, implementation_type& other);
    void move_assign(implementation_type& impl,
                     nd_connector_service& other_service,
                     implementation_type& other_impl);

    // open (client: create new connector)
    void open(implementation_type& impl, IND2QueuePair* qp,
              nd_config_t const& config, asio::error_code& ec);
    // open (server: from native connector)
    void open(implementation_type& impl, nd_connector_handle_t&& connector,
              IND2QueuePair* qp, nd_config_t const& config,
              asio::error_code& ec);

    // async operations
    template <typename Handler, typename IoExecutor>
    void async_connect(implementation_type& impl, endpoint_type const& ep,
                       std::span<const std::byte> private_data,
                       Handler& handler, IoExecutor const& io_ex);

    template <typename Handler, typename IoExecutor>
    void async_accept(implementation_type& impl,
                      std::span<const std::byte> private_data,
                      Handler& handler, IoExecutor const& io_ex);

    template <typename Handler, typename IoExecutor>
    void async_disconnect(implementation_type& impl,
                          Handler& handler, IoExecutor const& io_ex);
};

} // namespace detail
```

### nd_listener_service

```cpp
namespace detail {

template <typename PortSpace>
class nd_listener_service
    : public asio::detail::execution_context_service_base<
          nd_listener_service<PortSpace>>
    , public nd_service_base {
public:
    using endpoint_type = typename PortSpace::endpoint;

    struct implementation_type : nd_service_base::base_implementation_type {
        nd2_listener_ptr listener_;
        unique_handle_t listener_handle_;
        nd_adapter_ptr adapter_;
        nd_config_t config_;
    };

    explicit nd_listener_service(asio::execution_context& ctx);
    void shutdown() override;

    // lifecycle
    void construct(implementation_type& impl);
    void destroy(implementation_type& impl);
    void move_construct(implementation_type& impl, implementation_type& other);
    void move_assign(implementation_type& impl,
                     nd_listener_service& other_service,
                     implementation_type& other_impl);

    // open / bind / listen
    void open(implementation_type& impl, nd_config_t const& config,
              asio::error_code& ec);
    void bind(implementation_type& impl, uint16_t port, asio::error_code& ec);
    void listen(implementation_type& impl, int backlog, asio::error_code& ec);

    // async operations
    template <typename Handler, typename IoExecutor>
    void async_get_connection_request(implementation_type& impl,
                                     Handler& handler, IoExecutor const& io_ex);
};

} // namespace detail
```

---

## Overlapped Handle 总览

| 对象 | Handle 归属 | 注册到 IOCP 时机 | 用途 |
|------|-------------|-----------------|------|
| nd_io_completion_service | service 内部 | `use_device()` 时 | CQ Notify 完成通知 |
| nd_connector_service impl | per-connection | `open()` 时 | Connect / Accept / Disconnect 完成 |
| nd_listener_service impl | per-listener | `open()` 时 | GetConnectionRequest 完成 |
| nd_completion_queue | standalone | **不注册** | CQ 创建需要，poll 模式不走 IOCP |

---

## 用户代码示例

### Client IOCP Mode (Coroutine)

```cpp
asio::io_context io_ctx;

nd_config_t config;
config.cqe_ = 256;
config.max_send_wr_ = 64;
rdma::use_device(io_ctx, config);

rdma::nd_queue_pair<rdma::tcp> qp(io_ctx, config);
rdma::nd_connection<rdma::tcp> conn(io_ctx);
conn.open(qp, config);

auto [ec, peer_pd] = co_await conn.async_connect(endpoint, my_pd, use_awaitable);
co_await qp.async_send(buf, use_awaitable);
co_await qp.async_recv(buf, use_awaitable);
```

### Client Poll Mode (Coroutine)

```cpp
asio::io_context io_ctx;

nd_config_t config;
config.cqe_ = 256;
rdma::use_device(io_ctx, config);

auto& svc = asio::use_service<rdma::detail::nd_io_completion_service>(io_ctx);
rdma::nd_completion_queue cq(svc.get_device(), config);
rdma::nd_queue_pair<rdma::tcp> qp(io_ctx, cq, config);
rdma::nd_connection<rdma::tcp> conn(io_ctx);
conn.open(qp, config);

auto [ec, peer_pd] = co_await conn.async_connect(endpoint, my_pd, use_awaitable);

qp.async_send(buf, [](error_code ec, size_t n) { /* ... */ });
while (running) {
    cq.poll();
}
```

### Server IOCP Mode (Coroutine)

```cpp
asio::io_context io_ctx_main;
nd_config_t config;
config.cqe_ = 1024;
rdma::use_device(io_ctx_main, config);

rdma::nd_listener<rdma::tcp> listener(io_ctx_main, config);
listener.bind(port);
listener.listen();

// 第一阶段
auto [ec, connector, peer_pd] =
    co_await listener.async_get_connection_request(use_awaitable);

// 用户决策窗口
auto& io_ctx_worker = select_io_context(peer_pd);
rdma::use_device(io_ctx_worker, config);  // 若尚未初始化

// 创建 QP + connection（conn 复用 listener 的 io_context）
rdma::nd_queue_pair<rdma::tcp> qp(io_ctx_worker, config);
rdma::nd_connection<rdma::tcp> conn(io_ctx_main);
conn.open(std::move(connector), qp, config);

// 第二阶段
co_await conn.async_accept(response_pd, use_awaitable);

// 数据收发
co_await qp.async_send(buf, use_awaitable);
```

### Server IOCP Mode (Callback + Session)

```cpp
class rdma_session {
    rdma::nd_connection<rdma::tcp> conn_;
    rdma::nd_queue_pair<rdma::tcp> qp_;
    // ... buffers, state, etc.

public:
    // session 构造时绑定 io_context（conn_ 复用 listener 的 io_context）
    explicit rdma_session(asio::io_context& io_ctx)
        : conn_(io_ctx) {}

    void start(nd_connector_handle_t connector, std::span<const std::byte> peer_pd) {
        auto& io_ctx_worker = select_io_context(peer_pd);
        nd_config_t config;
        rdma::use_device(io_ctx_worker, config);

        qp_.open(io_ctx_worker, config);
        conn_.open(std::move(connector), qp_, config);

        conn_.async_accept(response_pd,
            [this](error_code ec) {
                if (!ec) do_send();
            });
    }

private:
    void do_send() {
        qp_.async_send(buf_, [this](error_code ec, size_t n) {
            if (!ec) do_recv();
        });
    }

    void do_recv() {
        qp_.async_recv(buf_, [this](error_code ec, size_t n) {
            // ...
        });
    }
};

// 使用
auto session = std::make_shared<rdma_session>(io_ctx_main);

listener.async_get_connection_request(
    [session](error_code ec, nd_connector_handle_t connector,
              std::span<const std::byte> peer_pd) {
        if (!ec) session->start(std::move(connector), peer_pd);
    });
```

---

## Server 侧完整时序

```
nd_listener                    用户代码                    nd_queue_pair / nd_connection
    │                              │                              │
    │ async_get_connection_request  │                              │
    │──────────┐                   │                              │
    │          │                   │                              │
    │  [IOCP: GetConnectionRequest]│                              │
    │          │                   │                              │
    │  callback(ec, connector, pd) │                              │
    │──────────┘                   │                              │
    │                              │                              │
    │                    ┌─────────▼──────────┐                   │
    │                    │ 解析 private_data   │                   │
    │                    │ 选择 io_context     │                   │
    │                    │ qp_.open(io_ctx)   │                   │
    │                    │ conn_.open(io_ctx)  │                   │
    │                    └─────────┬──────────┘                   │
    │                              │                              │
    │                              │  conn.async_accept           │
    │                              │─────────────────────────────►│
    │                              │                              │──┐
    │                              │                   [IOCP: Accept] │
    │                              │                              │──┘
    │                              │                 callback(ec) │
    │                              │◄─────────────────────────────│
    │                              │                              │
    │                              │  qp.async_send              │
    │                              │─────────────────────────────►│
    │                              │                              │
```

---

## Completion Flows

### IOCP Mode (verbs)

```
qp.async_send(buf, handler)
  │
  ├── nd_verbs_service::async_send()
  │     allocate nd_send_op (holds handler)
  │     post_send(QP, op_as_context, sge_list)
  │     └── nd_io_completion_service::arm_notify(op)
  │           → CQ::Notify(OVERLAPPED* = notify_op)
  │           → scheduler_.on_pending(notify_op)
  │
  ├── IOCP fires (CQ has completion)
  │     notify_op->do_complete():
  │       → CQ::GetResults()
  │       → cast RequestContext → nd_verbs_op_base*
  │       → op->complete(owner) → user handler invoked
  │
  └── if more completions: re-arm notify
```

### Poll Mode (verbs)

```
qp.async_send(buf, handler)
  │
  ├── nd_verbs_service::async_send()
  │     allocate nd_send_op (holds handler)
  │     post_send(QP, op_as_context, sge_list)
  │     └── (no arm_notify in poll mode)
  │
  ├── user: cq.poll()
  │     CQ::GetResults()
  │     for each result:
  │       op = cast RequestContext
  │       op->complete(nullptr) → handler invoked inline
  │
  └── return count
```

### Connector OVERLAPPED (connect/accept/disconnect)

```
conn.async_connect(endpoint, pd, handler)
  │
  ├── nd_connector_service::async_connect()
  │     allocate nd_connect_op
  │     IND2Connector::Connect(QP, addr, pd, op_as_OVERLAPPED)
  │     → scheduler_.on_pending(op)
  │
  ├── IOCP fires (connector handle)
  │     op->do_complete():
  │       → GetOverlappedResult
  │       → CompleteConnect
  │       → GetPrivateData
  │       → handler(ec, peer_pd)
  │
  └── (may need continuation for two-phase connect)
```

---

## Implementation Phases

### Phase 1: nd_io_completion_service + use_device

1. 实现 `nd_io_completion_service`
2. 实现 `use_device()` 自由函数
3. 实现 `nd_notify_completion_op`（IOCP CQ dispatch）

### Phase 2: nd_completion_queue (Poll Mode)

4. 实现独立 `nd_completion_queue`

### Phase 3: nd_queue_pair

5. 实现 `nd_verbs_service`
6. 实现 `nd_queue_pair` IO 对象（default construct + open + 一步到位构造）

### Phase 4: nd_connection

7. 实现 `nd_connector_service`
8. 实现 `nd_connection` IO 对象
9. 重写 `nd_connect_op`（支持 private data）
10. 实现 `nd_accept_op`（connection 侧）
11. 实现 `nd_disconnect_op`

### Phase 5: nd_listener

12. 实现 `nd_listener_service`
13. 实现 `nd_listener` IO 对象
14. 实现 `async_get_connection_request` op（GetPrivateData + 返回 connector）

### 实现原则：复用 nd_ops 层

各 service 实现（`nd_connector_service`、`nd_listener_service`、`nd_verbs_service`）内部应尽可能调用
`detail/nd_ops_cm.hpp` 和 `detail/nd_ops_verbs.hpp` 中的封装函数。这些函数的职责是将
Windows ND2 原生 API 封装为带 `asio::error_code` 的接口。

- Service 层负责：op 生命周期管理、IOCP 注册、handler dispatch
- Ops 层负责：ND2 API 调用 + HRESULT → error_code 转换

如果实现过程中发现 ops 层的函数签名不合理或缺失，暂停并讨论方案。

### Phase 6: 清理

15. 删除旧 `nd_connector_state_t` / `nd_iocp_connector_service` / `nd_iocp_listener_service`
16. 删除旧 `nd_connection` / `nd_listener` 中的 `set_executor` / `has_executor`

---

## Object Lifecycle & Dependency

```
use_device(io_ctx, device)              ← 最先调用
    │
    ▼
nd_io_completion_service initialized    ← adapter + CQ ready
    │
    ├── nd_queue_pair.open(io_ctx)      ← 从 service 取 adapter + CQ，创建 QP
    │       │
    │       ▼
    │   nd_connection.open(io_ctx, qp)  ← 从 service 取 adapter，创建 connector，引用 QP
    │       │
    │       ▼
    │   async_connect / async_accept
    │       │
    │       ▼
    │   qp.async_send / qp.async_recv
    │
    └── nd_listener.open(io_ctx)        ← 从 service 取 adapter，创建 listener
            │
            ▼
        async_get_connection_request
            │
            ▼
        callback(connector, pd)  →  用户决策  →  qp.open + conn.open
```
