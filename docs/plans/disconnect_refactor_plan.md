# 重构计划 -- connector 断开语义厘清 + 新增 `async_wait_disconnect`

> 厘清 connector 的"断开"语义,并补上当前**完全缺失**的"断开通知"能力。
> 相关:`cancellation_stage1_object_plan.md`(数据面 teardown = disconnect 的结论由此细化)、
> `../asio_cancellation_analysis.md`。
> 状态:**已实现并验证**。Phase 1(同步 disconnect + 死代码清理 + teardown drain/ack + 迁移)与 Phase 2
> (`async_wait_disconnect` + `ext_disconnected`/`ext_device_removed`)均已落地;ibv 侧在 RoCE(mlx5_0)上
> 端到端验证通过(echo / poll-echo / 新增 `test_ibv_wait_disconnect`:对端断开 -> `ext_disconnected`,电平触发
> re-arm 立即完成)。nd 侧已镜像(同步 fire-and-forget disconnect + `NotifyDisconnect` watcher),**待 Windows
> 编译/运行核实**(§5、§9)。Phase 3(空闲死对端探测)按 §3.7 不内建。

---

## 0. 背景与动机(调研结论)

1. **"我方发起断开"和"感知对端断开"是两件不同的事。** 前者是一次性 teardown;后者是被动等待的事件通知。
   把后者误并进 `async_disconnect` 是当前接口的根因问题。
2. **真正缺失、且有价值的是"断开通知"(on_disconnect),而不是把同步 teardown 包成 async。**
3. **断开可经三条通道得知**:
   - 通道 A:rdma_cm 事件通道 -- `RDMA_CM_EVENT_DISCONNECTED`(优雅断开)、`TIMEWAIT_EXIT`、`DEVICE_REMOVAL`。
   - 通道 B:CQ 工作完成错误 -- `IBV_WC_WR_FLUSH_ERR`(flush)/ `IBV_WC_RETRY_EXC_ERR`(重试耗尽)。
   - 通道 C:ibv 异步事件队列(`ibv_get_async_event`)-- `IBV_EVENT_QP_FATAL` 等。
4. **优雅 vs 非优雅**:对端 `rdma_disconnect` => 通道 A 有 `DISCONNECTED`;对端崩溃/拔线(无 DREQ)=>
   **通道 A 无事件**,只有通道 B 的 `connection_reset`(及通道 C 的 async event)。
5. **现状缺口(file:line)**:
   - 建链后**无常驻 reader 监听 cm 事件通道**(唯一事件读取点 ibv_op_cm.hpp:26-33),**对端 DISCONNECTED
     被丢弃,应用无法主动获知**。
   - `async_disconnect` 是"同步外衣":`start_disconnect_op`(connector_service.hpp:305-312)同步调
     `rdma_disconnect` 后立即 `post_immediate_completion`,从不 arm `ibv_disconnect_op`;
     `ibv_disconnect_op::do_perform`(ibv_op_connect.hpp:257-269)等 `DISCONNECTED` 是**死代码**。
   - 通道 C 全代码库零处理;`TIMEWAIT_EXIT`/`DEVICE_REMOVAL` 未处理。
6. **QP 归 connector**:QP 建在 cm_id 上(`cm_id->qp`),connector 拥有;`queue_pair` 非拥有、无 teardown、
   不持 cm_id。**断开与断开通知都必须落在 connector**。

---

## 1. 目标

- **(1)** `async_disconnect` -> **同步 `disconnect()` / `disconnect(ec)`**(主动 abrupt teardown)。
- **(2)** 新增 **`connector::async_wait_disconnect(token) -> void(ec)`**(一次性"断开通知",即 on_disconnect)。
- **(3)** 明确数据面 op 的 error code 作为**主断开探测器**(像 socket eof);`async_wait_disconnect` 为辅。

---

## 2. 接口最终形态【已共识】

| 语义 | 接口 | 同步/异步 | 备注 |
|---|---|---|---|
| 我方主动断开(abrupt teardown) | `connector::disconnect()` / `disconnect(ec)` | **同步** | 取代 `async_disconnect` |
| 感知断开(**on_disconnect**) | `connector::async_wait_disconnect(token) -> void(ec)` | **异步**(一次性) | **新增**,完成回单一码 `ext_disconnected` |
| ~~async_disconnect~~ | -- 删除 -- | | 现有测试需迁移(见 §6) |

数据面侧**不新增任何接口**:断开检测靠数据面 op 自身的 error code(§3.4)。

---

## 3. 设计【已共识】

### 3.1 active disconnect = 同步、非阻塞

原则:**只要 disconnect 本身不阻塞,就用同步语义。**

- **ibv**:`rdma_disconnect` 同步非阻塞 -> `disconnect()` 直接调,立即返回。
- **nd**:`IND2Connector::Disconnect` 是 overlapped,但采用 **fire-and-forget**:发起 Disconnect、挂一个
  自我回收(self-reaping)的 overlapped、**不等其完成**即返回,对调用方呈现"同步非阻塞"。
  - 【Windows 核实】fire-and-forget overlapped 的回收;**销毁 `IND2Connector` 前需保证该 overlapped 已
    完成/不在飞**(teardown 排序)。
- **重申**:"同步"只覆盖**发起那一下**;被 flush 的在途 send/recv 仍**异步**以 WC 排空(两后端皆然)。
  返回时 QP 已转 ERROR、DREQ 已发;**未送达的 send 被丢弃**(abrupt,非 graceful)。

清理:`ibv_disconnect_op` 中"等 `DISCONNECTED`"的死代码**抽出复用**到 §3.3 的 watcher op。

### 3.2 `disconnected_` 状态位 + 四种时序(电平触发)

connector 持一个 `disconnected_` 状态位,统一覆盖所有时序;`async_wait_disconnect` 是**电平触发**的
(已断开时再注册也会立即完成,而非吊死):

| 时序 | 行为 |
|---|---|
| `disconnect()`(我方) | **直接置 `disconnected_`** + fire-and-forget `rdma_disconnect` + 若有未决 wait 则 complete 它(**不依赖**不可靠的自端 `DISCONNECTED`)|
| 对端 `DISCONNECTED`(watcher 读到) | 置 `disconnected_` + complete 未决 wait |
| `async_wait_disconnect()`,且**已** `disconnected_` | **post 一个 deferred completion**(asio 惯例:绝不 inline 调 handler,投递给 scheduler、在关联 executor 上跑)|
| `async_wait_disconnect()`,且**未** `disconnected_` | arm watcher 等(见 3.3)|

一次性语义:wait 完成后即结束;晚到的 `DISCONNECTED` ack 掉即 no-op,**不双触发**。

**主动方不依赖自端事件**:规范(`rdma_disconnect(3)`)说两端都产生 `DISCONNECTED`,但实践中主动方收得不可靠
(connector_service.hpp:307-309 注释),故自断开走"本地确定性 complete"。

### 3.3 `async_wait_disconnect` = on-demand watcher + teardown drain/ack + 单一码

把两件事分开:**事件处理+ack 必须永远正确;callback 是否回调取决于有没有用 `async_wait_disconnect`。**【D-A/D-B 已定】

- **用户定制逻辑只能经 `async_wait_disconnect` 注册的回调触发。** `disconnect()` 是同步非阻塞的;disconnect/cm
  事件由库**内部始终正常处理**(读取、ack、置 `disconnected_`),但库**不替用户做任何反应**。
- **没注册 `async_wait_disconnect` 时**(无论本地还是远端断开):**只处理事件、不回调、也不自动注册任何 callback**。
- **不用 always-armed 常驻 watcher**(那会像 CQ poller 一样钉住 `run()`)。**on-demand** 即可且正确:
  - 没注册 wait 时:对端 `DISCONNECTED` 先排在 cm 通道里(无人 complete callback)。
  - 调 `async_wait_disconnect` 时 arm watcher,它**把已排队的 `DISCONNECTED` 读出并立即完成**(也实现 3.2
    的电平触发)。
  - 始终没人调:**teardown 时 drain + `rdma_ack_cm_event`** 掉所有未消费事件,再 `rdma_destroy_id`
    (connector_service.hpp:314-327 处补)。**必须做** -- 否则未 ack 事件会阻塞 `rdma_destroy_id`(man page)。
- watcher 实现:新 reactor op `ibv_wait_disconnect_op : ibv_op_cm`,armed 在 `cm_channel_->fd`,复用
  `ibv_op_cm::get_cm_event` + "返回 `not_done` 自我重挂"模式:
  - `RDMA_CM_EVENT_DISCONNECTED` -> 置 `disconnected_`、`status::done`、以**断开码**complete wait(码的选取见 §3.5 / D-D)。
  - `RDMA_CM_EVENT_TIMEWAIT_EXIT` -> **ack 后忽略**,`not_done` 继续(QP-复用内务,本库不复用)。**【D-E 已定】**
    不提供接口、不向用户暴露;watcher 还 armed 时 ack+忽略,否则 teardown drain+ack;**不依赖其送达**(它在
    `DISCONNECTED` 之后、只关乎 QPN 复用,与优雅断开的处理无关;ack 仅为防 `rdma_destroy_id` 被未 ack 事件阻塞)。
  - `RDMA_CM_EVENT_DEVICE_REMOVAL` -> 设备级致命事件(用户须销毁 cm_id);**以独立码 `ext_device_removed`
    完成 wait**(同样自定义,源自 CM 的 `DEVICE_REMOVAL` 事件),不与普通断开混为一谈。**【D-E 已定:本期先这样
    落地;以后再看是否提到设备层】**
  - **每个事件都 `rdma_ack_cm_event`**。
- **与 active disconnect 无冲突**:disconnect 现在不 arm cm op(同步立即返回),故建链后 cm fd 上唯一可能的
  reactor op 就是这个 watcher,不抢队列。

四种用法(文档化时收进 README):
- **A** 起一个 watcher sub-coroutine(全局善后/重连);
- **B** 主循环 `co_await (qp.async_recv || conn.async_wait_disconnect)` 竞速(无需额外协程;取消 wait 是干净的
  控制面取消);
- **C** 直接传 callback token(即 on_disconnect 回调写法);
- **D** 很多时候不需要它 -- 见 3.4。

### 3.4 数据面 op 的 error code = 主断开探测器(返回自然码,业务侧判断)

**在收发时,数据面 op 自身的 error code 就是最可靠的断开探测器**,用法与 socket 判 eof 一致。**不做任何强行的
eof<->flush 映射:该返什么自然码就返什么,把 ec 抛给用户,由业务侧自己判断"连接断了"**(正如 socket eof 成为
业务侧判断断开的惯例)。

```cpp
auto [ec, n] = co_await qp.async_recv(buf, use_nothrow);
if (ec) { /* 连接断了 / QP 失效 -> 退出循环、teardown */ break; }
```

本代码库里只要数据面 op 的 ec 非空,**QP 必已进 ERROR、连接必已不可用**(`wc_status_to_ec`
ibv_op_complete.hpp:15-24,**维持现状不改**):

| ec | 来源 | 含义 |
|---|---|---|
| (无) | `IBV_WC_SUCCESS` | 成功 |
| `operation_aborted` | `IBV_WC_WR_FLUSH_ERR` | QP 被 flush(我方或对端断开)-> QP 失效 |
| `connection_reset` | 其余所有错误(含 `RETRY_EXC_ERR`)| 传输失败/对端崩溃/本地错误 -> QP 失效 |

> 这与 asio 自己的做法一致 -- asio 也只是把 `eof`/`connection_reset` 透过 op 的 ec 抛出,由用户决定(见 §3.6)。
> `FLUSH_ERR -> operation_aborted` 是 flush 最自然的语义("被 teardown 中止",而非 eof);**不强行映射成 eof**。

### 3.5 为什么"断开通知"用单一码,而不细分 cm event(开源惯例)

优雅断开在 cm 层**实质只有一个可操作事件 `DISCONNECTED`**(`TIMEWAIT_EXIT` 是 QP-复用内务,`DEVICE_REMOVAL`
是另一类设备事件),没有"断开事件族"需要细分。结合开源惯例:

- **rsocket(librdmacm 的 RDMA-over-socket)**:把 RDMA/cm 复杂度**塌缩进标准 socket 语义** -- 断开 = `rrecv`
  返回 0(EOF),错误走 `errno`,**不向应用暴露 cm event**。
- **UCX(ucp)**:断开/错误收敛到**一个 per-endpoint error 回调 + 一个状态枚举**,不暴露底层事件。
- **libfabric**:最底层、最事件化,也只是一个 `FI_SHUTDOWN` 事件。
- **NCCL/MPI/gRPC**:连接丢失 = 单一致命/unavailable 状态。

**规律:面向易用的库都把传输 teardown 收敛成极小的码集合(常常就一个),只有最底层的可移植层才暴露一个 SHUTDOWN
事件。** 因此我们采用**两层、各自一个简单语义**:

- **数据面 op 的 ec**(操作层细节):`operation_aborted` / `connection_reset`(§3.4,Q4 已定,维持不动)。
- **`async_wait_disconnect` 的 ec**(带外):**单一断开码** = "这条连接结束了"。

不把 `DISCONNECTED`/`TIMEWAIT_EXIT` 等拆成多个用户码。

**断开码的选取【D-D 已定】:优先用 ibv/nd 原生码,不做映射;没有合适原生码就自定义,且不映射到 socket 错误码。**
- ibv:`RDMA_CM_EVENT_DISCONNECTED` 是事件、其 `status` 通常为 0,**没有原生 errno 表示"对端优雅断开"**
  -> **自定义 `ext_disconnected`**(加到 ibv_error,与既有 `ext_*` 风格一致)。
- nd:看 `IND2Connector::NotifyDisconnect` 完成时是否带原生 ND 状态码;有则用(映射到 `nd_errc`),无则同样
  **自定义 `ext_disconnected`**(nd_error)。
- **不映射到 `asio::error::eof` / `connection_reset` 等 socket 码** -- 带外断开通知用自己的码,语义清晰、不与
  数据面 WC 的 asio 标准码混淆。

### 3.6 与 asio socket 的对照(v1/v2 的正确定位)

asio 对 socket 断开**没有带外通知,全靠 I/O 操作的 error code**(vendored asio,file:line):

- 优雅 FIN -> `eof`:epoll `socket_ops.ipp:1014-1018`;IOCP `socket_ops.ipp:995-999`(`complete_iocp_recv`,
  0 字节+无错+流式 -> eof)。
- RST -> `connection_reset`:epoll 透传 `ECONNRESET`(error.hpp:95);IOCP 把 `ERROR_NETNAME_DELETED` 映射
  (`socket_ops.ipp:978-983`)。
- **无 `async_wait_disconnect`/CM 式事件**;`async_wait(wait_read)` 在 FIN 时 fire 但不说明原因,须跟一个 read
  才拿 eof。IOCP 亦无 `DisconnectEx`。

|  | 优雅断开(主探测) | 优雅断开(带外) | 非优雅(有 I/O) | 非优雅(空闲) |
|---|---|---|---|---|
| asio epoll | read -> `eof` | 无(只能 wait_read+read) | read/write -> `connection_reset` | 无(靠 OS `SO_KEEPALIVE`) |
| asio IOCP | recv overlapped 0 字节 -> `eof` | 无 | overlapped -> `connection_reset` | 无(靠 OS `SO_KEEPALIVE`) |
| 我们 ibv | 数据面 op -> `operation_aborted` | `async_wait_disconnect`(cm `DISCONNECTED`) | 数据面 op -> `connection_reset` | 无(靠应用层心跳)|
| 我们 nd | 数据面 op -> `operation_aborted` | `async_wait_disconnect`(`NotifyDisconnect` overlapped) | 数据面 op -> `connection_reset` | 无(靠应用层心跳)|

**结论(定位 v1/v2)**【D-C 已定】:
- **数据面 ec 探测器(底座 D)= asio socket 的完整模型**,覆盖优雅 + **有 I/O 时**的非优雅。数据面**不分 v1/v2**。
- **v1 = `async_wait_disconnect`**:asio socket 没有的带外能力(RDMA 提供了 cm `DISCONNECTED` / ND
  `NotifyDisconnect` 这种带外原语),**只盯优雅断开**。**Phase 2,做**。
- **v2(空闲时的非优雅断开)= 不内建**。调研结论(见 3.7):整个 RDMA 生态都**不**把 keepalive 做进传输层,
  而是放到协议/应用层(NVMe-oF KATO、SMB Direct、iSER NOP、UCX `UCX_KEEPALIVE_INTERVAL`);asio 自身也不实现
  keepalive(只暴露 OS 的 `SO_KEEPALIVE`),而 RDMA 没有 OS 级 keepalive 可暴露。**故本库不内建 keepalive,
  把空闲死对端探测留给应用层心跳**(用现有原语即可,见 3.7);可选 helper 留作以后。

### 3.7 空闲死对端探测 = 应用层心跳(不内建,调研结论)

RDMA RC 在传输层**没有** keepalive(QP 的 `timeout`/`retry_cnt`/`rnr_retry` 只在你**真的 post 了 WR** 后重试
耗尽才失败;rdma_cm 对已建链连接无 keepalive)。所以**空闲(不收不发)时对端死了,本地零信号** -- 这和 socket
是同一问题,且比 socket 更裸(TCP 至少有 OS 级可选 `SO_KEEPALIVE`)。

**生态做法(调研)**:底层(verbs / rdma_cm / **libfabric**,后者文档明确"无 keepalive 选项,失败只在尝试操作或
传输自行察觉时经 EQ 报出")都不解决,等同我们的底座 D;**上层**各自加自己的 keepalive(NVMe-oF 的 KATO、
SMB Direct、iSER 的 NOP、UCX 的 `UCX_KEEPALIVE_INTERVAL`)。**keepalive 一律活在传输之上。**

**本库的选择:不内建 keepalive。** 库提供两个原语 -- 底座 D(收发报 `connection_reset`)+ `async_wait_disconnect`
(优雅断开)-- 应用层用它们自行实现心跳:

```cpp
// 应用层心跳:周期发探测,失败即视为断开(用现有原语,库无需新增)
while (!stop) {
  timer.expires_after(1s); co_await timer.async_wait(use_nothrow);
  auto [ec, n] = co_await qp.async_send(heartbeat_buf, use_nothrow);
  if (ec) { /* connection_reset -> 对端死了,teardown/notify */ break; }
}
```

(可选,以后)提供一个轻量 helper(可配间隔 + 丢失计数),探测失败时 complete 未决的 `async_wait_disconnect`,
类似 UCX -- 但**默认关闭、非核心**。

---

## 4. ibv 实现要点(file:line)

- `disconnect()` 同步:`ibv_connector` 公开 `void disconnect()` / `void disconnect(ec)`(替换
  async_disconnect ibv_connector.hpp:115-124);service 侧保留同步 `disconnect()` 包装(rdma_disconnect),
  去掉 op 分配 + `post_immediate_completion`(connector_service.hpp:225-234/305-312)。
- `disconnected_` flag:加到 `ibv_connector_service::implementation_type`。
- `async_wait_disconnect`:新 `ibv_wait_disconnect_op`(复用 `ibv_disconnect_op` 死代码逻辑);service 入口
  按 flag 立即完成或 arm watcher;完成回 `ext_disconnected`(新增到 ibv_error)。
- teardown drain/ack:在 `close_for_destruction`(connector_service.hpp:314-327)的 `rdma_destroy_id` 前循环
  `rdma_get_cm_event`/`rdma_ack_cm_event` 排空。
- 数据面 ec:无需改动(`wc_status_to_ec` 现有映射即所需)。

---

## 5. nd 实现(与 ibv 接口一致,Windows 核实)

asio 在 IOCP(proactor)上对 socket 断开的做法:recv 的 overlapped **完成**时,在 `complete_iocp_recv`
(`socket_ops.ipp:995-999`)里 0 字节 -> `eof`、`ERROR_NETNAME_DELETED` -> `connection_reset`;**断开就是通过那条
recv 完成事件投递的,无独立通道、无 DisconnectEx**。nd 的 ND2 同为 proactor,但**多了一个专门的带外断开原语**:

- `disconnect()`(同步):`IND2Connector::Disconnect` fire-and-forget(self-reaping overlapped + teardown
  排序,见 3.1)。
- `async_wait_disconnect`:发起 `IND2Connector::NotifyDisconnect(overlapped)`;**当该 overlapped 在 IOCP
  完成时**,做完整内务(回收 overlapped、置 `disconnected_`),**再 complete 用户经 async_wait_disconnect 传入
  的 callback**(与 ibv watcher 收到 `DISCONNECTED` 后 complete wait 完全对称)。**callback 只在用了
  async_wait_disconnect 时才回调**;没用就内部把 NotifyDisconnect overlapped 处理掉。
- `disconnected_` flag + 电平触发 + 自断开本地确定性 complete,与 ibv 同。
- 【Windows 核实】`NotifyDisconnect` 是否对**优雅对端断开**完成、是否对**本地 Disconnect** 也完成、签名;
  fire-and-forget `Disconnect` overlapped 的回收与 teardown 排序;`NotifyDisconnect` 是否非阻塞(若非阻塞则
  按上述同步语义实现)。

---

## 6. 迁移

- 现有 `co_await conn.async_disconnect(use_nothrow)`(test_*_echo.cpp、README 示例)改为
  `conn.disconnect(ec)`(同步)。
- 数据面"`if (ec) break;`"已是现状,无需改。

---

## 7. 测试

- `tests/ibv/`(RoCE):
  - **同步 disconnect**:`conn.disconnect()` 后,在途 `async_recv`/`async_send` 以 `operation_aborted` 完成
    (异步排空,不假设与 disconnect 的顺序)。
  - **通知-先注册后断开**:server `co_spawn` 一个 `async_wait_disconnect` watcher;client `disconnect()`;
    断言 server watcher 以 `ext_disconnected` 完成。
  - **通知-先断开后注册(电平触发)**:对端先断开,本端**之后**调 `async_wait_disconnect` -> 立即完成。
  - **`||` 用法**:server `co_await (qp.async_recv || conn.async_wait_disconnect)`,client 断开后循环退出。
  - **主探测器 D**:client `disconnect()` -> server 在途 recv 以 `operation_aborted`;`kill -9` client ->
    `connection_reset`。
- `tests/rdma/`:跨平台版;nd 用例 Windows 端。

---

## 8. 分期实施

1. **Phase 1**:`async_disconnect` -> 同步 `disconnect()`(ibv;nd fire-and-forget)+ 清理死代码 +
   teardown drain/ack + 迁移测试 + 文档(数据面 ec 主探测器 + 与 asio 对照表)。
2. **Phase 2**:`async_wait_disconnect` **v1**(ibv:cm `DISCONNECTED` on-demand watcher + `disconnected_`
   flag + 电平触发 + `ext_disconnected` 码)+ nd `NotifyDisconnect` 镜像 + README 四种用法。
3. **Phase 3(文档,非核心)**:空闲死对端探测**不内建** -- 在 README 文档化"应用层心跳"模式(§3.7 示例)。
   可选 helper(默认关闭)留作以后。**不接 ibv async events 做内建 keepalive**(纯空闲时 async event 一样不产生
   信号,解决不了空闲探测;真正解法就是上层心跳)。

---

## 9. 待核实 / 按需(非阻塞共识)

- **nd 细节(Windows 核实)**:`NotifyDisconnect` 的完成条件(对端优雅 / 本地 Disconnect)、是否非阻塞、签名、
  完成时是否带原生 ND 状态码;fire-and-forget `Disconnect` overlapped 的回收与 teardown 排序。
- **断开码命名(D-D 已定原则,待落代码)**:两个自定义码 -- 优雅断开 `ext_disconnected`、设备移除
  `ext_device_removed`(加到 ibv_error / nd_error);ibv 无原生码故自定义,nd 有原生则用、无则自定义;均不映射
  socket 码。命名待最终确认。
- **(D-E `DEVICE_REMOVAL` 已定)**:本期在 `async_wait_disconnect` 以独立码 **`ext_device_removed`** 完成
  (码源自 CM 的 `DEVICE_REMOVAL` 事件);提到设备层处理留作以后。
- **(D-C 已定)空闲死对端探测不内建**:文档化应用层心跳(§3.7);可选 helper(默认关闭)留作以后。
- **(D-E `TIMEWAIT_EXIT` 已定)**:不提供接口、不向用户暴露;watcher armed 时 ack+忽略,否则 teardown drain+ack;
  不依赖其送达。与优雅断开处理无关(它在 `DISCONNECTED` 之后、只关乎 QPN 复用,本库不复用 QP)。
