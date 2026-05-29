# nd_connection.hpp Review

## Design Goals

1. nd_connection 没有同步接口
2. nd_connection 如果没有绑定 asio 的 io context，用户通过 queue pair 自己 poll completion，执行回调
3. nd_connection 如果绑定了 asio 的 io context，则通过 IOCP 的完成通知，响应回调

---

## Goal 1: No Synchronous Interface

**基本达标，但有一个灰色地带。**

数据面操作（connect / send / recv / read / write）全部是 async，这点没问题。但 `open()` 是同步的，且构造函数（`nd_connection.hpp:66-73`）内部做了同步 open + throw。如果意图是"所有与 ND2 设备交互的操作都不该是同步的"，那么构造函数里做 `open` 违反了这一点。

**建议**：要么去掉带 device 参数的构造函数，让用户显式调两步（default construct + open），要么接受 open 是一个"配置操作"而非"IO 操作"因此不算违反设计目标。

---

## Goal 2: 未绑定 executor 时用户自行 poll CQ

**这是目前最大的缺口。**

当前所有 async 操作都无条件解引用 `pimpl_`：

```cpp
self_->pimpl_->get_service().async_connect(...)   // line 210
self_->pimpl_->get_service().async_send(...)      // line 253
```

如果 `pimpl_` 为 null（未绑定 executor），调用任何 async 方法都是 UB（空指针解引用）。类中没有任何保护。

更关键的是：**nd_connection 没有为"无 executor"模式提供任何公开接口**。用户想自己 post work request 然后 poll CQ，但：
- `state_` 是 `protected`，外部无法访问 `state_->qp_` / `state_->cq_`
- 没有暴露 `get_queue_pair()` 或 `get_completion_queue()` 之类的 accessor
- 没有 `poll()` 或 `get_results()` 的封装

**需要完善的内容**：

1. 为无 executor 模式提供访问 QP/CQ 的途径（getter 或 friend class）
2. async 操作入口加上前置检查 -- 没有 executor 时应该返回错误而不是 crash
3. 或者提供一组非 IOCP 路径的异步操作：用户 post WR 后由自己的 polling 线程 call `GetResults` 并手动 dispatch callback

---

## Goal 3: 绑定 io_context 后走 IOCP 通知

**已基本实现。** `set_executor` -> `register_state` -> `scheduler_.register_handle(overlapped_handle)` 把 handle 注册到 IOCP，后续 `work_started` 里通过 `notify_cq` + `scheduler_.on_pending` 让 IOCP 在 CQ 有完成时回调。路径是通的。

---

## Other Issues

| 问题 | 位置 | 说明 |
|------|------|------|
| async 操作缺少前置校验 | async_send/recv/read/write | 未检查 `is_open()` / `has_executor()`，不像 `async_connect` 至少检查了 `is_open` |
| `cancel()` 未实现 | line 153-156 | 连接生命周期管理不完整，无法取消 in-flight 操作 |
| 缺少 `async_disconnect` | -- | 完整连接生命周期需要 disconnect（ND2Connector::Disconnect 是 OVERLAPPED 操作） |
| 移动转换构造函数可能有问题 | line 54 | `std::make_unique<impl_type>(*other.pimpl_)` 是拷贝 io_object_impl，但 impl 内部持有的 state 已经被 move 走了，copy 的 impl 里 state 是空的 |
| `assign()` 不验证 state 有效性 | line 158-178 | 可以 assign 一个 null state 进来，之后 `set_executor` 会失败但错误信息不直观 |

---

## Summary

离设计目标最远的是 **Goal 2**。核心需要解决的是：为"无 executor"模式定义一个明确的使用方式。两种可能的方向：

**A. 暴露底层原语**：加 `native_qp_handle()` / `native_cq_handle()` 之类的 accessor，让用户拿到 CQ 后自己 poll。简单但失去类型安全。

**B. 提供 poll-mode 封装**：在 nd_connection 上加一个 `poll_completion(span<wc_t>)` 接口，让用户驱动完成事件。提交 WR 仍然通过 nd_connection 的接口（需要一组不依赖 pimpl_ 的提交方法），只是完成通知不走 IOCP 而是用户手动 poll。
