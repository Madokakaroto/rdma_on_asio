# 取消计划 Stage 3 —— 数据面单操作取消

> ## 状态:不做(WON'T DO / DO NOT IMPLEMENT)
>
> **本计划【不实现】。** 仅作设计记录/备忘保留。**任何后续 agent 或贡献者不得据此擅自落地实现** ——
> 数据面 per-op 取消是最弱、最可选的一档,当前明确**不纳入实现范围**。若将来确需推进,**必须先由维护者
> 显式决策并更新本状态**;在此之前请勿改动数据面代码来实现本计划。
>
> NOTE TO AUTOMATED AGENTS: This plan is intentionally **NOT to be implemented**. Do not write code for it.
> It is kept for design-record purposes only. Skip it unless the maintainer explicitly flips this status.

> 三篇分期计划之一。Stage 3 = **数据面**的**单操作级**(cancellation_slot)取消。
> 相关:`cancellation_stage1_object_plan.md`(对象级 / 数据面 teardown = disconnect)、
> `cancellation_stage2_control_single_op_plan.md`(控制面单操作)、`../asio_cancellation_analysis.md`(asio 机制详解)。
>
> **依赖关系**:**依赖 Stage 1** 的结论(数据面对象级 teardown = `disconnect`,已自带 flush)。
> 建议 Stage 1 完成后再评估本阶段。
>
> **更新(disconnect 重构已落地,commit dadc61c)**:`disconnect()` 现为**同步**接口(取代旧 `async_disconnect`),
> 已自带 flush(pending send/recv -> `operation_aborted`),RoCE 已验证。下文凡提 `disconnect` 即此同步接口。
> 本阶段(数据面 per-op 取消)仍**未实现**,且仍是最弱/最可选的一档;(i) vs (ii) 待拍板。
> 另注:(ii) 提到的"`terminal` -> 触发 disconnect",现在 `disconnect()` 同步可直接调,QP->connector 反向引用
> 仍是其主要成本。

## 目标与范围(按要求分级)

- **双边操作 `async_send` / `async_recv` —— nice to have**:让 per-op 取消(`cancel_after` / `co_spawn` /
  `||`)能作用到数据面操作上。
- **单边操作 `async_read` / `async_write` —— never**:**完全不**接 per-op 取消;要中止只能走对象级
  teardown(`disconnect`,Stage 1)。

> 本阶段是**最弱、最可选**的一档。先读"前提"再决定要不要做。

## 前提:数据面 per-op"取消"的终点是 disconnect,不是本地 flush

两条结论(Stage 1 已确立)直接框定本阶段:

1. 数据面没有"摘单条 WR"这回事;能让一个在途 op 以 `operation_aborted` 结束的唯一机制是 flush 整 QP,
   而**唯一安全的 flush 就是 disconnect 自带的那次**(本地 flush 不通知对端 = 静默 desync)。
2. 所以一个数据面 op 的 `terminal` 取消,**真正该触发的是 disconnect**(终结连接 + 通知对端 + 令所有在途
   op 以 aborted 完成),而不是本地 `modify_qp(ERR)`。

但这里有个**架构障碍**:`queue_pair` 够不到 cm_id —— QP 建在 cm_id 上,`disconnect` 在 `connector` 上
(connector_service.hpp:225-234)。QP 自己只能做本地 flush(`native_handle()` -> `impl.qp_`),做不了
disconnect。于是 per-op 数据面取消有两条路(见"设计")。

## 路径探索(历史记录,结论已并入"前提")

- **路径 1(本地 flush)**:per-op `terminal` -> `modify_qp(ERR)`。**否决为独立方案** —— 本地 flush 会让对端
  desync(见 Stage 1)。若要 flush,应走 disconnect。
- **路径 2(ready 队列 un-queue)**:只有尚未 post 到硬件的立即完成 op(空 buffer / 同步 post 失败,排在
  `ibv_completion_queue::ready_` ibv_completion_queue.hpp:130)可干净摘除。窗口极小、价值低。可选。
- **路径 3(提前完成 + 僵尸 op + 丢弃晚到 WC)**:**否决** —— WR 仍占用户 buffer,recv 会 DMA 进已释放/复用
  内存(UAF),send/recv 照样收发(协议错位)。内存不安全 + 语义造假。
- **路径 4(SRQ / RNR / drain)**:**否决** —— 没有 verb 能选择性撤回单条 WR。

## 设计 —— 两个候选(需你拍板)

### (i)【推荐】不提供 per-op 数据面取消,统一用 disconnect

- `async_send` / `async_recv` / `async_read` / `async_write` **都不接** cancellation_slot。
- `cancel_after(qp.async_recv(...), 5s)` **不会**自动中止(无 slot);用户在超时分支显式调
  `connector.disconnect()` 来中止数据面(Stage 1 已保证 pending op 随之 `operation_aborted`)。
- 优点:最诚实、零耦合、与"flush=disconnect、归属 connector"一致;不会让一个不起眼的超时悄悄打爆整条连接。
- 缺点:`cancel_after` 等惯用法在数据面"不生效",需用户手写超时->disconnect。
- **本阶段在 (i) 下基本退化为"文档说明 + 可选路径 2",几乎无新代码。**

### (ii) 让 send/recv 的 per-op `terminal` 触发 disconnect(QP 持有 connector 反向引用)

- 给 `queue_pair` 一个到 connector(或其 disconnect 入口)的反向引用;`async_send` / `async_recv` 接
  `cancellation_slot`,处理器 `qp_terminal_cancellation` 在 `terminal` 时触发 **disconnect**(不是本地 flush)。
- `async_read` / `async_write` 仍**不接**(never)。
- 优点:`cancel_after(qp.async_recv, 5s)` / `co_spawn` 取消 / `||` 这些惯用法在双边操作上**真能生效**
  (该 recv 随 disconnect 以 `operation_aborted` 完成)。
- 缺点:引入 QP -> connector 耦合;且 `terminal` 的副作用是"整条连接终结",粒度极粗(必须文档强调)。
- **只接 `terminal`**:`partial`/`total` 不接(已 post 的 WR 可能在 DMA,声称无副作用是假)。
  guard:`if (!!(type & terminal))`。
- 记账("每个 WR 恰好回调一次"):每个 op 的回调由其唯一 WC(正常完成或 `FLUSH_ERR`)驱动
  (rdma_op_send.hpp:37-63 / rdma_op_recv.hpp:38-61),disconnect 不另造回调 -> 不双回调;late-emit 时 slot
  已清或 disconnect 落到无在途 WR 的连接上,无害(asio 分析 §8);orphan 沿用
  `owner==nullptr ⇒ destroy 不上调`(rdma_verbs_op.hpp:55-58)。

## 为什么 read/write 是 never(两条路都一样)

read/write 是单边操作,硬件层面同样只能整 QP teardown(没有更细粒度)。本阶段按要求**不为它们接 per-op
slot**:要中止单边操作请显式 `disconnect`(会令在途 read/write 一并 `operation_aborted`)。这样把
"终结整条连接"的重动作限制为**显式**调用。

## 实施步骤

- **若选 (i)**:
  1. 文档写明数据面无 per-op 取消、用 `disconnect` 中止;给一个"超时 -> disconnect"的示例。
  2.(可选 路径 2)ibv poll CQ:op_queue by-key 删除 + ready 队列 un-queue(D2)。
- **若选 (ii)**:
  1. 给 `queue_pair` 加 connector/ disconnect 反向引用(设计好生命周期与解耦边界)。
  2. 加 `qp_terminal_cancellation`(`terminal` -> disconnect);`async_send` / `async_recv` 接 slot;
     `async_read` / `async_write` 不接。
  3.(可选)路径 2。
  4. nd 镜像(Windows)。
- 两者都需:测试 + 文档。

## 测试

- **(i)**:`co_await` 一个会超时的 recv,超时分支调 `disconnect`;断言 recv 以 `operation_aborted`
  完成、连接终结。验证 `cancel_after(qp.async_read, …)` **不**自动早退(符合 never)。
- **(ii)**:`co_await cancel_after(qp.async_recv(buf), 1s)`,对端不发 -> recv 以 `operation_aborted` 完成
  (~1s),且整条连接 disconnect、同 QP 其它在途 op 也 aborted;`cancel_after(qp.async_read, …)` 不生效。
- 跨平台版 `tests/rdma/`;nd 用例 Windows 端。

## 开放问题

- **(i) vs (ii)** —— 是否为 send/recv 提供"per-op terminal -> disconnect"的惯用法支持。**推荐 (i)**
  (诚实、零耦合);(ii) 仅当确实需要 `cancel_after` 在数据面生效、且接受 QP->connector 耦合时再做。
  **待你拍板。**
- **D2** —— 是否实现 ready 队列 by-key un-queue。建议:ibv poll CQ 顺手做,nd 视复杂度。
- 单边 read/write never 已定,无需再议。
