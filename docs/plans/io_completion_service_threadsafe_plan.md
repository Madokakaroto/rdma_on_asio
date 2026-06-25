# 改造 io_completion_service 线程安全化 — 重构计划

> Make `io_completion_service` thread-safe for concurrent multi-thread submit + multi-thread
> `io_context::run()`, lock-free, via a single always-armed self-perpetuating CQ poller.
> Working/intermediate doc; delete after it lands. (Primarily an ibv fix; nd aligned for parity.)

## 目标 / Goal

生产环境下 `io_context` 会被多个线程 `run()`,且多个线程会并发提交 `async_send`(RDMA 支持并发
post)。当前 ibv 的完成通知机制在这种场景下有**数据竞争**,要做成线程安全且**无锁**(热路径不加锁)。

## 当前的竞争(ibv)

`ibv_io_completion_service` 用一组**裸**成员做"按在途 op 数量开合 poller"的状态机:
- `arm_notify()` 由 `verbs_service::finish_event` **每个数据面 op** 调用(来自任意提交线程):写
  `pending_`、读写 `armed_`,并在 `ensure_armed()` 里 `req_notify_cq` + `poll_cq` + `start_op`。
- poller(`do_perform`/`on_notify_complete`,在 run() 线程)写 `armed_`/`in_dispatch_`、减 `pending_`、`poll_cq`。

裸 `bool`/`size_t` 并发读写是 UB;更严重的是 **`ibv_poll_cq(cq_)` 会被提交线程的 `ensure_armed` 和
run 线程的 `do_perform` 同时调用**(同 CQ 并发 poll 在 provider 层不保证安全)。

(nd 侧不同:`nd_io_completion_service` 现在每次 arm 现 new 一个 `nd_notify_wr_op` 调 `Notify`,没有
共享 `pending_/armed_` 标志,所以**没有这个特定的标志竞争**;但它有"每 op 一个 notify"的开销,见下。)

## 设计:单一、自我续armed 的 poller(Scheme A,精化版)

核心:把"谁来 arm/poll CQ"**完全收敛到一个复用的 poller op**,提交线程**一概不碰** service 状态。

1. **点火时机 = 第一个 event 模式 QP `bind(io)`(一次性,原子),不是 `initialize()`。**
   - `initialize()` 仍只建 CQ + comp_channel(use_device 期间)。
   - `queue_pair::bind(io)` 末尾调 `io_svc.ensure_poller_started()`:用一个 `std::atomic<bool>
     poller_started_` 做一次性 CAS,**赢家**执行 `req_notify_cq + poll(兜 post-before-notify) +
     start_op`;其余 bind 看到已点火即返回。
   - **为什么不在 initialize 点火**:那会钉住该 io_context 的**每一次** `io.run()`(含只跑控制面的),
     破坏 poll 模式/控制面用 `io.run()` 分阶段返回的用法。点火绑定到 event 模式 bind,就只影响真正用
     event 数据面的 io_context。
2. **自我续armed**:`on_notify_complete` 派发完抽干的 ops 后,**无条件 re-arm**(`start_op`)——只要
   `owner != nullptr`(`owner == null` 是 shutdown,取消 poller、不再 re-arm)。`do_perform` 的
   `ack → req_notify_cq → poll` 配方不变。
3. **提交线程零接触**:删除 `arm_notify()` 的每-op 调用。`finish_event` 的非立即分支变成 **no-op**
   (poller 一直 armed);只有空 buffer / 同步 post 错误仍走 `scheduler_`(立即完成)。于是
   `verbs_service` **不再依赖 io_completion_service** 来 arm(可去掉 split 重构里加的 cached
   `io_completion_service&`)。

### 为什么这是无锁 + 线程安全

- 全局只有**一个**复用 poller op;asio 保证单个 op 不会被并发执行(即使多线程 run())→ `do_perform`
  /`on_notify_complete` 任一时刻只在一个线程跑 → `ibv_poll_cq(cq_)` 独占、`completed_` 独占 → **无需锁**。
- 点火是**一次性原子 CAS**;赢家在 `start_op` **之前**做那次 poll,此时 poller 尚未在途,与 `do_perform`
  不重叠。
- 第一次点火后,提交线程(`async_send`)对 service **不写任何状态**(立即完成路径走线程安全的 scheduler)。
- `start_op` / `req_notify_cq` 本身线程安全,可从任意线程调。
- 删除 `pending_` / `armed_` / `in_dispatch_`(不再需要按 op 计数/去抖/防重入——poller 恒 armed,
  handler 在派发里重新 post 也无需特殊处理,下一轮无条件 re-arm 自然兜住)。新增 `atomic poller_started_`。

### post-before-notify 竞态

第一个 op:post 在 `ensure_poller_started` 之前(`finish_event`/bind 顺序),首次点火里的"`req_notify_cq`
后立即 poll"兜住它(同现有 `ensure_armed` 的处理)。之后 poller 恒 armed,notify 永远在 post 之前就绪
→ 后续 op 无此竞态。

## 契约变化(代价)

一旦某 io_context 上**绑定过 event 模式 QP**,它的 `io.run()` 就**不再因 RDMA 空闲而自动返回**(poller
钉住它);需 `io.stop()`(或析构)让其返回。
- **生产 event 模式服务**(专用 io_context、N 线程长期 run、关停时 stop)——正合适,本来就不靠空闲返回。
- **poll 模式 / 纯控制面 io_context**——**不受影响**(从不点火 poller),`io.run()` 分阶段返回照旧。

退出机制 = `io_context::stop()`(用户)+ `shutdown()`(析构时 deregister comp_channel → poller 以
`owner==null` 收尾、不 re-arm)。**[D2 已定:不提供 service 级 `stop()`]**。

## 重命名(poller 类与 verbs WR 解耦)

`ibv_op_notify_wr` 已与 verbs work request 完全解耦(它只是"被 CQ 事件唤醒 → poll WC → 派发"),且名字也
不守兄弟类 `ibv_connect_op`/`ibv_accept_op` 的 `ibv_*_op` 约定。重命名:
- `ibv_op_notify_wr` → **`ibv_poll_wc_op`**(成员 `notify_op_` → `poller_`)。
- nd 对应 `nd_notify_wr_op` → **`nd_poll_wc_op`**。
- 相关方法名顺带去掉 "notify_wr" 语义残留(如 `on_notify_complete` → `on_poll_complete`)。

## 文件改动

### ibv
- `ibv_io_completion_service.hpp`:删 `pending_/armed_/in_dispatch_` 与 `arm_notify()`;加
  `std::atomic<bool> poller_started_` + `ensure_poller_started()`(一次性点火);poller 完成回调
  改为 `owner ? 无条件 re-arm : 收尾`;`do_perform` 配方不变。**重命名** poller 类
  `ibv_op_notify_wr` → `ibv_poll_wc_op`、成员 `notify_op_` → `poller_`。
- `ibv_queue_pair.hpp`:`bind(io_ctx)` 末尾调 `io_svc.ensure_poller_started()`。
- `ibv_verbs_service.hpp`:`finish_event` 非立即分支变 no-op;立即分支仍用 `scheduler_`;去掉 cached
  `io_completion_service&`(不再 arm)。
- 事件模式 echo 测试(`test_ibv_echo`、`test_rdma_echo`):`co_spawn(..., asio::detached)` →
  `co_spawn(..., [&io](std::exception_ptr){ io.stop(); })`,协程结束即 stop 让 `io.run()` 返回。

### nd(镜像;Windows 未验证)
- nd 现状没有标志竞争,但"每 op 一个 `Notify`"既有开销、也可能有合并(coalescing)下的语义问题。镜像
  Scheme A:**单一复用 poller op**(`nd_notify_wr_op` → `nd_poll_wc_op`),首个 event bind 时点火,完成后
  `poll + dispatch + 重新 Notify` 自我续armed,`owner==null` 收尾。`nd_verbs_service::finish_event`
  非立即分支 no-op。`nd_queue_pair::bind(io)` 调 `ensure_poller_started()`。
- nd `test_nd_echo` 同样改 `io.stop()`-on-completion。

### 不动
- `completion_queue`(poll 模式)、connector/listener、device_service:均不涉及(poll 模式不点火 poller)。

## 验证(ibv 先行,nd 镜像)
1. ibv:改 io_completion_service(含 poller 重命名)+ queue_pair::bind + verbs_service::finish_event。Build。
2. 改事件模式 echo 测试为 stop-on-completion;跑 `test_ibv_echo` / `test_rdma_echo`(event)+ poll
   echo + connector_listener,RoCE 端到端无回归(确认 poll/控制面 `io.run()` 仍正常返回)。
3. nd 镜像。
4. 文档(CLAUDE.md 完成通知一节 + TODO 条目,见 D3)。

## 决策(已定)
- **D1 — 点火时机 = `bind(io)`**:只有 event 模式 QP 绑定到 io_context 才点火 poll-CQ poller;绑定到
  用户自己 CQ 的 poll 模式不点火。
- **D2 — 不提供** service 级 `stop()`;靠 `io_context::stop()` / 析构退出。
- **D3 — 不在本次实现 MT 压力测试**;作为条目写入 **CLAUDE.md 的 TODO list**:"多线程 `io.run()` +
  多线程并发 `async_send` 的并发压力/正确性测试(高并发不崩、完成计数一致、`io.stop()` 干净退出)"。
- **命名 — 已定**:`ibv_op_notify_wr` → `ibv_poll_wc_op`(成员 `notify_op_` → `poller_`);
  nd `nd_notify_wr_op` → `nd_poll_wc_op`。(若你更想要 `ibv_cq_poll_op`,告诉我即可。)
