# 取消计划 Stage 1 —— 对象级取消(控制面 + 数据面)

> 三篇分期计划之一。Stage 1 = **对象级**取消,同时覆盖控制面与数据面。
> 相关:`cancellation_stage2_control_single_op.md`(控制面单操作)、
> `cancellation_stage3_data_single_op.md`(数据面单操作)、`asio_cancellation_analysis.md`(asio 机制详解)。
>
> **依赖关系**:Stage 1 是基础。Stage 2 独立;Stage 3 依赖本阶段对"数据面 teardown = disconnect"的结论。
> 建议先做 Stage 1。

## 本次评审落定的核心决策(范围的设计前提)

经多轮评审,本阶段的设计在原结论之上有三处重大收敛,务必先读:

1. **数据面不新增 cancel**(沿用旧结论)。已 post 的 WR 的 buffer 归硬件所有,RDMA 没有 `ibv_cancel_wr`;
   撤回在途 WR 只能转 QP 到 ERROR(flush 全部)或销毁 QP,都是**整 QP、terminal**。而 `rdma_disconnect`
   本就自带这次 flush(转 `FLUSH_ERR` -> `operation_aborted`,见 `ibv_op_complete.hpp:15-24`)。所以
   **数据面"对象级取消" = `connector::disconnect()`,不新增 `queue_pair::cancel()` / 本地 flush。**

2. **`connector::cancel()` 删除,折叠进 `disconnect()`。** connector 上"中止在途 connect"与"拆已建立连接"
   不是两件事:它们对应连接生命周期的两个互斥阶段。`disconnect()` 现已**自带** `cancel_ops`
   (connector_service.hpp:246-248),只差一个"未建立时不要调 `rdma_disconnect`"的守卫。于是
   **`disconnect()` 升级为按连接状态自适应的唯一 teardown**,connector 不再暴露 `cancel()`。
   - 理由:pending connect 时 cm_id 卡在握手中途,`rdma_disconnect` 对未建立 cm_id 返回 EINVAL
     (`ibv_ops_cm.hpp:169-182` 裸调 `rdma_disconnect`);established 时才该 `rdma_disconnect`。一个
     按状态分派的 `disconnect()` 两种情况都做对,且消灭了"先 cancel、再查结果、再决定要不要 disconnect"的竞争。

3. **`listener::cancel()` 保留。** listener 没有"连接"也就没有 disconnect;它唯一的对象级操作就是
   "中止在途 `async_get_connection`,对象保持 LISTEN 可复用",这正是 `acceptor::cancel()`。
   故 listener 仍走 `cancel_ops`(listener_service.hpp:125-129),不动。

4. **MT-safe(本阶段最大改动)。** asio 把 socket 设成"共享对象线程不安全、让用户用 strand 串行化",是因为
   底层 syscall(close 撞 read 等)本身不安全。**librdmacm / verbs 自带内部锁、容忍并发调用**,所以 RDMA 的
   `disconnect()` **要做成线程安全**:可从任意线程调用,无需把它 post 回 io_context。实现用
   `std::atomic<connect_state>` + CAS 仲裁(见下),不引入锁。代价仅为 C++ 通用的"别并发析构同一对象"。

---

## 设计 A:`connect_state_` 细粒度状态机 + 无锁 MT-safe teardown(本阶段核心)

### A.1 现状与问题

`ibv_connector_service::implementation_type` 现在只有一个 `bool disconnected_`
(connector_service.hpp:52),身兼两职:(a) `disconnect()` 设它(241 行)做 teardown 标志;
(b) wait-disconnect op 通过 `bool*`(op_wait_disconnect.hpp:30、52、59)在收到 peer DISCONNECTED 时设它,
令 `async_wait_disconnect` level-triggered(connector_service.hpp:265)。

它在多线程下的暴露:`do_perform`(reactor 线程,持 epoll descriptor 锁,见
`epoll_reactor.ipp:784-812`)写它,而 `disconnect()`(调用者线程)无锁读写它 —— 跨线程即 data race + 逻辑上
的 cancel/complete 竞争(读到 connecting、op 同时翻 connected -> 漏掉 `rdma_disconnect` -> 半开泄漏)。

### A.2 新状态:细粒度 `connect_state_`(原子)

把 `bool disconnected_` 换成两个原子量。`connect_state` 枚举放进 `ibv_impl_types.hpp`(op、accept op、
service 共享):

```cpp
// teardown 仲裁状态: 镜像 connect/accept op 的阶段, 是 disconnect() 决策的唯一依据.
enum class connect_state : int {
  idle,          // 尚未发起 connect/accept (op 未 arm)
  addr_resolve,  // [client] resolve_addr 已发, 等 ADDR_RESOLVED
  addr_route,    // [client] resolve_route 已发, 等 ROUTE_RESOLVED
  connecting,    // rdma_connect/rdma_accept 已发, 等 ESTABLISHED
  connected,     // ESTABLISHED -- 唯一让 rdma_disconnect 合法的状态
  closed,        // 已拆 / 已 abort / 已失败 -- terminal, 只能销毁
};

struct implementation_type : ... {
  std::atomic<connect_state> connect_state_{connect_state::idle};
  std::atomic<bool>          peer_closed_{false};   // wait 通知闩, 与 teardown 解耦 (A.6)
  // ... cm_channel_ / cm_id_ / cm_reactor_data_ / timeout_ / private_data_ ...
};
```

server(accept)只经过 `idle -> connecting -> connected`,跳过两个 resolve 态;client(connect)走全程。

### A.3 状态转换表

| 触发 | 位置 | 线程 | CAS from -> to |
|---|---|---|---|
| `start_connect_op` 发 `resolve_addr` 后 | service | 发起线程 | `idle -> addr_resolve` |
| `do_process_addr_resolve` 收 `ADDR_RESOLVED`,建 QP/发 `resolve_route` 前 | connect op | reactor | `addr_resolve -> addr_route` |
| `do_process_addr_route` 收 `ROUTE_RESOLVED`,发 `rdma_connect` 前 | connect op | reactor | `addr_route -> connecting` |
| `start_accept_op` 发 `rdma_accept` 后 | service | 发起线程 | `idle -> connecting` |
| 收 `ESTABLISHED` | connect/accept op | reactor | `connecting -> connected`(**唯一仲裁点**)|
| 任一阶段失败(ADDR_ERROR/ROUTE_ERROR/REJECTED/...) | op | reactor | `<当前阶段态> -> closed` |
| `disconnect()` | service | **任意线程** | `<当前态> -> closed`(CAS 循环)|
| peer DISCONNECTED/DEVICE_REMOVAL | wait op | reactor | 不动 `connect_state_`;`peer_closed_=true`(A.6)|

**核心不变量:`<...> -> connected` 这一个 CAS 是离开"可建立"区间的唯一原子仲裁点。**
两个竞争者:op(`connecting -> connected`)与 `disconnect()`(`* -> closed`)。**谁第二个动,谁负责对已建立
的连接做那唯一一次 `rdma_disconnect`。**

### A.4 reactor 侧:逐阶段 CAS 推进(防覆盖 + 第二行动者补拆)

因为状态细分到每阶段,推进**必须用 CAS 而非 store**:普通 store 会覆盖 `disconnect()` 刚设的 `closed`。
两类 CAS:
- **前进**用精确 expected 的 `advance(from, to)`:本 op 是唯一前进写者且被 reactor 串行化,失败必因
  disconnect 抢先(actual == closed),中间阶段一律 bail 成 `operation_aborted`(从未建立,无需拆)。
- **失败收尾**用 `claim_closed()`(任意非 closed 态 -> closed 的循环):失败分支无需关心精确阶段,谁先到
  `closed` 谁报错;disconnect 抢先则覆盖为 aborted。
- 唯一例外是 ESTABLISHED 边界:`CAS(connecting -> connected)` 失败时要"第二行动者补拆"。

`ibv_op_connect.hpp` 全量改动(基类 `ibv_connect_op_base`)。

**新增成员 + 构造**:`std::atomic<connect_state>* state_;`(指向 `impl.connect_state_`,构造参数透传)。

**三个 helper**:
```cpp
// 前进: 仅从精确前一阶段推进. 失败 iff disconnect() 已 claim closed (唯一的另一写者).
bool advance(connect_state from, connect_state to) {
  connect_state e = from;
  return state_->compare_exchange_strong(
      e, to, std::memory_order_acq_rel, std::memory_order_acquire);
}
// reactor 侧终态失败: claim closed, 除非 disconnect() 抢先. 返回 true=我们claim(保留真实
// error), false=disconnect 已赢(调用方把 ec_ 覆盖成 operation_aborted).
bool claim_closed() {
  connect_state e = state_->load(std::memory_order_acquire);
  for (;;) {
    if (e == connect_state::closed) return false;          // disconnect won
    if (state_->compare_exchange_weak(
            e, connect_state::closed,
            std::memory_order_acq_rel, std::memory_order_acquire))
      return true;
  }
}
// advance() 输给了 disconnect(): 从未建立, 直接报 aborted (无需拆).
status aborted_by_disconnect() {
  this->ec_ = asio::error::operation_aborted;
  return status::done;
}
```

**`do_perform` / `do_process`(派发器不变,且故意不加 blanket 早停)**:
```cpp
static status do_perform(asio::detail::reactor_op* base) {
  auto* op = static_cast<ibv_connect_op_base*>(base);
  unique_rdma_cm_event_ptr event{};
  if (op->get_cm_event(event)) return status::done;   // 拉事件硬错误
  if (!event)                  return status::not_done; // EAGAIN: 无事件, 续 arm
  return op->do_process(event);
}

// 注意: 这里故意不加 `if (state_==closed) bail`. blanket 早停会错误跳过 ESTABLISHED 仲裁 ——
// 若 disconnect() 设了 closed 而 ESTABLISHED 随后到达, 必须跑 do_process_connect 的第二行动者
// 补拆, 而不是静默 abort 泄漏一个已建立的 cm_id. 各阶段自己用 advance()/CAS 处理 closed.
status do_process(unique_rdma_cm_event_ptr const& event) {
  switch (stage_) {
    case stage_t::addr_resolve: return do_process_addr_resolve(event);
    case stage_t::addr_route:   return do_process_addr_route(event);
    case stage_t::connect:      return do_process_connect(event);
    default:
      this->ec_ = make_error_code(ibv_errc::ext_invalid_device);
      return status::done;
  }
}
```

**Stage 1 `do_process_addr_resolve`**:
```cpp
status do_process_addr_resolve(unique_rdma_cm_event_ptr const& event) {
  switch (event->event) {
    case RDMA_CM_EVENT_ADDR_RESOLVED:
      // 即将建 QP + resolve_route -> 先 addr_resolve -> addr_route. disconnect 已 claim
      // closed 则在做任何事之前 bail (不建 QP, 不 resolve_route).
      if (!advance(connect_state::addr_resolve, connect_state::addr_route))
        return aborted_by_disconnect();
      if (create_qp_) {
        this->ec_ = create_qp_(cm_id_);
        if (this->ec_) {
          if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
          return status::done;
        }
      }
      if (resolve_route(cm_id_, timeout_, this->ec_) == 0) {
        stage_ = stage_t::addr_route;
        return status::not_done;
      }
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
    case RDMA_CM_EVENT_ADDR_ERROR:
      this->ec_ = make_system_error_code(event->status ? -event->status : EHOSTUNREACH);
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
    default:
      this->ec_ = asio::error::connection_aborted;
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
  }
}
```

**Stage 2 `do_process_addr_route`**:
```cpp
status do_process_addr_route(unique_rdma_cm_event_ptr const& event) {
  switch (event->event) {
    case RDMA_CM_EVENT_ROUTE_RESOLVED: {
      // 即将发 rdma_connect -> 先 addr_route -> connecting.
      if (!advance(connect_state::addr_route, connect_state::connecting))
        return aborted_by_disconnect();             // disconnect 赢: 不 connect
      rdma_conn_param param{};
      param.private_data = private_data_;
      param.private_data_len = private_data_len_;
      param.responder_resources = responder_resources_;
      param.initiator_depth = initiator_depth_;
      param.retry_count = 7;
      param.rnr_retry_count = 7;
      if (connect(cm_id_, &param, this->ec_) == 0) {
        stage_ = stage_t::connect;
        return status::not_done;
      }
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
    }
    case RDMA_CM_EVENT_ROUTE_ERROR:
      this->ec_ = make_system_error_code(event->status ? -event->status : EHOSTUNREACH);
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
    default:
      this->ec_ = asio::error::connection_aborted;
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
  }
}
```

**Stage 3 `do_process_connect`(唯一仲裁点)**:
```cpp
status do_process_connect(unique_rdma_cm_event_ptr const& event) {
  switch (event->event) {
    case RDMA_CM_EVENT_ESTABLISHED: {
      // 仲裁: connecting -> connected. {本 op, disconnect()} 只有一个能离开 connecting.
      connect_state e = connect_state::connecting;
      if (state_->compare_exchange_strong(
              e, connect_state::connected,
              std::memory_order_acq_rel, std::memory_order_acquire)) {
        // 赢: 正常建立, 抓取 server 回复的 private data.
        auto const& cp = event->param.conn;
        if (remote_pd_.buf && remote_pd_.len && cp.private_data && cp.private_data_len) {
          std::size_t n = (std::min)(static_cast<std::size_t>(cp.private_data_len),
                                     remote_pd_.cap);
          std::memcpy(remote_pd_.buf, cp.private_data, n);
          *remote_pd_.len = n;
        }
        this->ec_ = asio::error_code{};            // success
      } else {
        // e == closed: disconnect() 在我们等 ESTABLISHED 时抢先, 它当时看到 connecting 没拆.
        // 连接确已建立 -> 由我们 (第二行动者) 补拆, 恰好一次.
        asio::error_code ignored;
        detail::disconnect(cm_id_, ignored);
        this->ec_ = asio::error::operation_aborted;
      }
      return status::done;
    }
    case RDMA_CM_EVENT_CONNECT_ERROR:
      this->ec_ = asio::error::connection_aborted;
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
    case RDMA_CM_EVENT_UNREACHABLE:
      this->ec_ = asio::error::host_unreachable;
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
    case RDMA_CM_EVENT_REJECTED:
      this->ec_ = asio::error::connection_refused;
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
    default:
      this->ec_ = asio::error::connection_aborted;
      if (!claim_closed()) this->ec_ = asio::error::operation_aborted;
      return status::done;
  }
}
```

**初态 `idle -> addr_resolve` 放在 service 的 `start_connect_op`,不在 `do_process`**(这是对原草图的修正):
若 `connect_state_` 在整个 `resolve_addr` 等待期间还停在 `idle`,则此时来的 `disconnect()` 读到 `old==idle`
-> 走 A.5 的 `idle` 分支(**不** `cancel_ops`)-> 在途 op 永不被中止。所以 `idle` 必须严格表示"op 未 arm",
arm(发 `resolve_addr`)时就要发布 `addr_resolve`:
```cpp
void start_connect_op(implementation_type& impl, endpoint_type const& endpoint,
                      asio::detail::reactor_op* op) {
  if (resolve_addr(impl.cm_id_.get(), nullptr,
                   const_cast<sockaddr*>(endpoint.data()), impl.timeout_, op->ec_) == 0) {
    // arm 前发布 addr_resolve, 使并发 disconnect() 能看到在途 op (addr_resolve -> cancel_ops).
    // 若 disconnect() 已抢先到 closed, CAS 失败 -> 完成 aborted, 不 arm.
    connect_state e = connect_state::idle;
    if (!impl.connect_state_.compare_exchange_strong(
            e, connect_state::addr_resolve,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
      op->ec_ = asio::error::operation_aborted;
      this->reactor_.post_immediate_completion(op, false);
      return;
    }
    op->ec_ = asio::error_code{};
    this->reactor_.start_op(asio::detail::reactor::read_op, impl.cm_channel_->fd,
                            impl.cm_reactor_data_, op, false, false);
  } else {
    impl.connect_state_.store(connect_state::closed, std::memory_order_release);
    this->reactor_.post_immediate_completion(op, false);
  }
}
```
所以 op 的 `do_process` 永不做 `idle -> addr_resolve`,只做 `addr_resolve -> addr_route`、
`addr_route -> connecting`、`connecting -> connected` 仲裁(+ 失败 `claim_closed()`),每个 `do_process_X`
从自己所在阶段往外推进。

**accept op(`ibv_op_accept.hpp`,单阶段)**:server 无 resolve 阶段。`start_accept_op` 在 arm 前
`CAS(idle -> connecting)`(同 start_connect_op 的保护);op 的 `do_perform` 收 ESTABLISHED 做
`CAS(connecting -> connected)` 仲裁(失败补拆),REJECT/ERROR 做 `claim_closed()`。

### A.5 `disconnect()`:任意线程的 CAS 循环 + 按前态分派

`ibv_connector_service::disconnect()`(现 240-249):

```cpp
void disconnect(implementation_type& impl, asio::error_code& ec) {
  ec.clear();
  connect_state old = impl.connect_state_.load(std::memory_order_acquire);
  for (;;) {
    if (old == connect_state::closed) return;                 // 幂等
    if (impl.connect_state_.compare_exchange_weak(
            old, connect_state::closed,
            std::memory_order_acq_rel, std::memory_order_acquire))
      break;          // 成功; weak 失败时 old 已刷新为当前值, 自动重新快照重决策
  }
  // 用"成功 CAS 时观察到的前态" old 分派清理:
  switch (old) {
    case connect_state::addr_resolve:
    case connect_state::addr_route:
    case connect_state::connecting:        // 三个中间态: 未建立 -> 只中止在途 op
      if (impl.cm_channel_)
        this->reactor_.cancel_ops(impl.cm_channel_->fd, impl.cm_reactor_data_);
      break;                               // 若它随后才建立, 由 op 的 ESTABLISHED-CAS-失败补拆
    case connect_state::connected:         // 已建立 -> 中止 armed wait + 拆, 恰好一次
      if (impl.cm_channel_)
        this->reactor_.cancel_ops(impl.cm_channel_->fd, impl.cm_reactor_data_);
      detail::disconnect(impl.cm_id_.get(), ec);
      break;
    case connect_state::idle:              // 无在途、未连接
    default: break;
  }
}
```

> CAS 循环与 `exchange(closed)` 在本状态机里**功能等价**(disconnect 的清理是"前态"的纯函数,且永远落到
> `closed`);此处按评审约定保留显式 CAS 循环。**为什么不会双重 `rdma_disconnect` / 双重完成**:
> `rdma_disconnect` 只出现在"第二行动者"路径(disconnect 见 `connected`,或 op 的 ESTABLISHED-CAS 失败),
> 由仲裁保证恰好一次;`cancel_ops` 与 op 的 `do_perform` 抢同一把 descriptor 锁(`epoll_reactor.ipp:355`
> vs 784),handler 锁内出队、恰好完成一次。

`ibv_connector.hpp` 的 `disconnect()`(120-128)签名不变(同步、`ec` 版 + throw 版);删除
`ibv_connector::cancel()`(79-81)与 service 的 `cancel()`(178-182)。

### A.6 peer-disconnect 通知必须与 teardown 解耦

`rdma_disconnect` 在 `disconnect()` 里还要 **flush 本地在途 WR**,即使对端已先断也要 flush。所以**绝不能**让
wait op 把 `connect_state_` 推到 `closed`(否则随后用户 `disconnect()` 走 `closed` no-op,永远不 flush 本地
WR -> 回归)。改用独立原子闩:

```cpp
// ibv_op_wait_disconnect.hpp: 把 bool* disconnected_ 换成 std::atomic<bool>* peer_closed_.
// 收到 RDMA_CM_EVENT_DISCONNECTED / DEVICE_REMOVAL (现 50-61):
peer_closed_->store(true, std::memory_order_release);   // 不动 connect_state_ (仍 connected)
this->ec_ = make_error_code(ibv_errc::ext_disconnected / ext_device_removed);
return status::done;

// async_wait_disconnect 立即完成判据 (现 connector_service.hpp:265):
if (impl.peer_closed_.load(acquire) ||
    impl.connect_state_.load(acquire) == connect_state::closed) { /* 立即 ext_disconnected */ }
```

### A.7 "已建立但 ESTABLISHED 事件未投递、cancel_ops 抢先"的残窗

交错:`disconnect()` 赢(设 `closed`),其 `cancel_ops` 在 op 还没 drain 到 ESTABLISHED 前就把 op abort 了
-> op 的"补拆"分支不会执行 -> 非优雅拆除(无 DREQ),靠析构兜底。**堵法**:`close_for_destruction`
(现 350-366)里 `drain_cm_events`(357)排空时,若发现尚未处理的 `ESTABLISHED`,补一次 `rdma_disconnect`
再 `rdma_destroy_id`,使迟到事件也优雅。

### A.8 内存序与残留契约

- **内存序**:所有 CAS 用 `memory_order_acq_rel`(失败侧 `acquire`),`peer_closed_` 用 release/acquire。
  control plane 是冷路径,直接全 `seq_cst` 也行、更易讲清正确性。release/acquire 的作用是把"对 `cm_id_` 的
  拆除访问 / handler 结果"正确发布给对手线程。
- **唯一残留契约 = 对象生命周期**:`cm_id_` 在 `connected` 期间稳定,只在 `close_for_destruction`/`destroy`
  里 reset。所以仍需一条**远弱于 asio 全序列化**的约束:**别在另一线程正跑 `disconnect()` 时析构同一
  connector** —— 这是 C++ 通用"不能边用边析构",不是 RDMA 额外负担。

### A.9 mechanical fixups

- `construct` / `move_construct`(79-93)/ `move_assign`(95-109):把 `disconnected_` 的拷贝换成
  `connect_state_` 与 `peer_closed_`(注意 atomic 不可平凡拷贝,move 时用 `.load()`/`.store()`)。
- `open()`(115-143)/ `assign()`(147-167):成功末尾设 `connect_state_ = idle`。
- `is_open()`(169-171)保持正交(`cm_id_ != nullptr`):它表"句柄在否",`connect_state_` 表"连接生命周期"。

---

## 设计 B:线程安全为何选 MT-safe 而非 asio-socket 式序列化

| | asio socket | 本项目 connector |
|---|---|---|
| 底层调用并发安全? | 否(close 撞 read 等)| **是**(librdmacm/verbs 自带内部锁)|
| 框架取舍 | "共享对象不安全",用户用 strand 串行化 | **`disconnect()` 做成线程安全**,任意线程可调 |
| 实现手段 | 用户 post 到 strand | `std::atomic<connect_state>` + CAS 仲裁(设计 A)|
| 残留约束 | 全对象序列化 | 仅"别并发析构同一对象"|

reactor 侧本就被 epoll descriptor 锁串行化(同一 connector 的 `do_perform` 不会在两个 `run()` 线程并发,
见 `epoll_reactor.ipp:784-812`),所以跨线程竞争只在"发起/disconnect 线程 vs reactor 线程"之间,正是设计 A
的 CAS 仲裁所覆盖。`listener::cancel()` 只调 `cancel_ops`(受 descriptor 锁保护)、不碰额外状态,与
`acceptor::cancel()` 同样安全。

---

## flush 是 disconnect 的真子集(为何不单独暴露 flush)

| | 本地 flush(`modify_qp ERR`) | `rdma_disconnect` |
|---|---|---|
| 本地 QP 转 ERROR + flush 在途 WR | yes | yes |
| 通知对端(发 DREQ) | no -> **静默 desync** | yes |
| 拆 CM 状态 | no | yes |
| 之后该做什么 | 还得 disconnect | 已是终态 |
| 对端已死时 | 本地完成、不阻塞 | 也不阻塞(本地转 ERROR + 发 DREQ 即返回)|

唯一正确的 flush 就是 disconnect 自带的那次。**且 `disconnect()` 在控制面上也是 `cancel` 的超集**:它先
`cancel_ops` 摘掉 CM 在途 op,再 `rdma_disconnect` 拆连接 + flush 数据面 WR。

## socket vs QP 的对称

- **asio socket**:多个 I/O 复用在一个长寿可重用 fd 上 -> `cancel()`(中止操作,对象仍可用)与 `close()`
  (销毁)是两件事,cancel 后照常用。
- **RDMA QP / cm_id**:对象本身约等于"连接" -> 中止其上在途操作基本就等于拆连接。所以数据面没有 socket 式
  cancel;看起来像"取消数据面",实质只能是 disconnect。
- 唯一干净对应"socket cancel 后可复用"的是 **`listener::cancel()`**(LISTEN 态不变,可再 accept);
  connector 的 connect 取消后 cm_id 卡在握手中途 -> terminal,故折叠进 `disconnect()`。

---

## 目标与范围

对象级取消语义(asio 惯例):取消该对象上全部在途操作,被取消者以 `operation_aborted` 完成。本阶段交付:

- **控制面 cancel**:仅 **`listener::cancel()`**(ibv 已实现,仅核验 + 测试;nd 补 TODO 桩)。
  **删除 `connector::cancel()`**,其语义并入 `disconnect()`(设计 A)。
- **数据面 teardown**:不新增接口;`connector::disconnect()` 令 pending send/recv/read/write 以
  `operation_aborted` 完成(via QP->ERROR flush -> `ibv_op_complete.hpp:15-24`)。
- **MT-safe disconnect**:`connect_state_` 原子状态机 + CAS 仲裁(设计 A),`disconnect()` 任意线程可调。

**不在本阶段**:单操作级(cancellation_slot)取消 —— 见 Stage 2 / Stage 3。其中"中止挂住的 connect"的推荐
UX(`co_await (connect || timeout)`)属 Stage 2;`connector::disconnect()` 只是其最终 dispatch 到的底层原语
+ 对象级 teardown。

---

## 控制面 cancel 现状

- **ibv listener(已就绪)**:`ibv_listener::cancel()`(ibv_listener.hpp:69-71)-> service
  `cancel()`(listener_service.hpp:125-129)= `reactor_.cancel_ops(impl.cm_channel_->fd,
  impl.cm_reactor_data_)`。**本阶段只核验 + 测试,无代码改动。** cancel 后 listener 仍 LISTEN、可复用。
- **ibv connector**:`cancel()`(connector.hpp:79-81 + service 178-182)**删除**,改 `disconnect()`(设计 A)。
- **nd listener(待实现)**:`nd_listener_service::cancel`(nd_listener_service.hpp:156-158)空 TODO。
  实现:`IND2Listener::CancelOverlappedRequests()`(整接口取消,匹配对象级语义),或
  `CancelIoEx(impl.listener_handle_.get(), NULL)` + 吞 `ERROR_NOT_FOUND`(照抄 asio
  `win_iocp_socket_service_base::cancel`)。被取消的 OVERLAPPED 经 `GetOverlappedResult` 拿 `ND_CANCELED`
  -> 映射 `nd_errc::canceled`。
- **nd connector**:同样删 `cancel()`(nd_connector_service.hpp:151-153 空 TODO,直接删),改为给 nd 的
  `disconnect()`(nd_connector_service.hpp:223-246)加同款 `connect_state_` 守卫:`connecting` 走
  `CancelOverlappedRequests`,`connected` 走 ND2 `Disconnect`,通知闩共用 `peer_closed_`。ND2 接口同样线程安全。

---

## 实施步骤

1. **ibv `connect_state_` 状态机**:
   - impl 加 `std::atomic<connect_state> connect_state_` + `std::atomic<bool> peer_closed_`,删 `bool disconnected_`。
   - connect op / accept op:逐阶段 CAS 推进 + ESTABLISHED 仲裁 + 第二行动者补拆(A.4)。
   - service `start_connect_op`/`start_accept_op`:arm **前** CAS 发布初态(`idle->addr_resolve` /
     `idle->connecting`),CAS 失败(disconnect 抢先)则完成 aborted、不 arm(A.4 末)。
   - `disconnect()`:CAS 循环 + 按前态分派(A.5);删 `connector::cancel()`。
   - wait op + `async_wait_disconnect`:改用 `peer_closed_`(A.6)。
   - `close_for_destruction`:drain 兜底补拆(A.7)。
   - construct/move/open/assign 的字段处理(A.9)。
2. **ibv 数据面 teardown**:加测试断言 `disconnect` 令 pending send/recv 以 `operation_aborted` 完成(无新代码)。
3. **ibv listener cancel**:核验现有实现 + 加测试。
4. **nd**:listener cancel 补桩;connector 删 cancel + disconnect 加 `connect_state_` 守卫(Windows 端)。
5. **文档**:CLAUDE.md "Unified Public API Surface" + README:155-167 订正 —— 列出 `listener::cancel()`、
   `connector::disconnect()`(任意线程、按状态自适应 teardown),明确**connector 无独立 cancel**、数据面无独立
   cancel;disconnect/cancel 后对象 terminal、不可复用。

## 测试

**已实现并在真实 RoCE(mlx5_0 / RoCE v2)上验证通过**:`tests/ibv/test_ibv_disconnect_cancel.cpp`
(用跨平台 `rdma_*` 别名 + `rdma/rdma.hpp`,故同时覆盖 ibv 与 rdma 两套接口面),三个 phase 全部从
**另一线程**调 `disconnect()` 以验证 MT-safe。用法:`test_ibv_disconnect_cancel <roce-ip> [port]`。

- **Phase A —— 在途 connect 中止(中间态 CAS bail + 跨线程 disconnect)**:server 收到 connect REQ 但
  **故意不 accept**,使 client 确定性地停在 `connecting`;worker 线程跑 `io.run()`,主线程 `disconnect()`。
  断言 connect 以 `operation_aborted` 完成。✅
- **Phase B —— 已建立连接的数据面 teardown(`connected->closed` flush + 跨线程)**:建立连接后 client 挂一个
  阻塞的 `async_recv`(server 不发);worker 线程跑 `io.run()`,主线程 `disconnect()`。断言 pending recv 以
  `operation_aborted` 完成。✅
- **Phase C —— `connecting->connected` 仲裁 soak(建立瞬间撞 disconnect)**:server 真实 accept(每条都会
  建立),client 在 disconnect 前 sleep 一个**扫过 CM 握手延迟(0~19.6ms)**的延时,使迭代落在竞态两侧 ——
  op 先赢(disconnect 随后见 `connected` 由其拆)与 disconnect 先赢(op 的 ESTABLISHED-CAS 失败 -> op 补拆,
  或 A.7 drain)。断言每个 connect 恰好完成一次且 ec ∈ {success, operation_aborted},`other=0`、`missing=0`,
  无崩溃/无 hang/无双重 teardown。150 次/轮,连跑 8 轮稳定 ~120 established / ~30 aborted(两侧每轮均命中)。✅

**回归基线(同样真机通过)**:`test_ibv_connector_listener`、`test_ibv_wait_disconnect`(connected teardown +
`peer_closed_` level-trigger)、`test_ibv_echo` / `_poll`、`tests/rdma/test_rdma_echo` / `_poll` —— event/poll
两模式、ibv/rdma 两套别名全过,正常路径无回归。

**仍待补(后续)**:
- `listener::cancel()` 对象取消的专用断言(`async_get_connection` 在途 -> `lis.cancel()` ->
  `operation_aborted`;断言 listener 之后仍可复用)—— 现有测试间接覆盖了 listener 生命周期,但无专测。
- nd 后端用例 Windows 端验证(随 nd 实现一并)。

## 开放问题(已拍板)

- **D4(已定)**:不在 `queue_pair` 加对象级 cancel/本地 flush;数据面对象级取消 = `connector::disconnect()`。
- **D3(已定)**:disconnect/cancel 后 connector terminal、不可复用(对端已 desync,本地 reset QP 无意义)——
  暴露 `connect_state_ == closed` + 写文档,由用户重建。
- **cancel 命名(已定)**:connector 不暴露 `cancel()`(会误导为 socket 式可复用),统一 `disconnect()`;
  仅 listener 保留 `cancel()`(确为可复用语义)。
- **MT-safe(已定)**:`disconnect()` 线程安全,用 `std::atomic<connect_state>` + CAS 仲裁,不用锁、不 post
  回 io_context(保持同步签名 commit dadc61c);残留契约仅"别并发析构"。
- **状态粒度(已定)**:采用细粒度 `connect_state_`(镜像 op 各阶段),推进用 CAS;评审知悉其对 disconnect 决策
  力等价于粗粒度 `{idle,connecting,connected,closed}`,细粒度取其"显式 + 早停 + 对称",代价是更多 CAS 点。
