# 取消计划 Stage 2 —— 控制面单操作取消

> 三篇分期计划之一。Stage 2 = **控制面**的**单操作级**(cancellation_slot)取消。
> 相关:`cancellation_stage1_object_plan.md`(对象级)、`cancellation_stage3_data_single_op_plan.md`(数据面单操作)、
> `../asio_cancellation_analysis.md`(asio 机制详解,§7 是本阶段的蓝本)。
>
> **依赖关系**:与 Stage 1 独立(可并行或先后),但本阶段的若干结论**建立在 Stage 1 已落地的事实之上**
> (`connect_state_` 状态机、`peer_closed_` latch、`disconnect()` 统一 teardown、`connector::cancel()` 已删)。
>
> **更新(Stage 1 已落地,见 `cancellation_stage1_object_plan.md` / commit 440543e)**:
> - `async_disconnect` 早已被**同步** `disconnect()` 取代(本阶段与它无关);`ibv_disconnect_op` 已删。
> - **`connector::cancel()` 已删除**,对象级 teardown 统一为 `disconnect()`(自适应 `connect_state_`,MT-safe);
>   `listener::cancel()` 保留。这直接改变了本阶段的两个论点(见下):
>   (a) 控制面 op 的 in-flight 取消现在通过 `disconnect()`(对象级)或本阶段的 per-op slot;
>   (b) **`async_wait_disconnect` 的 per-op 取消现在是"停掉 watcher 但保留连接"的唯一手段** ——
>   因为对象级只剩 `disconnect()`,而它会把整条连接拆掉。
> - connector 的 in-flight op 已挂在 `connect_state_` 原子状态机上(connect/accept op 逐阶段 CAS);本阶段新增
>   per-op 取消时**必须想清楚它与该状态机的交互**(新增 §"与 connect_state_ 的交互",本阶段最关键的新内容)。

## 目标与范围

让控制面异步操作接入 asio 的**单操作级取消**(cancellation_slot),从而 `cancel_after`、`co_spawn` 取消、
`awaitable_operators` 的 `||`、`parallel_group` 等惯用法能作用到**具体某一个**控制面操作上。

覆盖的操作:
- `connector::async_connect`
- `connector::async_accept`
- `listener::async_get_connection`(底层 `async_get_connection_request`)
- **`connector::async_wait_disconnect`**(当前缺 per-op 取消,**重点补这个**)。
- **不含** `disconnect()` —— 它是**同步**操作,不存在"等待中的异步 op"可取消(N/A)。

## 可行性结论:可行,且能完全照搬 asio

控制面的 op(connect/accept/get_connection/wait_disconnect)都派生自 `ibv_op_cm : asio::detail::reactor_op`
(ibv_op_cm.hpp:13),全部以 `reactor::read_op` 挂在 `impl.cm_channel_->fd` 上等 CM 事件。它们**正好停在
reactor 软件队列里**,满足 asio "执行前摘队列"的可取消前提(asio 分析 §2)。而且:

- 基类 `asio::detail::reactor_op` **自带** `cancellation_key_`(reactor_op.hpp:34,init 0 于 :56),
  `ibv_op_cm` 及所有控制面 op 都继承到,但目前**从未赋值**。
- asio 已提供 `epoll_reactor::cancel_ops_by_key`(epoll_reactor.ipp:373+),代码库目前**从未调用**。
- 代码库目前**没有任何** `get_associated_cancellation_slot` 调用(connector_service.hpp 已确认无)。

字段和 reactor 能力都齐了,**只差接线**。这正是 asio 分析 §10"给库作者的清单"的直接套用。

## 设计(ibv)

### 1. 取消处理器 functor

在 connector/listener service 内新增一个小 functor(放在 `start_*_op` 私有助手旁):
```cpp
struct cm_op_cancellation {
  asio::detail::reactor* reactor_;
  asio::detail::reactor::per_descriptor_data* reactor_data_;
  int fd_;
  void* key_;
  void operator()(asio::cancellation_type_t type) {
    if (!!(type & (asio::cancellation_type::terminal
                 | asio::cancellation_type::partial
                 | asio::cancellation_type::total)))
      reactor_->cancel_ops_by_key(fd_, *reactor_data_,
                                  asio::detail::reactor::read_op, key_);
  }
};
```
三档全接(与 socket 一致;摘等待项在 asio 侧无部分副作用)。

**关键:functor 只调 `cancel_ops_by_key`,绝不碰 `connect_state_` / `peer_closed_`。** 理由见 §"与
connect_state_ 的交互"——同一个 functor 既用于 connect/accept 也用于 wait_disconnect,而这两类对状态的期望
相反,不动状态对两者都正确。

### 2. 发起处接线

在每个 service `async_*` 里,**分配 op 之后、`start_*_op` / `reactor_.start_op` 之前**:
```cpp
auto slot = asio::get_associated_cancellation_slot(handler);
if (slot.is_connected()) {
  p.p->cancellation_key_ = p.p;                    // op 指针即 key
  slot.emplace<cm_op_cancellation>(
      &this->reactor_, &impl.cm_reactor_data_, impl.cm_channel_->fd, p.p);
}
```
插入点(按函数名定位,行号随 Stage 1 改动已位移):
- `ibv_connector_service::async_connect`:op 分配后、`start_connect_op` 前。
  - 注意:Stage 1 里 `start_connect_op` 在 arm 前会 `CAS(idle->addr_resolve)`;slot 接线放在 op 分配后、
    调用 `start_connect_op` 前即可,与该 CAS 不冲突(slot 只设 key,不动状态)。auto-open 失败的
    `post_immediate_completion` 分支无可取消项,跳过接线。
- `ibv_connector_service::async_accept`:op 分配后、`start_accept_op` 前(device-not-registered 的立即完成
  分支跳过)。
- `ibv_connector_service::async_wait_disconnect`(**新增,重点**):op 分配后、`reactor_.start_op(read_op,...)`
  前。注意它只有在 `!(peer_closed_ || connect_state_==closed) && is_open` 时才 arm(connector_service.hpp
  的立即完成判据);走 `post_immediate_completion` 的分支无可取消项,跳过接线即可。
- `ibv_listener_service::async_get_connection_request`:op 分配后、`start_get_connection_request_op` 前。

slot 必须在 service 层取(key = op 指针只在此处存在),不能放到公开类的 initiation lambda 里。

### 3. 无需基类改动

`cancellation_key_` 已在 `reactor_op`;所有控制面 op 的 op_type 统一为 `reactor::read_op`。

## 与 connect_state_ 的交互(本阶段最关键的新内容)

Stage 1 给 connect/accept op 装了逐阶段 CAS(`idle->addr_resolve->addr_route->connecting->connected`),
且 `disconnect()` 以 `connect_state_` 仲裁 teardown。**本阶段的 per-op 取消是另一条独立路径**:
`cancel_ops_by_key` 由 reactor 在锁内把 op 摘队列、以 `operation_aborted` 完成,**完全不经过我们的 CAS
逻辑**。所以必须确认它不破坏 Stage 1 的不变量。结论:**functor 不碰 `connect_state_`,对 connect/accept 与
wait_disconnect 两类都正确**,理由分述:

### connect / accept 的 per-op 取消(取消生效 -> 标记废弃 `closed`)

**设计决定**:取消生效后,connector 与 `disconnect()` 一样进入废弃态 `closed`,使"废弃"语义统一
(disconnect / 失败 connect / 单操作取消 三者都 -> `closed`)。实现**不放在 functor**(它在 connect/accept/
wait_disconnect 间共用),而是放在 **connect/accept op 的 `do_complete`:完成时若 `ec` 非成功(含
`operation_aborted`),就置 `connect_state_ = closed`**。

- **race-safe**:op 恰好只完成一次。`ec` 被设 <=> 它**没**走 ESTABLISHED 成功路径(成功路径清 `ec` + 置
  `connected`)。所以置 `closed` 时**绝不会覆盖一个真实的 `connected`** —— 两者互斥。
- **自然区分 op 类型**:wait_disconnect 是另一个 op 类型,其 `do_complete` **不碰** `connect_state_`,所以取消它
  之后状态仍是 `connected`、连接存活(见下)。无需给 functor 加参数,类型本身就分开了两种行为。
- cm_id 停在握手中途 -> connector terminal,用户销毁重建(QP 跟随)。`close_for_destruction` 的 drain 兜底:
  若 ESTABLISHED 曾竞态到达却没被处理,drain 发 graceful `rdma_disconnect`(Stage 1 A.7)。
- **取消 vs 完成 的竞态(asio 标准语义)**:`cancel_ops_by_key` 与 `do_perform` 抢同一把 descriptor 锁
  (epoll_reactor.ipp:373+ vs 784)——
  - op 先跑完 ESTABLISHED:`CAS(connecting->connected)` 成功、handler 以 **success** 完成,取消变 late no-op。
    **此时 connector 是 `connected`(一条真实连接),不是废弃** —— 用户拿到了连接,应当 `disconnect()` 它。
    即**"取消 -> 废弃"只在取消确实生效(handler 收到 `operation_aborted`)时成立**;这条 asio 语义无法绕过。
  - 取消先生效:op 报 aborted -> `do_complete` 置 `closed`;若 ESTABLISHED 竞态到达则 drain 兜底优雅拆。
  两种交错都无双重 `rdma_disconnect`、无泄漏。

### wait_disconnect 的 per-op 取消(连接处于 `connected`,仍存活)

- `cancel_ops_by_key` 摘掉 watcher、报 aborted,`connect_state_` **保持 `connected`**、`peer_closed_` 不变。
- **连接仍然存活、数据面 QP 仍可用** —— 这正是 `co_await (qp.async_recv || conn.async_wait_disconnect)` 里
  recv 先到时想要的:取消 watcher,但保留连接继续收发。若 functor 误把状态推到 `closed`,就会让后续
  `disconnect()` 走 `closed` no-op、不 flush,且语义上等于拆了连接 —— 所以 functor **绝不能**动状态。
- 这条也是 Stage 1 删掉 `connector::cancel()` 后的**新刚需**:对象级只剩 `disconnect()`(会拆连接),
  因此"停掉 wait_disconnect 但保留连接"现在**只能**靠本阶段的 per-op 取消。

### 取消后 `connect_state_` 处于什么状态(各 op 一览)

采用上面"取消生效 -> 标记 `closed`"的设计后,`closed` 统一表示"废弃":

| 被取消的 op | 取消后 `connect_state_` | 对象可用性 / 后续 |
|---|---|---|
| `async_connect` | **`closed`**(取消生效时,由 op `do_complete` 置);**若取消输给 establish 竞态则是 `connected`**(handler 收到 success,是真实连接,需 `disconnect()`)| connector **terminal**(cm_id 半握手);重发被守卫拒(`ext_connector_terminal`);销毁重建,QP 跟随 |
| `async_accept` | 同 `async_connect`(`closed`,或竞态输了为 `connected`)| server connector **terminal**;且对端可能已见 connect-then-disconnect(REP 已发)|
| `async_get_connection` | **不涉及** connector 的 `connect_state_`(取消时通常尚未产出 connector);**listener 的监听 cm_id 仍 `LISTEN`** | **listener 可复用**,可再 `async_get_connection` |
| `async_wait_disconnect` | **保持 `connected`**;`peer_closed_` 不变(其 op 类型的 `do_complete` 不碰状态)| 连接仍存活;connector + QP 仍可用;可重新 arm wait / 继续收发 / 之后 `disconnect()` |

**复用守卫用 `!= idle`(而非 `== closed`)。**
取消生效后 connect/accept 已是 `closed`,所以 `== closed` 能覆盖"取消后再 connect";但仍建议用更稳的
**`connect_state_ != idle`**(`idle` 才是唯一能发起新 connect 的态),它额外还挡住两种误用:在 `connected` 上重发
connect、在 in-flight(`connecting` 等)时并发第二个 connect —— 一律 early-exit 抛 `ext_connector_terminal`。
`async_accept` 同理用 `!= idle` -> `ext_connector_terminal`。
- 该守卫**不影响** `async_wait_disconnect` 的重新 arm:wait 在 `connected` 态、走的是 `peer_closed_ ||
  connect_state_==closed` 判据,与"connect 复用守卫"无关。
- **实现现状**:Stage 1 落地的守卫目前是 `== closed`。本阶段需:(a) 给 connect/accept op 的 `do_complete`
  加"非成功完成置 `closed`"(使单操作取消也进入废弃态);(b) 把守卫放宽到 `!= idle`;(c) 给 `async_accept`
  补同款守卫。

## 要点与边界

- **connect 多阶段不是问题**:resolve_addr -> route -> connect 是**同一个** reactor_op 用 `stage_` +
  `not_done` 自我重挂(ibv_op_connect.hpp:34 的 `stage_t`、:117-147 的 `do_perform`/`do_process`),fd 上始终是
  这一个 op、一个 key,一次 `cancel_ops_by_key` 不论停在哪个阶段都能整条取消。
- **disconnect 不接(N/A)**:`disconnect()` 是同步操作,没有"等待中的异步 op"可取消。
- **wait_disconnect 必须接(本阶段重点)**:它是个长期 armed 在 cm fd 上的 reactor_op,当前无 per-op slot。
  后果是 `awaitable_operators` `||` 与 `cancel_after` **作用不到它**:`co_await (qp.async_recv ||
  conn.async_wait_disconnect)` 里 recv 先返回时,`||` 要取消 watcher,但 watcher 忽略 cancellation_slot
  -> 取消变 no-op -> **整个 `||` 挂住**直到真断开。补上 slot 后这些惯用法才生效,且(见上)这是"保留连接地
  停掉 watcher"的唯一手段。
- **MT-safe 一致性**:cancellation_slot 的 emit 可能来自任意线程;`cancel_ops_by_key` 与 Stage 1 的
  `disconnect()`/`cancel_ops` 一样受 reactor descriptor 锁保护,二者互斥安全。functor 不碰 `connect_state_`,
  故不引入新的跨线程字段竞争。
- **terminal 语义**:取消让 asio 侧以 aborted 结束,内核 cm_id 仍停在握手中途;connector 视为**不可复用**
  (`connect_state_` 停在中间态会让重发 connect 失败),由用户 disconnect/销毁。
- **late-emit 安全**:emit 晚于完成时,`cancel_ops_by_key` 找不到该 key(op 已离队)-> no-op(asio 分析 §8)。
- **粒度说明(管理预期)**:一个 connector/listener 通常**同时只有一个**在途控制面操作,所以"单操作取消"在
  实际效果上常常≈对象级。本阶段的**真正价值是组合性** —— `cancel_after` / `co_spawn` 取消 / `||` 这些惯用法
  **需要 slot 才能工作**(它们不会去调 `disconnect()`),且对 wait_disconnect 还能"保留连接地取消"。

## nd(镜像,Windows 核实)

- op 是 `nd_op_base : asio::detail::operation`(OVERLAPPED,nd_op_base.hpp:12,21)。单操作取消 =
  `CancelIoEx(handle, overlapped /* 即 op 指针 */)`,与 win_iocp_socket_service_base 的
  `iocp_op_cancellation`(win_iocp_socket_service_base.hpp:648-685)同构。
- **障碍**:service 目前**不保留**在途 op 指针(`start_*_op` 调完 `on_pending` 后即 `p.v=p.p=0`)。
  支持按操作的 `CancelIoEx` 需把在途 op 的 OVERLAPPED 存进 `implementation_type`。若不想存,只能
  `CancelOverlappedRequests()`(整接口),那退化为对象级。
- **与 nd `connect_state_` 的交互**:nd 端同样要落 Stage 1 的 `connect_state_` 守卫(见 stage1 文档 nd 小节)。
  规则与 ibv 一致:connect/accept op 完成处置废弃态 `closed`、复用守卫 `!= idle`、wait_disconnect 取消后连接存活。
- **QP 复用对齐 rdma_cm(取交集,已定)**:ND 的 QP 由 `queue_pair` 持有(connector 死后仍存活),物理上可复用;
  但**统一契约取交集,对齐 ibv 的 terminal 语义** —— cancel/disconnect 后该 qp 也视为 terminal、需重建。
  所以 **ND 要主动把被拆连接所绑的 qp 标 terminal**(后续 `async_send/recv` 或再次绑定时报终态错误),
  **不**暴露"qp 重绑新 connector"这条 ibv 没有的路径。理由:抽象取能力交集 -> 行为跨平台一致。

## 实施步骤

1. ibv:加 `cm_op_cancellation` functor(只调 `cancel_ops_by_key`,**不碰状态**)。
2. ibv:`async_connect` / `async_accept` / `async_get_connection_request` / **`async_wait_disconnect`** 四处接线
   (取 slot + 设 `cancellation_key_ = op`)。
3. ibv:**connect/accept op 的 `do_complete` 加"非成功完成(`ec` 被设)-> 置 `connect_state_ = closed`"**
   (使单操作取消生效后进入废弃态,与 disconnect 统一;race-safe,见上)。wait_disconnect op 不改。
4. ibv:**复用守卫放宽到 `connect_state_ != idle` -> `ext_connector_terminal`**,并给 `async_accept` 补同款守卫
   (当前只有 `async_connect` 且为 `== closed`)。
5. 测试(见下)。
6. nd:在 impl 里保留在途 op 指针 + 接 `CancelIoEx(handle, op)`(或先用 `CancelOverlappedRequests` 兜底);
   含 `nd_wait_disconnect_op`;nd 端同样在 op 完成处置废弃态 + `!= idle` 守卫。
7. 文档:在 Cancel 说明里补"控制面(含 `async_wait_disconnect`)支持 per-op 取消(cancel_after / co_spawn / ||)",
   并写明"取消生效后 connect/accept 的 connector 进入废弃态(`ext_connector_terminal`),wait_disconnect 的
   取消保留连接"。

## 测试

> 复用 Stage 1 已建立的真机测试骨架(`tests/ibv/test_ibv_disconnect_cancel.cpp` 的 server/client + worker 线程
> 模式;`test_ibv_wait_disconnect.cpp` 的单进程 server+client)。

- `tests/ibv/`(RoCE):
  - **connect cancel_after**:`cancel_after(conn.async_connect(unaccepted_server_ep, pd, use_awaitable), 1s)`
    (server 收 REQ 不 accept,client 卡 `connecting`)-> ~1s 后 `operation_aborted`;**断言取消后 connector 进入
    废弃态**:重发 `async_connect` 以 **`ext_connector_terminal`** 被拒(`!= idle` 守卫),需销毁重建。
  - **`||` 竞速**:`co_await (conn.async_connect(...) || timer.async_wait(1s))`,timer 先到 -> connect 被取消
    (aborted)。
  - **listener**:`cancel_after(listener.async_get_connection(...), 1s)`(无人连)-> aborted;断言 listener
    之后仍可 `async_get_connection`(可复用,与 connector terminal 对比)。
  - **`async_wait_disconnect` per-op 取消(重点)**:
    - `cancel_after(conn.async_wait_disconnect, 1s)` 在无断开时 ~1s 以 `operation_aborted` 完成;
    - `co_await (qp.async_recv || conn.async_wait_disconnect)`:recv 先到时 `||` **立即返回**(watcher 被 per-op
      取消,不再挂住),**且断言取消后连接仍存活**:随后还能在同一 qp 上成功 `async_send`/`async_recv`
      (验证 functor 没动 `connect_state_`、没拆连接)。
- `tests/rdma/`:跨平台版(仅 `rdma_*` 别名)。
- nd 用例 Windows 端验证。

## 开放问题

- nd 是否值得为 per-op `CancelIoEx` 在 impl 里保留 op 指针,还是先用 `CancelOverlappedRequests`(对象级)
  兜底、把 nd 的单操作取消标为后续?建议:ibv 先完整落地;nd 先用整接口取消兜底,真正 per-op 留到有
  Windows 验证条件时。
- (已定)**取消生效后是否把 connector 标为废弃 `closed`?** 是 —— 与 `disconnect()` 统一。但**不在 functor 里
  写**(它在三类 op 间共用,且 wait_disconnect 取消后连接须存活、不能置 closed);改在 **connect/accept op 的
  `do_complete`:非成功完成 -> 置 `closed`**。这样按 op 类型天然区分(wait_disconnect 的完成不碰状态),且
  race-safe(`ec` 被设 <=> 未 established,绝不覆盖真实 `connected`)。复用守卫随之用 `!= idle`。
