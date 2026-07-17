# ibv queue_pair 生命周期与 pre-post receive 重构计划

日期：2026-07-17

状态：下一期专项，尚未进入实现。本计划不属于当前 `asio_on_rdma_defect_report_and_fix_plan.md` 的 RDMA-006；RDMA-006 本期只实现 async move-accept operation。

## 目标

把 ibv backend 从“connector 通过 `rdma_create_qp()` 创建并拥有 native QP，`ibv_queue_pair` 只借用指针”重构为“`ibv_queue_pair` 通过 `ibv_create_qp()` 创建并拥有 native QP，connector 只负责 rdma_cm 协商和连接状态”。

目标能力：

- event/poll 两种 `ibv_queue_pair::bind` 成功后即存在 native QP；
- QP 在连接前可以向硬件 RQ pre-post receive；
- accept/connect 使用 rdma_cm 的 self-created QP 模式；
- send/read/write 不承诺连接前 pre-post；
- QP move 后 native ownership、CQ route 和 pending WR completion 保持正确；
- disconnect、取消和建连失败能够 flush pending receive，而不泄漏 handler 或产生悬空 `wr_id`。

本计划中的 pre-post receive 指 native hardware pre-post：`ibv_post_recv()` 在 `rdma_connect()` / `rdma_accept()` 之前成功把 Receive WQE 提交到 QP 的 RQ。它不是在库内保存 buffers，等连接成功后再补做 native post。

## 当前实现基线

当前 ibv QP：

1. `bind(io_context/cq)` 只保存 device、CQ 和 config，`impl.qp_` 仍为空；
2. `connector.async_connect/async_accept(qp, ...)` 把 `make_create_qp_fn()` 传入 service；
3. service 在 connect/accept initiation 内调用 `rdma_create_qp(cm_id, pd, attr)`；
4. native QP 由 `cm_id`/connector 拥有，`ibv_queue_pair` 只保存 non-owning pointer；
5. `rdma_create_qp()` 与 `rdma_connect/rdma_accept()` 之间没有用户可见的 pre-post receive 窗口。

## 目标公共语义

```cpp
ibv_queue_pair qp(io);  // event mode
ibv_queue_pair qp(cq);  // poll mode
```

成功后统一满足：

```text
is_bound() == true
native_handle() != nullptr
QP 由 queue_pair 按值/RAII 拥有
QP 至少已进入允许 ibv_post_recv 的状态
```

服务端高级流程：

```cpp
ibv_queue_pair qp(cq);
qp.async_recv(buffer, recv_token);            // native pre-post
conn.async_accept(std::move(qp), reply, token); // completion: (ec, qp)
```

当前 RDMA-006 的 convenience move-accept 继续保留：

```cpp
conn.async_accept(cq, reply, token); // 内部创建 QP；不提供 pre-post 窗口
```

本专项新增的 rvalue-QP overload 用于需要严格 pre-post receive 的高级路径；accept operation 按值持有 QP，完成时再 move 给 handler。

## pre-post receive 公共语义

### operation 能力矩阵

| QP 阶段 | native QP | `async_recv` | `async_send/read/write` |
|---|---:|---:|---:|
| default/unbound | 无 | 不允许 | 不允许 |
| bound/INIT，尚未关联连接 | 有 | 允许 pre-post | 不允许 |
| connecting/accepting | 有 | 允许继续补 RQ | 不承诺 |
| connected/RTS | 有 | 允许 | 允许 |
| error/draining/closed | 可能有 | 拒绝新 post | 拒绝新 post |

本专项只把 receive 定义为连接前可用 operation。Send、RDMA Read、RDMA Write 都进入 SQ，需要可发送的 QP/remote state；即使某个 provider 偶然接受连接前 SQ WQE，也不构成公共 API 保证。

### 严格时序

服务端高级路径必须满足：

```text
ibv_create_qp
    ↓
QP → INIT
    ↓
async_recv initiation 内调用 ibv_post_recv
    ↓
ibv_post_recv 返回成功
    ↓
rdma_init_qp_attr / QP connection transitions
    ↓
rdma_accept
```

客户端对应为：

```text
ibv_create_qp → INIT → ibv_post_recv 成功 → rdma_connect
```

只满足“`async_accept()` 返回后、accept handler 执行前调用 recv”不算严格 pre-post，因为 `rdma_accept()` 已提交，对端可能先推进并发送数据。

### `async_recv` initiation 与完成语义

- `async_recv` 必须在 initiation 执行期间同步调用 `ibv_post_recv()`；使用 deferred token 时，以 deferred operation 真正启动的时刻为准；
- `ibv_post_recv()` 返回 0 只表示 Receive WQE 已进入 RQ，不触发成功 callback；
- receive handler 只在数据到达产生 CQE，或 QP error/teardown 产生 flush CQE时完成；
- native post 同步失败不得伪造成已 pre-post，必须通过所选 event/poll completion route 报错且 `bytes_transferred == 0`；
- event mode 的 receive completion 由 data `io_context` 的共享 CQ poller 驱动；poll mode 只在用户调用 `cq.poll()/poll_one()` 时完成；
- accept/connect 是 control-plane operation，其 handler 与 receive handler 可以运行在不同 execution context/thread。

### native post 失败与是否继续建连

官方 verbs 示例会检查 `ibv_post_recv()` 的同步返回值，再决定是否调用 `rdma_accept/rdma_connect`。现有纯 Asio `async_recv` 只通过 completion token 暴露结果，因此调用者在成功 CQE 前无法区分“WQE 已成功提交并等待数据”和“同步 post 失败、错误 callback 尚未调度”。

这是本专项必须在 L0 冻结的 API 决策，不能静默跳过。候选方案：

1. 为 pre-connect receive 提供显式 submission-result API：同步返回 native submission `error_code`，handler 只负责已成功提交 WR 的最终 CQE；
2. 让 rvalue-QP accept/connect operation 能读取 QP 上记录的 pre-post submission failure，并 fail-closed，不调用 native accept/connect；
3. 提供组合式 accept/connect overload，由库在 native CM call 前创建 receive op、检查所有 `ibv_post_recv` 返回值，再推进 CM。

默认推荐先原型方案 2：不新增一套与 Asio completion token 冲突的 public recv API；仅在 pre-connected QP 上记录第一条同步 post failure，后续 accept/connect fail-closed。需要明确多个 pre-post 中部分成功、后续失败时，已成功 WQE 的 flush 与各 handler completion 语义。原型若证明状态耦合过强，再退回方案 1。

### buffer、MR 与容量契约

- payload buffer 和 MR 必须从 `async_recv` initiation 一直有效到 receive handler；accept/connect handler 完成不代表 receive buffer 可以复用；
- QP accept/connect 失败、取消或 disconnect 后，buffer 仍需保持到对应 flush completion；
- CQ、device/PD 和 MR 必须比所有 pending receive op 活得更久；
- 每个 Receive WQE 的 SGE 数、单段长度和总长度先经过公共校验；
- RQ 满、SGE 超限、无效 lkey/MR 或 provider 拒绝必须形成可观察错误，不能继续把该 WQE 计入“已准备 receive”；
- 支持 pre-post 0、1、多个 receive；RQ depth/backpressure 由 effective config 限制，不在库内创建无界软件队列。

### QP move 与 pending receive

native Receive WQE 的 `wr_id` 指向 verbs op，CQ 也独立于 C++ QP 对象地址，因此在 native post 成功后 move QP wrapper 可以成立，但必须满足：

- native QP RAII ownership 被完整 move，native handle 本身不变；
- pending recv op 不保存指向 move 前 `implementation_type` 的裸指针；
- completion route、device/CQ/config 与 connection association generation 一起 move；
- moved-from QP 变成完整空状态；
- move construction 用于把 QP 交给 accept/connect operation；move assignment 覆盖一个仍有 native QP 或 pending WR 的目标对象必须禁止，或先完成明确的 error/drain 流程。

成功的 rvalue accept/connect operation 按值持有 QP，不需要 `unique_ptr/shared_ptr`；完成时再 move 给用户。

### completion ordering

连接建立后，对端可能立即发送，因此 pre-post receive CQE 可能在本地 accept/connect handler 之前被 data executor/poll thread 观察到。公共 API 不保证以下两类 handler 的顺序：

```text
accept/connect handler
receive handler
```

只保证各 handler 恰好完成一次、各自在其 completion route 上执行。应用若需要“先处理连接成功，再处理首包”，必须在上层做状态协调，库不能靠延迟 CQ polling 隐式保证。

## 官方 rdma_cm 行为依据

rdma-core 官方示例采用的顺序与本计划一致：

- `rdma_server.c`：`rdma_post_recv()` 后调用 `rdma_accept()`；
- `rping.c` 服务端：setup QP/buffer，`ibv_post_recv()`，再 `rping_accept()`；
- `rping.c` 客户端：setup QP/buffer，`ibv_post_recv()`，再 `rdma_connect()`；
- `rping -q`：使用 `ibv_create_qp()`、手工转 INIT、通过 `conn_param.qp_num` 告知 CMA，并自行完成 QP 状态转换/`rdma_establish()`。

因此本计划把“native post 成功发生在 CM call 之前”作为验收事实，不接受仅凭 accept/connect callback 时序推断 pre-post 已成立。

## rdma_cm self-created QP 模式

官方 rdma-core `rping -q` 证明 rdma_cm 支持用户自行创建 QP。需要实现的不是把 `rdma_create_qp` 简单替换为 `ibv_create_qp`，而是接管以下职责：

1. `ibv_create_qp(pd, init_attr)`；
2. 使用 `ibv_modify_qp` 把 QP 转入 INIT；
3. 在 `rdma_conn_param.qp_num` 中传递本地 QP number；
4. 使用 `rdma_init_qp_attr()` 取得与 cm_id/route 对应的 QP 属性和 mask；
5. 由 backend 执行 INIT/RTR/RTS 转换；
6. 主动端按 self-created QP 流程调用 `rdma_establish()`；
7. disconnect/cancel 时自行把 QP 转入 ERROR 并收敛 flush CQE；
8. 使用 `ibv_destroy_qp()`，不再依赖 connector/cm_id 销毁 QP。

## 前置决策与能力补齐

### 1. port/device 元数据

QP 转 INIT 需要 concrete port。当前 `ibv_device_t` 保存 context、PD、device attr 和 IP 地址，但没有明确保存 address 对应的 `port_num`/GID metadata。

实现前必须：

- discovery 为每个可用地址记录 `port_num`，RoCE 时同时记录 GID/index 等必要信息；
- `use_device`/effective device 能提供创建 QP 所需的 concrete port；
- QP 与后续 cm_id attach 时验证 `cm_id->verbs == device->context_` 且 route port 兼容；
- 不允许把在一个 adapter/port 上创建的 QP 用于另一个 child cm_id。

如果现有 device abstraction 无法唯一确定 port，本专项先补齐 device/port identity，再改变 QP 创建时机。

### 2. ownership

新增 native QP RAII holder，例如 `unique_ibv_qp_ptr`，放入 `ibv_verbs_service::implementation_type`。删除“connector owns、queue_pair borrows”的语义。

move 构造/赋值必须转移：

- native QP ownership；
- device/CQ/config；
- completion route；
- 与 cm_id/connection generation 的关联信息。

moved-from QP 回到完整空状态，不保留 raw native pointer。

### 3. QP 与 connector 的关联

QP 在 bind 时可独立创建，但 accept/connect 时仍必须验证它与当前 cm_id、device、port space 和 route 兼容。不要在 connector 中保存指向可移动 QP implementation 的裸指针。

建议使用按值 association metadata/generation；accept operation 接收并按值持有 rvalue QP。

### 4. 内部 QP phase

不能继续只用 `impl.qp_ != nullptr` 推断所有 operation 是否可用。建议为 self-created QP 维护最小内部 phase：

```text
unbound
  ↓ bind/create/INIT
recv_ready
  ↓ connect/accept initiation
connecting
  ↓ established/RTS
connected
  ↓ failure/disconnect
error → draining → closed
```

状态只服务于 library guard 和 teardown，不伪装成完整硬件状态镜像：

- `async_recv` 允许 `recv_ready/connecting/connected`；
- `async_send/read/write` 只允许 `connected`；
- `error/draining/closed` 拒绝新 post；
- CM state 与 QP phase 的转换需要同一套 race arbitration，不能一个普通 bool 分别更新；
- 如果 data post 与 disconnect 可并发，phase 至少使用原子状态或在已有 connector/QP 同步边界内串行化；
- native post 返回的 provider error 仍是最终事实，phase guard 只防明显非法调用和空 handle。

### 5. CQ terminal failure 与 pending WR 收敛

本专项同时接管 RDMA-010/RDMA-021 的最终修复：`ibv_req_notify_cq()` 失败、`ibv_poll_cq()` 返回负数、`IBV_EVENT_CQ_ERR` 和 `IBV_EVENT_DEVICE_FATAL` 都不能再由 CQ service 单独遍历软件 pending list 并立即执行用户 handler。2026-07-17 已撤回当前分支中这个临时 `fail_pending()` 原型；原因是它只等待并发 `ibv_post_*` 调用退出，没有证明已提交 WR、DMA 和未来 CQE 已经 quiesce。

必须区分四类事件：

- `ibv_poll_cq() > 0` 且 `wc.status != IBV_WC_SUCCESS`：poll 成功取得单个 WR 的失败完成，按正常 CQE 路径解析 `wr_id` 并完成该 operation；
- `ibv_req_notify_cq()` 失败：只证明 notification arm 失败，不自动证明 CQ 或全部 WR 失败；如果 CQ 仍可 poll，优先进入有界 polling/backoff，而不是 fail-all；
- `ibv_poll_cq() < 0`：CQ progress 已不可信，上层必须把该 CQ 视为 terminal failed，但不能据此假定所有关联 WR 已停止；
- verbs async event：`IBV_EVENT_CQ_ERR` 只直接判定对应 CQ 不可用，`IBV_EVENT_DEVICE_FATAL` 扩大到同一 device/context；二者都要进入统一的 QP teardown 协调流程。

#### ownership 与追踪边界

- event-mode shared CQ 必须维护“当前绑定 QP”的可并发安全注册表；poll-mode CQ 也必须能定位其关联 QP，但 completion 仍由用户 poll 驱动；
- pending operation 应按 QP 归属追踪，CQ 只能聚合关联 QP，不能用一个无归属的全局 pending list 代替 QP 生命周期；
- software operation 至少区分 `submitted/native-outstanding`、`native-quiesced/handler-pending`、`completed`，不得把“移出 pending list”当成 native WR 已停止；
- post 与 failure/close 必须有同一个同步边界：进入 `error/draining` 后拒绝新 post，并等待已经进入 native post 临界区的提交返回；这一步只关闭提交 race，不是 quiescence 证明；
- operation、`wr_id` token、payload buffer 和 MR 必须一直有效到 native quiescence 且 handler 已按 completion route 交付。若采用 logical completion 与 native reclamation 分离的双生命周期设计，也不能在用户 callback 后继续 DMA 用户 buffer，除非 API 明确提供内部 copy/独立 ownership。

#### terminal recovery 顺序

```text
CQ/notify/device terminal signal
    ↓
原子记录首个 failure cause，停止 notification re-arm
    ↓
标记所有受影响 QP 为 error/draining，拒绝新 post
    ↓
等待正在执行的 ibv_post_* 临界区退出
    ↓
逐 QP 转 ERROR / disconnect；仍可 poll 时 drain 正常与 WR_FLUSH_ERR CQE
    ↓
CQ 已不可 poll 时，按 provider 合同销毁/隔离 QP，并证明不会再 DMA 或产生可见 CQE
    ↓
处理 shared CQ 中可能晚到或属于旧 generation 的 CQE
    ↓
将仍未由真实 CQE 完成的 operation 标记为失败，bytes_transferred = 0
    ↓
通过原 event/poll completion domain 恰好完成一次 handler
    ↓
所有关联 QP 解除注册后才能销毁 CQ
```

不能仅依赖 `active_posts == 0`、`ibv_destroy_cq()` 或一次空 poll 作为 quiescence 证明。`ibv_destroy_cq()` 在仍有关联 QP 时会失败，也不会替库执行 callback。shared CQ 必须给 QP/operation 增加 generation 或等价 token，防止旧 CQE 在对象地址复用后命中新 operation。

#### notification arm 失败策略

`ibv_req_notify_cq()` 失败与 `ibv_poll_cq() < 0` 不应共用一个无条件 `fail_pending()`：

1. 先同步 drain CQ；
2. 将错误分类为可重试、notification path permanent failure、device/CQ terminal failure；
3. 可重试错误使用有界 retry/backoff，不能在 `io_context` 中无间隔自旋；
4. notification 永久失败但 CQ 可 poll 时，切换到显式 polling fallback，并保证 scheduler 公平性和停止条件；
5. 只有确认 CQ/QP terminal 时才进入上述 teardown；pending handler 仍必须等 native quiescence。

#### 错误与 callback 语义

- 用户 disconnect/cancel 引起的 flush 映射为 `asio::error::operation_aborted`；
- CQ poll、notification permanent failure或 `IBV_EVENT_CQ_ERR` 导致的未完成 operation 使用稳定的 I/O/service failure error，精确公共错误码在 L0 冻结；
- `IBV_EVENT_DEVICE_FATAL` 保留 device-fatal 诊断信息，并使同一 context 下所有受影响 QP 收敛；
- 任一失败 completion 都返回 `bytes_transferred == 0`，除非已经取得真实成功 CQE；
- event mode 在原 data `io_context` 上执行 handler；poll mode 不得偷偷借用其他 executor，必须由 CQ 的显式 failure-drain/poll 契约驱动；
- handler 必须恰好一次，且在 handler 可见时 buffer/MR 已经可以按 API 契约安全释放或复用。

生产实现依据：UCX 对 `ibv_poll_cq() < 0` 采用 process-fatal，不尝试就地完成 pending；SPDK 将 CQ 错误升级到 qpair disconnect，并显式等待 shared CQ 中旧 WC 收敛，避免 `wr_id` 指向已释放 request 的 UAF；libfabric verbs provider 也把 poll failure 与单个 `wc.status` failure 分开处理。因此本计划只借鉴 pending tracking，不采用“poll 负数即立即 callback”的语义。

## 被动端流程

```text
listener 收到 CONNECT_REQUEST
    ↓
用户取得 connector/private data
    ↓
已有 self-created QP，RQ 已 pre-post receive
    ↓
验证 QP device/port 与 child cm_id 匹配
    ↓
rdma_init_qp_attr + ibv_modify_qp
    ↓
conn_param.qp_num = qp->qp_num
    ↓
rdma_accept
    ↓
等待 ESTABLISHED
    ↓
accept handler(ec, move(qp))
```

必须有真机测试证明 Receive WQE 在 `rdma_accept()` 之前已经提交，并能接收客户端连接建立后立即发送的第一条消息。

## 主动端流程

主动端必须覆盖 self-created QP 的完整 rdma_cm 时序：

```text
resolve addr/route
    ↓
验证 self-created QP 与 resolved cm_id 匹配
    ↓
conn_param.qp_num = qp->qp_num
    ↓
rdma_connect
    ↓
按 rdma_init_qp_attr 结果转换 QP
    ↓
rdma_establish（需要时）
    ↓
connect completion
```

具体转换顺序以 rdma-core `rping -q`、目标 rdma-core 版本和 provider 实测为准，不凭单一 provider 行为简化。主动端 pre-post receive 也只允许在 QP 已进入可 post RQ 的状态之后。

## 失败、取消与销毁

这是本专项的 correctness gate，不允许只完成 happy path。

- `ibv_create_qp` 或 INIT 失败：bind 失败并回滚为空 QP；
- pre-post 同步失败：错误进入对应 event/poll completion route，不继续假定 RQ 已准备好；
- connect/accept 失败：QP 进入 ERROR，所有已提交 WR 最终完成；
- disconnect/cancel：先阻止新 post，再转 ERROR、drain flush CQE，最后销毁 native QP；
- pre-post receive 的 flush completion 映射为 `asio::error::operation_aborted`；
- QP 销毁前不得遗失仍由 CQE `wr_id` 引用的 op；
- accept/connect handler 与 data-plane flush handler 的先后顺序不作保证，但每个 handler 必须恰好完成一次；
- poll mode 下 flush handler 仍由用户 `cq.poll()` 驱动；event mode 由共享 CQ poller 驱动。
- CQ/notification/device terminal failure 必须执行“标记失败 → QP ERROR/销毁并证明 native quiescence → 处理 late CQE → callback”的顺序，禁止恢复已撤回的就地 `fail_pending()`。

需要明确 close/drain 契约：如果同步析构无法安全等待 pending CQE，则提供显式 drain/close 阶段或规定 connector teardown 持有 QP 到 flush 收敛，不能在析构中直接丢弃 pending op。

## 不在本专项

- ND QP 生命周期重构；
- SRQ；
- 连接前 pre-post send/read/write；
- unsignaled/batching；
- data-plane per-WR cancellation 新语义；
- 修改当前 RDMA-006 move-accept 的本期交付范围。

## 实施阶段

### L0：原型与决策冻结

- 用独立 prototype 验证目标 provider 的 `ibv_create_qp + qp_num + rdma_init_qp_attr + rdma_establish`；
- 验证 InfiniBand/RoCE 目标环境的 port/GID metadata；
- 记录主动/被动端精确状态转换和 teardown 顺序；
- 冻结 advanced rvalue-QP accept/connect API。
- 冻结 pre-post native submission failure 的交付方案，明确失败时是否 fail-closed 阻止 CM call；
- 冻结 CQ terminal failure error code、notification fallback、poll-mode failure-drain API，以及 QP/CQ generation token 方案；
- 用 provider prototype 验证 QP 转 ERROR、flush CQE、`ibv_destroy_qp` 返回与 DMA/CQE quiescence 的精确关系；不能只凭通用 RAII 假设；
- 用可记录 native 调用序列的 hook 证明 `ibv_post_recv(success)` 先于 `rdma_accept/rdma_connect`。

出口：server/client self-created QP smoke 均能连接；pre-post receive 在 accept/connect 前成功；失败路径不会遗失 WR completion。

### L1：QP RAII 与 bind-time native creation

- 新增 QP RAII ownership；
- 补 device port identity；
- event/poll bind 创建 QP 并转入允许 post receive 的状态；
- move/default/bind failure 单测。

出口：bind 成功即 `native_handle()!=nullptr`；moved-from/失败对象为空；尚未接入正式 connect/accept。

### L2：被动端 self-created QP

- 改造 accept service，不再调用 `rdma_create_qp`；
- 增加 rvalue-QP accept overload；
- 实现 qp_num、QP 状态转换、成功/拒绝/取消；
- event/poll pre-post receive 集成测试。

出口：服务端严格在 native accept 前 post receive，立即首包不依赖 RNR 补救。

### L3：主动端 self-created QP

- 改造 connect 多阶段 op；
- 实现 qp_num、状态转换、`rdma_establish`；
- 覆盖 resolve/connect failure 和 cancellation。

出口：client/server 全部不再使用 `rdma_create_qp`，event/poll echo/read/write 回归通过。

### L4：disconnect、flush 与压力验收

- QP ERROR/flush/drain；
- CQ 维护 bound-QP registry，QP 维护 pending operation/generation；
- 实现 notification arm fallback，以及 poll/CQ/device terminal failure 的 coordinated teardown；
- 只有 native quiescence 后才把残余 pending operation 通过原 completion domain 交付错误；
- accept/connect race、disconnect race、move、重复连接拒绝；
- hardware smoke、soak、并发 CQ 压力测试；
- 更新 queue_pair semantics 与 ownership 文档。

出口：ASan/UBSan 可运行部分无生命周期错误；真机 stress 无 handler 泄漏、double completion 或永久 pending；旧 `rdma_create_qp` 路径完全删除后再结束专项。

## 测试矩阵

- event / poll；
- server / client；
- IPv4 / IPv6（支持时）；
- pre-post 0、1、多个 receive；
- eager callback、`deferred`、`use_awaitable`：真正 initiation 前不得 native post，真正 initiation 时必须完成 native submission；
- native 调用顺序：`ibv_create_qp → INIT → ibv_post_recv → rdma_accept/rdma_connect`；
- `ibv_post_recv` 成功后，在没有 CQE 时 receive handler 不得提前完成；
- `ibv_post_recv` 同步失败时不得把该 WR 计入 prepared receive，按 L0 决策验证是否阻止 CM call；
- RQ 满：已有成功 WQE 与失败 WQE 的 handler 各自恰好完成一次；
- 对端连接完成后立即发送首包，服务端使用预投递 WQE 接收；记录 RNR/retry 指标，不能用重试掩盖调用顺序错误；
- accept/connect 成功、同步失败、异步失败、取消；
- accept/connect 失败或取消时，所有成功 pre-post 的 receive 最终以 flush/`operation_aborted` 收敛；
- accept/connect handler 与首个 receive handler 两种可见顺序，应用层协调测试不得假设固定先后；
- disconnect 时 pending recv/send；
- QP pre-post 后 move construct、move 进入 accept/connect op、完成后 move 返回；native handle 和 CQ route 不变；
- moved-from QP 不保留 ownership；move-assign 覆盖 live/pending 目标按最终契约被拒绝或先 drain；
- receive payload/MR 在 accept handler 后继续有效，直到 receive/flush handler；
- CQ 满、RQ 满、错误 MR/SGE；
- `ibv_req_notify_cq` 可重试失败、永久失败与 fallback polling；fallback 不 busy-loop、不饿死 timer/CM task；
- `ibv_poll_cq` 首次/连续返回负数：停止新 post，关联 QP 全部进入 teardown，handler 不提前释放仍可能被 DMA 的 buffer；
- `IBV_EVENT_CQ_ERR` 只影响关联该 CQ 的 QP，`IBV_EVENT_DEVICE_FATAL` 扩展到同 device/context；
- failure 与并发 post、disconnect、QP move、CQ shutdown 竞争；每个 operation 恰好完成一次；
- shared CQ 晚到 CQE、旧 QP generation 与 operation 地址复用；不得 UAF、误完成新 operation 或 double completion；
- CQ terminal 且无法继续 poll 时，provider-specific teardown 仍能证明 native quiescence，并以 `bytes_transferred == 0` 收敛残余 handler；
- device/context/port 不匹配；
- provider：至少覆盖项目正式支持的 RoCE/InfiniBand 环境，iWARP 若声明支持则必须单独验收。

## 最终验收标准

1. `ibv_queue_pair` 独立拥有 native QP，connector 不保存 QP 裸所有权；
2. bind 成功后可以在 connect/accept 前 native pre-post receive；
3. connect/accept 使用 self-created QP 的受支持 rdma_cm 流程；
4. 所有 WR 在成功、失败、取消和 disconnect 下恰好完成一次；
5. event/poll completion 语义保持一致；
6. 当前 RDMA-006 move-accept convenience API 无行为回退；
7. 真机 smoke/stress 和现有回归全部通过。
8. 单测/native hook 与真机 trace 都能证明 Receive WQE 在 CM accept/connect call 前已成功提交，而不是依赖 RNR retry 或库内延迟 post。
9. CQ notification/poll/device terminal failure 不会静默 hang，也不会在 native WR quiesce 前执行允许用户释放 buffer/MR 的 callback。
10. shared CQ 上旧 QP 的 late CQE 不会命中已释放或地址复用的 operation；失败路径无 handler 泄漏、UAF 或 double completion。
