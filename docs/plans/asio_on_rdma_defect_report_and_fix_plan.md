# asio on rdma 代码缺陷报告与修复计划

日期：2026-07-08

最近整理：2026-07-17

参考来源：alibaba/yalantinglibs#1195 中对 `rdma_on_asio` ND backend 使用侧的 review 意见。本文只 review 本仓库的 `include/rdma` 下 ND/ibv backend 与通用 RDMA 基础设施；`coro_io` / `coro_rpc` 相关代码不纳入本次范围。

优先级说明：

- P0：可能导致随机句柄关闭、崩溃、内存越界或 release 下未定义行为，建议优先修复。
- P1：接口语义明显错误或状态机可能卡死，建议进入下一批修复。
- P2：错误语义不一致、错误处理丢失或能力校验缺口，建议随功能稳定性一起补齐。
- P3：健壮性和可维护性问题，可在前面风险收敛后处理。

## 结论摘要

本仓库中能直接映射到 PR #1195 review 的问题主要集中在 ND：Windows `HANDLE` RAII 判断错误、`CreateOverlappedFile` 失败路径返回未初始化句柄、`nd_memory_region::slice(addr, len)` 拒绝 MR 起始地址、ND accept 失败时连接状态没有统一进入 terminal 状态。此外，本仓库的 ibv 与 ND 都存在一组共同风险：MR 边界检查使用 `offset + length` 可能溢出，服务端 accept 接口要求用户预先提供 backend 状态并不一致的 QP，SGE length 存在窄化/截断风险。

建议先修 P0/P1 中的确定性问题，并给 ND/ibv 都补“不依赖真实 RDMA 设备”的状态保护单元测试；需要真实硬件或 provider 的行为再进入后续集成测试。

## 实施状态（2026-07-17）

本期冻结范围的代码施工已完成：RDMA-001～009、RDMA-011～018、RDMA-020、RDMA-022～025（RDMA-023 已并入 RDMA-018）均已落地。RDMA-010/RDMA-021 的临时 `fail_pending()` 实现经复审于 2026-07-17 撤回：仅等待并发 native post 返回，不能证明已提交 WR、DMA 和未来 CQE 已 quiesce，立即执行 callback 可能让用户过早释放 buffer/MR。两项与 RDMA-019、RDMA-026、ibv QP 生命周期/pre-post receive 一起显式延期，其中 RDMA-010/RDMA-021 的最终设计已并入 `ibv_queue_pair_lifecycle_and_prepost_receive_plan.md`；本期未新增 `operation_not_connected`。

主要产出：

- ND handle/CreateOverlappedFile 严格失败契约、MR 公共安全范围 helper、connector open guard；
- ND/ibv event/poll async move-accept，completion 为 `(error_code, queue_pair)`，QP 在最终 operation storage 中按值持有；
- SGE 单段与总长度校验、`buffer_too_large` / `invalid_config` 公共错误、send/write posted bytes 缓存；
- ND accept terminal、取消映射、poll `ec.clear()`、空 RequestContext；ibv poll throwing overload 正确传播同步 poll 错误；
- ND scheduler shutdown 销毁 pending connect op 时不再推进 `CompleteConnect`，避免 connector service 已释放 COM 对象后的 UAF；
- `mr_access_flag_t` breaking rename、普通 typo、`small_sglist` throwing allocation 语义；
- event poller 每轮最多四个 batch；ND poller 改为复用 member operation。

验证结果：Windows/ND Debug 与 Release 全量构建通过；14 个纯 unit/compile test 全部通过；ND 显式设备、unopened accept guard、SGL/regression，以及 event/poll move-accept 双进程 echo 均通过。ASan 曾定位到 scheduler shutdown 的 pending connect UAF，修复后 `test_nd_control_cancel` 在 ASan 下连续 10 轮通过。当前主机没有 Linux/ibverbs 构建环境，因此本期保留的 ibv 变更仍需在 Linux CI/真机执行最终 backend 验收。RDMA-010/RDMA-021 当前仍是已知缺陷；在 QP 生命周期专项完成 coordinated teardown 前，不用不安全的提前 callback 掩盖它们。RDMA-019 所定义的“op 完成后旧 signal late emit/地址复用”也仍按决策延期。

## 缺陷清单

### RDMA-001 ND `HANDLE` deleter 条件错误

- 后端：ND
- 优先级：P0
- 位置：`include/rdma/nd/detail/nd_impl_types.hpp:13`
- 现象：`handle_deleter` 使用 `if (handle != INVALID_HANDLE_VALUE || handle != NULL)`。该条件对 `INVALID_HANDLE_VALUE` 和 `NULL` 也会成立，因此 deleter 会调用 `CloseHandle(INVALID_HANDLE_VALUE)` 或 `CloseHandle(NULL)`。
- 影响：当上游失败路径把 invalid handle 放入 `unique_handle_t`，析构或 reset 会关闭无效句柄；再叠加未初始化句柄时，可能关闭随机值。
- 建议修复：改为 `if (handle != nullptr && handle != INVALID_HANDLE_VALUE)`，或抽成 `is_valid_win32_handle()`，所有 Windows handle holder 统一使用。
- 建议测试：新增 Windows-only 单测覆盖 `unique_handle_t{}`、`unique_handle_t{INVALID_HANDLE_VALUE}`、`unique_handle_t{nullptr}` 的 deleter 行为；如果不方便 mock `CloseHandle`，至少引入 helper 并单测 helper。

### RDMA-002 ND `CreateOverlappedFile` 失败返回未初始化句柄

- 后端：ND
- 优先级：P0
- 位置：`include/rdma/nd/detail/impl/nd_device_impl.ipp:20`
- 相关调用点：`include/rdma/nd/impl/nd_completion_queue.ipp:21`、`include/rdma/nd/detail/nd_service_connector.hpp:385`、`include/rdma/nd/detail/nd_service_listener.hpp:107`、`include/rdma/nd/detail/nd_service_listener.hpp:204`、`include/rdma/nd/detail/impl/nd_ops_verbs.ipp:20`、`include/rdma/nd/detail/impl/nd_service_io_completion.ipp:34`、`include/rdma/nd/detail/impl/nd_device_impl.ipp:418`
- 现象：`HANDLE result;` 未初始化，随后无论 `CreateOverlappedFile(&result)` 成功或失败都返回 `result`。多个调用点先 `reset(create_overlapped_file(..., ec))`，再检查 `ec`。
- 影响：失败时 `unique_handle_t` 可能接管未初始化值；结合 RDMA-001，失败路径可能关闭随机句柄或无效句柄。
- 已确认修复规格：失败契约收紧为“失败必须返回 `nullptr`，成功必须返回可拥有的有效 handle”。`create_overlapped_file()` 先把输出初始化为无效值，native 调用失败时保留映射后的原始错误并返回 `nullptr`；native 调用报告成功但输出仍为 `nullptr`/`INVALID_HANDLE_VALUE` 时按 provider contract violation 返回 `rdma_errc::invalid_handle`。调用点必须先保存临时 handle，确认 `!ec` 且 handle 有效后才能 `reset/adopt`，不得先把失败结果放入 RAII holder。
- 建议测试：用可注入/模拟的 `native_context_t` 覆盖“失败且未写输出”“失败但写入垃圾/无效值”“报告成功但返回无效 handle”“正常成功”四条路径；验证失败时调用者不持有 handle，也不会调用 `CloseHandle`。

### RDMA-003 ND `nd_memory_region::slice(addr, len)` 拒绝 MR 起始地址

- 后端：ND
- 优先级：P1
- 位置：`include/rdma/nd/impl/nd_mr.ipp:67`、`include/rdma/nd/impl/nd_mr.ipp:76`
- 现象：地址版本 slice 只在 `ptr_diff > 0` 时转为 offset slice，因此 `slice(mr.addr(), len)` 返回空 buffer。
- 影响：合法的 MR 首地址不能切片；这与 offset 版本 `slice(0, len)` 的语义不一致，也是 PR #1195 中指出过的同类问题。
- 建议修复：改为接受 `ptr_diff >= 0`，并复用 offset 版本做范围检查。更推荐同时处理 RDMA-004，用整数地址比较避免任意指针相减。
- 建议测试：ND MR 单测覆盖 `slice(base, 0)`、`slice(base, n)`、`slice(base + len, 0)`、`slice(base - 1, n)`。

### RDMA-004 ND/ibv MR 边界检查存在溢出与未定义行为风险

- 后端：ND、ibv
- 优先级：P0
- 位置：`include/rdma/nd/nd_mr.hpp:48`、`include/rdma/nd/nd_mr.hpp:60`、`include/rdma/ibv/ibv_mr.hpp:68`、`include/rdma/ibv/ibv_mr.hpp:80`
- 现象：offset 版本使用 `offset + length <= this->length()`，当 `offset + length` 溢出时可能把非法范围判为合法。指针版本先做 `static_cast<char const*>(addr) - static_cast<char const*>(addr_)`，对不属于同一数组对象的指针相减本身就是未定义行为；后续 `diff + length` 也可能溢出。
- 影响：`buffer(mr, offset, n)` / `is_in_mr(addr, n)` 可能接受越界范围，进而生成错误的 local buffer 或 remote addr。
- 已确认修复规格：offset 版本使用 `offset <= length_ && length <= length_ - offset`。指针版本使用 `std::uintptr_t` 做数值边界检查：先验证 MR 的整数地址区间可表示，再判断 `p >= base`，计算整数 `offset = p - base`，最后复用同一套无溢出公式；不得做任意 C++ 指针相减，也不得无保护计算 `base + length_`。
- 平台假设：该实现明确依赖项目当前支持的 Windows/Linux 平坦虚拟地址空间，以及 object pointer 与 `std::uintptr_t` 可往返转换并按数值地址排序的实现约定；这不是纯 ISO C++ 对任意架构的可移植保证。应在共享 range helper 附近写明假设，并在不提供 `std::uintptr_t` 或不满足地址模型的平台拒绝构建/走平台专用实现。
- 边界语义：允许 `[base, base + length_]` 尾端的 zero-length range；尾端非零 range、MR 地址区间本身整数溢出、`nullptr`/foreign pointer 均拒绝。
- 建议测试：ND/ibv 复用同一组 helper 测试，覆盖 `offset = SIZE_MAX`、尾端 `length = 0/1`、`offset = length_ - 1 && length = 2`、base 前地址、foreign pointer、整数地址区间溢出和 zero-length MR。

### RDMA-005 ND/ibv `async_accept` 对“已注册设备但 connector 未打开”缺少保护

- 后端：ND、ibv
- 优先级：P0
- 位置：`include/rdma/nd/detail/nd_service_connector.hpp:225`、`include/rdma/ibv/detail/ibv_service_connector.hpp:245`
- 现象：`async_accept` 在 `device_registered()` 成立时继续执行，但没有先检查 connector 是否已经由 listener `assign()` 或显式 open。ND 会把 `impl.connector_.Get()` 传入 op，并在 `start_accept_op()` 调用 `accept(impl.connector_.Get(), ...)`；ibv 会继续访问 `impl.cm_channel_->fd` 并调用 `start_accept_op()`。
- 影响：典型场景是 `use_device(io, ...)` 后构造默认 connector，直接调用 `async_accept(qp, ...)`。现有代码可能空指针解引用，或把 null native handle 传到 backend。
- 建议修复：在分配 op 后、任何 native handle 使用前增加 `if (!is_open(impl))`，立即完成 `rdma_errc::invalid_handle`。ND 和 ibv 保持相同错误语义。
- 建议测试：现有单测只覆盖未 `use_device` 的 guard；新增“已 `use_device` 但 connector 未 assign/open”的 ND/ibv 单测，handler 应收到 `invalid_handle`，进程不崩溃。

### RDMA-006 参考 Asio move-accept，由 `async_accept` 创建并返回已绑定 QP

- 后端：ND、ibv
- 优先级：P1（接口缺陷）
- 位置：`include/rdma/nd/nd_connector.hpp:162`、`include/rdma/ibv/ibv_connector.hpp:166`、对应 connector service 与 accept op。
- 现象：当前 `async_accept(queue_pair&, ...)` 要求用户先创建并绑定 QP，但 ND 在 bind 时创建 native QP，ibv 则到 accept initiation 才创建 native QP。公共接口向用户暴露了 backend 创建时机差异，也允许把默认构造、绑定失败或 completion target 不符合预期的 QP 传给 accept。
- 本期目标：只新增 async move-accept operation。服务端解析 connection request private data 后只选择数据面的 completion target；库内部创建、绑定并持有 QP，accept 成功后通过 callback 按值返回 move-only QP。
- 建议新增 event-mode overload：
  ```cpp
  template <typename AcceptToken>
  auto async_accept(asio::io_context& qp_io,
                    asio::const_buffer outgoing_private_data,
                    AcceptToken&& token);
  // completion signature: void(asio::error_code, rdma_queue_pair)
  ```
- 建议新增 poll-mode overload：
  ```cpp
  template <typename AcceptToken>
  auto async_accept(rdma_completion_queue& cq,
                    asio::const_buffer outgoing_private_data,
                    AcceptToken&& token);
  // completion signature: void(asio::error_code, rdma_queue_pair)
  ```
- 同时提供省略 `outgoing_private_data` 的 convenience overload；接口形态参考 Asio `async_accept(executor, token)` 返回 move-constructed peer socket 的模式。
- 建议调用顺序：
  1. listener 的 `async_get_connection` 返回 connector，并把 request private data 写入用户 buffer；
  2. 用户解析 private data，选择 `qp_io` 或 `cq`；
  3. 用户调用新的 `connector.async_accept(qp_io/cq, ...)`；
  4. 库内部创建 QP、绑定所选 completion target，并发起 native accept；
  5. accept 成功后，通过 completion handler 按值移交 `rdma_queue_pair`。
- completion domain：`async_accept` 是 connector 的控制面 operation，其完成仍由 connector 所属的 control-plane `io_context` 驱动；传入的 `qp_io` / `cq` 只决定返回 QP 后续 send/recv/read/write 的 data-plane completion route。poll-mode 用户仍需运行 control-plane `io_context` 完成 accept，连接建立后再用 `cq.poll()` 驱动数据面 completion。
- 成功后置条件：
  - event mode：QP 已绑定 `qp_io` 的 shared CQ，event service 有效，native QP 非空；
  - poll mode：QP 已绑定传入 CQ，poll completion sink 有效，native QP 非空；
  - 两种模式都不允许把 partial-bound QP 作为成功结果返回。
- 失败语义：QP bind、native QP 创建或 accept 任一步失败，都通过 connector 已有的 control-plane completion route 异步调用 handler；`ec` 保留具体失败原因，返回的 QP 为空/不可用于数据面。handler 不得在 initiating function 内联执行。
- 生命周期：`rdma_queue_pair` 是 move-only resource；move-accept operation 直接按值持有 QP，并在完成时把它 move 给 handler，不需要额外的 `unique_ptr` / `shared_ptr` 所有权层。operation 必须先在最终异步 operation storage 中构造其 QP 成员，再把该成员用于 native accept；不能保存指向调用栈局部 QP 的裸指针，也不能让跨异步阶段保存的 callback 捕获 QP 内部 implementation 在 move 前的旧地址。传入的 `qp_io` 或 `cq` 必须比成功返回的 QP 及其 pending data-plane operation 活得更久。
- backend 差异：ND 在绑定 completion target 时创建并由 QP 持有 native QP；ibv 先绑定 CQ，再在 accept initiation 中由 connector 创建 native QP 并回填 QP。公共成功后置条件保持一致，内部所有权差异不泄露给用户。
- 兼容策略：本期新增 move-accept overload，不删除、不改变现有 `async_accept(queue_pair&, ...)`；旧接口的长期定位留给独立 QP 生命周期计划决定。
- 明确不在本期：不重构 ibv QP 所有权或创建时机；不改成 `ibv_create_qp`；不设计 pre-post receive；不增加 connected/data-plane guard；不定义未绑定 QP 的 callback executor；不删除默认构造或 deferred bind API。相关内容移入独立计划 `ibv_queue_pair_lifecycle_and_prepost_receive_plan.md`。
- 建议测试：ND/ibv 分别覆盖 event/poll overload；验证 private data 解析后可以选择不同 completion target；成功返回的 QP `is_bound()` 且 `native_handle()` 非空；bind/native accept 失败异步返回且不泄漏 partial QP；QP move 后 completion route 保持正确；poll-mode accept callback 由 control-plane `io_context` 驱动，而数据面 callback 只在 `cq.poll()` 后发生。

### RDMA-007 ND accept 非取消失败不更新 connector 状态

- 后端：ND
- 优先级：P1
- 位置：`include/rdma/nd/detail/nd_op_accept.hpp:43`、`include/rdma/nd/detail/nd_service_connector.hpp:471`
- 现象：`start_accept_op()` 先把状态 CAS 到 `connecting`。`nd_accept_op::do_complete()` 成功时设置 `connected`，`operation_aborted` 时设置 `closed`，但其他失败错误不会把状态从 `connecting` 改走。
- 影响：accept 失败后 connector 会卡在 `connecting`，后续 `async_accept`/`async_connect` 得到 `connector_terminal` 或 disconnect 行为异常；状态机与 `nd_connect_op`、ibv accept 的 failure handling 不一致。
- 建议修复：在 `owner && ec && state_` 时统一设置 `closed`；如果要保留 disconnect race 语义，按 ibv `claim_closed()` 模式用 CAS 处理。
- 建议测试：构造 `accept()` 同步失败或完成失败路径，验证 handler 收到原始错误，同时 connector 后续状态为 terminal/closed。

### RDMA-008 ND poll mode 与 event mode 对取消完成的错误映射不一致

- 后端：ND
- 优先级：P2
- 位置：`include/rdma/nd/detail/impl/nd_service_io_completion.ipp:84`、`include/rdma/nd/impl/nd_completion_queue.ipp:92`
- 现象：event mode 中 `ND_CANCELED` 被映射为 `asio::error::operation_aborted`；poll mode 中 `dispatch_completion()` 直接 `static_cast<nd_errc>(wc.Status)`，handler 会收到 `nd_errc::canceled`。
- 影响：相同 backend 的 event/poll 模式 handler 错误语义不同，取消相关上层逻辑难以复用。
- 建议修复：提取公共 helper，例如 `map_nd_wc_status(HRESULT)`，event/poll 都复用；`ND_CANCELED` 统一为 `operation_aborted`，其他状态保持 ND error。
- 建议测试：ND poll completion 人工喂入 `ND_CANCELED`，验证 handler 收到 `operation_aborted`。

### RDMA-009 ibv no-throw `completion_queue::poll()` / `poll_one()` 忽略错误

- 后端：ibv
- 优先级：P2
- 位置：`include/rdma/ibv/impl/ibv_completion_queue.ipp:29`、`include/rdma/ibv/impl/ibv_completion_queue.ipp:56`
- 现象：无 `error_code&` 的 `poll()` / `poll_one()` 创建局部 `ec` 后直接返回 `poll(ec)` / `poll_one(ec)`，没有在 `ec` 非空时 throw。ND 对应实现会 throw。
- 影响：`ibv_poll_cq` 返回负值时，throwing overload 会静默吞掉错误，API 语义与 ND 不一致。
- 建议修复：镜像 ND：调用 error_code overload 后检查 `ec`，非空则 `asio::detail::throw_error(ec)`。
- 建议测试：mock 或封装 `poll_cq` 失败路径，验证 throwing overload 抛错，`error_code&` overload 不抛。

### RDMA-010 ibv CQ notification arm 失败被忽略

- 后端：ibv
- 优先级：P1
- 位置：`include/rdma/ibv/detail/impl/ibv_service_io_completion.ipp:84`、`include/rdma/ibv/detail/impl/ibv_service_io_completion.ipp:126`
- 现象：`verbs_ops::req_notify_cq(..., ec)` 设置的 `ec` 在 `do_perform()` 和 `arm_poller()` 中都没有处理。
- 影响：如果 re-arm notification 失败，poller 可能没有真正挂到 comp_channel；后续 CQE 可能无人唤醒，表现为异步操作卡住。
- 建议修复：`do_perform()` 中 re-arm 失败应使 poller op 完成错误或触发 shutdown/retry；`arm_poller()` 初次 arm 失败也要把错误投递到 scheduler，避免静默挂起。需要定义清楚“arm 失败后是否继续 drain 已有 CQE”的语义。
- 建议测试：mock `req_notify_cq` 失败，验证不会无限等待，已 pending 的 op 能收到错误或服务进入明确失败状态。
- 2026-07-17 决策：本期不再用 `fail_pending()` 把 notification arm 失败等同于全部 WR 已失败。notification 失败时 CQ 可能仍可 poll；最终实现必须提供有界 retry/backoff 或 polling fallback，并在确认 CQ/QP terminal 后先 quiesce native WR，再完成 handler。实现与验收移入 ibv QP 生命周期专项，当前缺陷保持可见。

### RDMA-011 `use_device(io, explicit_device, config)` 未早期校验 config 兼容性

- 后端：ND、ibv
- 优先级：P2
- 位置：`include/rdma/nd/impl/nd_use_device.ipp:10`、`include/rdma/ibv/impl/ibv_use_device.ipp:10`
- 现象：显式 device 路径直接 `derive_effective_config()` 并初始化 CQ。ibv 已有 `detail::is_config_compatible()`，但 `use_device()` 未调用；ND 的兼容性逻辑在 `detail::is_valid_adapter(adapter, config)`，显式 device 路径也未调用。
- 影响：用户传入超过硬件能力的配置时，错误可能延迟到 CQ/QP 创建或 native 调用，错误码不稳定且难以定位。
- 已确认修复规格：`use_device()` 在 derive 前校验用户非零配置是否超过 caps 或形成 backend/device 不兼容组合；失败统一返回 ND/ibv 共有的公共错误 `rdma_errc::invalid_config`。native 创建阶段的 provider/system failure 保留原始错误，不改写成 `invalid_config`。
- 建议测试：ND/ibv config derive 单测扩展为显式 device use path；ibv 可以先复用已有 `is_config_compatible` 测试夹具。

### RDMA-012 SGE length 存在窄化/截断风险

- 后端：ND、ibv
- 优先级：P1
- 位置：`include/rdma/nd/nd_buffer.hpp:10`、`include/rdma/ibv/ibv_buffer.hpp:11`
- 现象：公共 buffer 的 `length()` 是 `std::size_t`。ibv 填 `ibv_sge.length` 时直接 `static_cast<std::uint32_t>`；ND 填 `BufferLength` 时隐式赋值给 native 字段。当前只检查 SGE 数量，不检查单段长度和总传输长度是否超过 backend/native 类型。
- 影响：超过 32-bit 的 buffer length 会被截断，导致少传、越界访问或 native post 失败；错误不会在库层以清晰 error_code 暴露。
- 已确认修复规格：在 `build_native_sglist()` / 单 buffer fast path 中，于任何 narrowing/cast 和 native post 前校验 segment length；超出 native 表示范围统一返回 ND/ibv 共有的公共错误 `rdma_errc::buffer_too_large`。`built.total_bytes` 使用无溢出累加，累加溢出或超过库明确支持的 operation 上限也返回该错误。
- 建议测试：构造长度大于 `UINT32_MAX` 的 adapted buffer，验证 async op 立即完成错误，不调用 native post。

### RDMA-013 `small_sglist` 的 `error_code` 参数没有真实错误传播

- 后端：通用，影响 ND/ibv
- 优先级：P3
- 位置：`include/rdma/detail/small_sglist.hpp:37`、`include/rdma/detail/small_sglist.hpp:77`
- 现象：`reserve()` 使用 `std::make_unique`，默认异常模式下分配失败会抛异常，不会通过 `ec` 返回；`append_uninitialized()` 调用 `reserve()` 后又无条件 `ec.clear()`。`resize()` 也忽略 `reserve()` 的 `ec`。
- 影响：API 表面看起来支持 no-throw error_code，但实际失败路径不可控；如果未来 no-exception 构建或自定义 allocator，引发语义问题。
- 建议修复：二选一：
  - 明确该类型使用 throwing allocation，移除/弱化 `ec` 参数；
  - 改为 `new (std::nothrow)` 或可注入 allocator，并在 `append_uninitialized()` 尊重 `ec`。
- 建议测试：如果保留 `ec` 语义，加入失败 allocator 测试。

### RDMA-014 ND `WSCGetProviderPath` 第二次调用检查错了错误变量

- 后端：ND
- 优先级：P2
- 位置：`include/rdma/nd/detail/impl/nd_device_impl.ipp:127`
- 现象：第二次 `WSCGetProviderPath()` 失败时，代码检查 `res != 0` 后比较 `res == WSAEINVAL`。该 API 失败返回 `SOCKET_ERROR`，具体错误在 `err`。
- 影响：失败原因会被错误分类，常见错误可能被误报为 `not_enough_memory`，设备枚举/ provider 加载问题难以诊断。
- 建议修复：第二次调用失败时检查 `err`，例如 `if (res == SOCKET_ERROR) { switch (err) ... }`；第一次调用也保持 “返回 SOCKET_ERROR 且 err == WSAEFAULT 表示需要 buffer” 的判断。
- 建议测试：mock `WSCGetProviderPath` 返回 `SOCKET_ERROR + WSAEINVAL` 和 `SOCKET_ERROR + WSAEFAULT`，验证 error_code。

## 补充独立 Review

以下条目不依赖 alibaba/yalantinglibs#1195 的 review 信息，是重新从拼写/API、性能和 op 生命周期角度扫出的补充问题。

### RDMA-015 公共 API 拼写错误：`mr_acccess_flag_t`

- 后端：通用，影响 ND/ibv
- 优先级：P2
- 位置：`include/rdma/rdma_commons.hpp:13`、`include/rdma/rdma.hpp:5`、`include/rdma/nd/nd_mr.hpp:19`、`include/rdma/ibv/ibv_mr.hpp:32`
- 现象：公共枚举类型拼成了 `mr_acccess_flag_t`，`access` 多了一个 `c`。该名字已经进入 MR 构造函数、native wrapper 参数和顶层 include 注释。
- 影响：这是用户可见 API typo。不改会把 typo 固化到文档、示例和下游代码里。
- 用户已确认策略（2026-07-16）：直接 rename 为 `mr_access_flag_t`，同步更新 ND/ibv 构造函数、native wrapper 参数、注释和测试；不保留 deprecated compatibility alias。
- 建议测试：编译测试覆盖新名字，并增加负向/检索检查确保旧拼写从公共头文件、测试、示例和文档中完全消失。

### RDMA-016 源码中存在多处普通拼写/命名错误

- 后端：主要是 ND
- 优先级：P3
- 位置：`include/rdma/nd/detail/nd_op_connect.hpp:69`、`include/rdma/nd/detail/nd_op_accept.hpp:27`、`include/rdma/nd/detail/impl/nd_device_impl.ipp:171`、`include/rdma/nd/detail/impl/nd_device_impl.ipp:211`、`include/rdma/nd/detail/nd_ops_cm.hpp:8`、`include/rdma/nd/detail/nd_ops_verbs.hpp:21`
- 现象：存在 `conncetor`、`provier_module`、`provdier`、`simular`、`poset recv` 等 typo。
- 影响：多数是局部变量、参数或注释，不改变 ABI，但会影响代码检索和维护质量。
- 建议修复：局部变量/参数/注释直接改；公共 API 的 typo 按 RDMA-015 的直接 rename 策略处理。
- 建议测试：无需专门运行时测试，编译覆盖即可。

### RDMA-017 `async_read/write` initiation lambda 按引用捕获 `remote_addr`

- 后端：ND、ibv
- 优先级：P1
- 位置：`include/rdma/nd/nd_queue_pair.hpp:177`、`include/rdma/nd/nd_queue_pair.hpp:195`、`include/rdma/ibv/ibv_queue_pair.hpp:146`、`include/rdma/ibv/ibv_queue_pair.hpp:164`
- 现象：one-sided API 的 initiation lambda 使用 `[this, &remote_addr]`。对于普通 eager token，initiation 通常立即执行，风险不明显；但对 lazy/deferred token 或用户传入临时 `remote_addr` 时，lambda 可能在 initiating function 返回后才执行，引用会悬空。
- 影响：`async_write(buffers, mr.remote_addr(...), asio::deferred)` 这类写法可能形成 use-after-scope。op 本身会按值保存 `remote_addr_`，问题发生在 op 构造之前的 initiation lambda。
- 建议修复：把 lambda 改为按值捕获 `remote_addr`，例如 `[this, remote_addr]`。`rdma_remote_addr_t` 很小，复制成本低。
- 建议测试：新增 `deferred`/lazy token 编译与运行测试，使用临时 remote_addr 发起 read/write，确认没有悬空引用。

### RDMA-018 data-plane BufferSequence 描述生命周期与重复遍历（合并原 RDMA-023）

- 后端：通用，影响 ND/ibv
- 优先级：P1
- 位置：`include/rdma/detail/rdma_verbs_op.hpp:68`、`include/rdma/detail/rdma_op_send.hpp:42`、`include/rdma/detail/rdma_op_write.hpp:43`、`include/rdma/rdma_buffer.hpp:25`、`include/rdma/nd/nd_buffer.hpp:23`、`include/rdma/ibv/ibv_buffer.hpp:24`
- 现象：Asio 风格的 buffer API 不拥有用户 payload 内存，这一点应保持；这里的问题不是要 RDMA 库管理 payload 生命周期，而是当前 op 会把 `BufferSequence` 描述对象保存到完成阶段。对 `std::vector`、`std::array`、单个 buffer 来说，按值保存通常会复制描述元素；但对 `std::span`、`std::initializer_list`、ranges view 或自定义非 owning sequence，按值保存的只是指针/迭代器视图。`rdma_buffer.hpp` 注释还提到 `initializer_list -> real scatter/gather`，容易让用户以为临时 SGL 描述也安全。
- 具体风险：native post 通常同步消费 SGE 数组，理论上 SGL 描述只需活到 post 完成；但当前 `rdma_send_op` / `rdma_write_op` 在 completion path 再次调用 `buffer_size(o->get_buffer_sequence())`，如果 sequence backing storage 已经失效，就会在 handler 前访问悬空 view。
- 合并的性能问题：multi-SGE post 时 `build_native_sglist()` 已经计算 `built.total_bytes`，但 send/write completion 又调用 `buffer_size()` 做第二次 O(n) 遍历；这既是原 RDMA-023 的热路径开销，也是悬空 view 被再次访问的直接原因。
- 已确认修复规格：保持 Asio 语义，即用户 payload 内存必须活到 handler，库不复制 payload。send/write 在实际 initiation/post 阶段使用无溢出累加缓存 `total_bytes` 到 op；completion 只读取缓存值，不再访问或遍历 `BufferSequence`。非 owning SGL view 的 backing storage 必须活到实际 initiation/post 完成；deferred token 以真正执行 initiation 的时点计算。
- 可选收紧：对显式 `std::initializer_list` / 明显临时 view 增加 overload 或约束，避免 API 注释鼓励危险用法。
- 建议测试：覆盖 `std::span`/`initializer_list` 描述对象在 post 后销毁但 payload 仍有效的 send/write 场景，修复后 completion 不再读取悬空 sequence；测试 deferred token 在实际 initiation 时缓存；用可计数 range 验证 completion 不做第二次遍历；micro benchmark 仅作为性能佐证，不作为正确性修复的前置门槛。

### RDMA-019 控制面 cancellation_slot handler 持有裸 op 指针，完成后 late emit 可能命中复用地址

- 后端：ND、ibv
- 风险等级：P1 correctness/lifetime（显式延期，非“后续优化”）
- 位置：`include/rdma/nd/detail/nd_ops_cm.hpp:59`、`include/rdma/ibv/detail/ibv_op_cm.hpp:20`
- 范围边界：只覆盖控制面/CM op 的 per-op cancellation，例如 connect、accept、get_connection、wait_disconnect。**不包含** queue_pair 数据面 `async_send/async_recv/async_read/async_write` 的单 IO 操作取消；本期也不设计 data-plane per-WR cancellation。
- 复核结论：边界收窄后问题仍然存在，但只属于控制面 cancellation_slot 生命周期问题。现有 `test_*_control_cancel` 覆盖的是“op pending 时 emit 能取消当前 op”，没有覆盖“op 已完成后旧 signal late emit 是否会误取消后续控制面 op”。
- 现象：ND 的 `nd_cm_op_cancellation` 保存 `HANDLE + LPOVERLAPPED`，ibv 的 `cm_op_cancellation` 保存 `fd + key(op pointer)`。op 完成后，代码没有清空或失效 cancellation slot 中的 handler；如果用户复用同一个 `cancellation_signal` 并在 op 完成后 emit，slot 里可能仍保留旧 op 地址。
- 影响：常见情况下 late emit 只是找不到 pending op；但如果 allocator 复用了相同 op 地址，并且 native handle/fd 也仍然有效，late emit 可能取消另一个与该 signal 无关的新 op。ND 还会把已释放的 OVERLAPPED 地址传给 `CancelIoEx` 作为 key。
- Asio socket 对照：reactive socket 使用 slot 中的 cancellation functor 地址作为 `cancellation_key_`，取消时按该 key 过滤 reactor queue；Windows IOCP socket 使用 slot-owned cancellation wrapper 作为 `CancelIoEx` 的 OVERLAPPED，而不是使用已释放 target op 地址。
- 可照搬程度：ibv 控制面是 reactor fd + reactor op，和 Asio reactive socket 形态一致，可以几乎按同一 key 方案实现。ND 控制面是 IOCP/OVERLAPPED，并且 connect 可能跨 `Connect` / `CompleteConnect` 多阶段复用/重置 OVERLAPPED，不能简单照搬一份 socket wrapper 代码；如果要完全贴近 Asio Windows socket，需要把所有 ND 控制面的 native submit 对象从 target op 改成独立 submit wrapper，并让 wrapper completion 转发到 target op。
- 用户已确认策略（2026-07-16）：本期不做。该决定是对已知 P1 correctness/lifetime 风险的显式延期，不改变其风险等级，也不能把它归类为性能优化或普通整理。保留当前控制面 cancellation_slot 行为，本期不引入 generation/liveness token，也不重构 ND submit wrapper。
- 后续方向：和 RDMA-026 一样进入专项迭代。ibv 可按 Asio reactive socket 风格改 `cancellation_key_`；ND 需要在 generation/liveness token 和独立 submit wrapper 之间做设计取舍。
- 建议测试：同一个 `cancellation_signal` 发起控制面 op A，A 完成后不重新绑定 slot，制造 op 地址复用，再 emit，验证不会取消控制面 op B。不新增数据面单 IO 取消语义测试。

### RDMA-020 ND `completion_queue::poll(ec)` / `poll_one(ec)` 不清理 `ec`

- 后端：ND
- 优先级：P2
- 位置：`include/rdma/nd/impl/nd_completion_queue.ipp:49`、`include/rdma/nd/impl/nd_completion_queue.ipp:72`
- 现象：error_code overload 没有在成功或无事件时 `ec.clear()`。如果调用者复用一个已经带错误的 `ec`，`poll(ec)` 返回后 `ec` 仍然是旧错误。
- 影响：用户会把一次成功 poll 误判为失败；与 ibv 的 `poll(ec)` 行为不一致。
- 建议修复：进入函数时先 `ec.clear()`。如果未来 ND poll 能暴露 native poll 错误，再在错误路径设置。
- 建议测试：先把 `ec` 设为非空，再调用空 CQ 的 `poll(ec)` / `poll_one(ec)`，期望 `ec` 为空。

### RDMA-021 ibv event-mode CQ poller 忽略 `ibv_poll_cq` 失败

- 后端：ibv
- 优先级：P1
- 位置：`include/rdma/ibv/detail/impl/ibv_service_io_completion.ipp:106`
- 现象：`poll_into()` 直接 `do { n = poll_cq(...); for (i < n) ... } while (n > 0)`。当 `ibv_poll_cq` 返回负数时，函数既不记录错误，也不完成 pending op，更不使 poller 进入失败状态。
- 影响：CQ poll 失败会被静默吞掉，随后 poller 继续 re-arm；用户侧表现可能是 op 永远不完成或只看到后续超时。
- 建议修复：让 `poll_into()` 返回 `error_code` 或把错误存到 poller op；遇到 `n < 0` 时完成当前可定位的 pending op 或触发 service-level failure。至少要 log/断言并停止无限 re-arm。
- 建议测试：mock `poll_cq` 返回 -1，验证 event-mode async op 不会永久挂住。
- 2026-07-17 决策：`ibv_poll_cq() < 0` 使 CQ progress 不可信，但不证明所有 WR 已停止。撤回“遍历 software pending list 并立即 callback”的实现；下一期由 CQ 标记 terminal、关联 QP 停止 post 并转 ERROR/销毁、处理 late CQE，在 native quiescence 后才以错误和 `bytes_transferred == 0` 收敛残余 handler。详细规格见 ibv QP 生命周期专项。

### RDMA-022 ND event-mode `resolve_wc` 对空 RequestContext 只有 assert

- 后端：ND
- 优先级：P2
- 位置：`include/rdma/nd/detail/impl/nd_service_io_completion.ipp:84`
- 现象：event-mode `resolve_wc()` 只有 `assert(result.RequestContext)`，release 下会继续把空指针转成 op 并解引用；poll-mode `nd_completion_queue::dispatch_completion()` 已经有 `if (!wc.RequestContext) return;`。
- 影响：异常/外部 CQE 或 native bug 会导致 release 崩溃；同一 backend 的 event/poll 防御行为不一致。
- 建议修复：event-mode 与 poll-mode 一样显式判空；必要时计数或返回错误给 service。
- 建议测试：人工构造空 `RequestContext` 的 completion，确认 event-mode 不崩溃。

### RDMA-024 event-mode CQ poller 每次 drain 到空，极端负载下可能影响调度公平性

- 后端：ND、ibv
- 优先级：P3（性能/公平性）
- 位置：`include/rdma/nd/detail/impl/nd_service_io_completion.ipp:107`、`include/rdma/ibv/detail/impl/ibv_service_io_completion.ipp:106`
- 现象：event-mode poller 每次被唤醒后都循环 poll 到 CQ 为空。`cq_poll_batch_` 只限制单次 native poll 的数组大小，不限制本轮最多处理多少 CQE。
- 影响：在 CQ 持续非空的压力场景下，一个 poller completion 可能长时间占用 `io_context` 线程，影响 timer、CM 控制面事件或其他 socket 任务的尾延迟。吞吐优先时这是合理取舍，但需要可配置。
- 建议修复：增加 `max_completions_per_turn` 或复用 `cq_poll_batch_` 作为每轮 budget；达到 budget 后把 poller 重新 post/arm，让 scheduler 有机会执行其他任务。
- 建议测试：压测 send/recv 同时跑 timer/CM 事件，观察尾延迟。

### RDMA-025 ND event-mode CQ poller 每次 re-arm 都分配新 op

- 后端：ND
- 优先级：P2（性能）
- 位置：`include/rdma/nd/detail/impl/nd_service_io_completion.ipp:123`
- 现象：`arm_poller()` 每次创建新的 `nd_poll_wc_op`，completion 后立即释放，再重新分配。ibv event-mode 则复用 service 成员 `poller_`。
- 影响：CQ 通知频繁时会产生持续 allocator churn，并增加 cache miss。虽然使用 Asio handler allocator，但这里的 dummy handler 没有用户 allocator，通常会走默认分配。
- 建议修复：评估把 `nd_poll_wc_op` 改为 service 成员复用，或至少引入小对象池。需要确认 Windows IOCP/OVERLAPPED 在完成前不可复用的约束，确保同一时刻只有一个 notify outstanding。
- 建议测试：ND echo/stress benchmark 观察 allocation count 和 CPU。

### RDMA-026 ibv 数据面全部 signaled，吞吐上限可能受限

- 后端：ibv
- 优先级：P3（后续 batch op 专项，非本期）
- 位置：`include/rdma/ibv/detail/impl/ibv_service_verbs.ipp:41`、`include/rdma/ibv/detail/ibv_ops_verbs.hpp:89`、`include/rdma/ibv/detail/ibv_ops_verbs.hpp:110`
- 现象：QP 创建时 `sq_sig_all = 1`，post send/rdma 时也设置 `IBV_SEND_SIGNALED`。这保证每个 async op 都有 CQE，但也让所有 send/write 都产生 completion。
- 影响：高吞吐写/发场景下，CQE 和 poller 压力会变成瓶颈。成熟 verbs 应用通常会对部分 WR unsignaled，并周期性 signaled 来回收/推进。
- 本期策略：不做。保持 `sq_sig_all = 1` 和当前 per-op signaled completion 语义；不新增 fire-and-forget/unsignaled API。
- 后续方向：和 batch op 一起设计 unsignaled/batching 语义，统一处理 pending op 队列、signaled marker、错误传播、flush、断连、SQ backpressure 和 benchmark。
- 建议测试：后续 batch op 专项中再 benchmark 对比 all-signaled、fire-and-forget 和周期性 signaled batching 的吞吐、延迟、CQE 数量。

### RDMA-027 ND scheduler shutdown 会让 pending connect op 访问已释放 connector（施工期发现并修复）

- 后端：ND
- 优先级：P1 correctness/lifetime
- 位置：`include/rdma/nd/detail/nd_op_connect.hpp`、`include/rdma/nd/detail/nd_service_connector.hpp`
- 现象：execution context shutdown 先调用 connector service 的 `shutdown()` 释放 `IND2Connector`，随后 IOCP scheduler 以 `owner == nullptr` destroy pending `nd_connect_op`；旧的 `do_complete()` 仍会执行 `resume_process()`，进而调用 `GetPrivateData` / `CompleteConnect`。
- 影响：确定性 heap-use-after-free；非 ASan 构建表现为 `integration_nd_control_cancel` 偶发崩溃。
- 修复：`owner == nullptr` 只释放 operation/handler storage，不再推进多阶段 native connect；这不改变 owner 非空时的正常 connect、取消或 completion 语义，也不替代 RDMA-019 的 cancellation-slot 生命周期专项。
- 验收：新增 owner-null 单测使用不可解引用 connector 哨兵；ASan 修复前稳定报告 `nd_connect_op_base::process_complete_connect` UAF，修复后 `test_nd_control_cancel` 连续 10 轮通过。

## 计划复核结论

原先的 Phase 计划能表达大方向，但混在一起的维度较多：有的是崩溃/生命周期风险，有的是错误语义统一，有的是 API 兼容决策，还有的是性能优化。建议后续执行时同时维护两张视图：

- 按重要等级排交付顺序，确保 P0/P1 的安全问题先落地。
- 按问题分类组织代码改动，方便一次性抽公共 helper、补同类测试、分配 ND/ibv 所有权。

默认执行原则：

- 不先做大重构；先用小而确定的 guard、range check、error mapping 把 crash/hang 收敛。
- ND 和 ibv 的同类行为尽量同一轮修，避免接口语义继续分叉。
- 需要改变公共 API 或 async 语义的点先加兼容层和文档，再考虑 breaking change。
- 性能优化不压过正确性；除非优化不改变 handler completion 语义。

计划使用方式：

- 先按“重要等级的执行队列”决定交付顺序，保证 P0/P1 不被性能和清理工作挤掉。
- 实际开 PR 或分支时按“问题分类的实施包”拆分，这样同一类 helper、测试夹具和 backend 差异可以一次处理。
- 每个 milestone 都应至少包含测试或可复现用例；暂时不能接真机的项，先落 unit/mock，再标记真机验收缺口。

实施元数据要求：每个条目进入 patch 前，在 issue/PR 描述中补齐以下字段；本文不为 26 个条目重复空模板。

- 证据状态：源码确认 / unit 可复现 / mock 可复现 / 仅真机可确认；
- 完成条件：handler、error_code、状态机和资源所有权的可观察后置条件；
- 兼容性影响：source/ABI/async 时序是否变化；
- 依赖决策：引用本节已确认决策或明确剩余确认人；
- 建议 PR 边界：不得把 correctness、breaking API 和大性能重构无理由混为一个 patch；
- 目标平台：Windows ND、Linux ibv 或共享；
- 测试能力：现有 seam、需新增 native hook、或必须真机/provider 验收。

## 已确认决策与剩余确认点

以下产品/API 决策均由用户确认；此前文档缺少决策来源记录。本轮统一标注为“用户已确认（2026-07-16）”。没有标注为已确认的技术细节仍需在对应 patch 前通过源码、mock 或 provider 测试验证，不能由计划生成者代替用户决定公共语义。

1. 公共 typo `mr_acccess_flag_t` 是否允许 breaking rename？
   用户已确认（2026-07-16）：这是拼写错误，直接 rename 为 `mr_access_flag_t`，不保留 deprecated compatibility alias。
   影响范围：`rdma_commons.hpp`、顶层注释、ND/ibv MR 构造函数、native verbs wrapper 参数、相关测试和示例。
   阻塞性：不阻塞 P0/P1；执行 RDMA-015 时按直接 rename 处理。

2. data-plane `BufferSequence` 生命周期策略选哪一种？
   用户已确认（2026-07-16）：按 Asio 语义处理，库不拥有用户 payload 内存；RDMA-018 与原 RDMA-023 合并为一个正确性/性能修复规格。
   需要澄清：这里不建议 RDMA 库拥有用户 payload 内存；payload lifetime 仍按 Asio 语义由用户保证到 handler。具体问题是库当前把 `BufferSequence` 描述对象保存到 op，并在 send/write completion path 再次遍历它计算 bytes。如果传入的是 `span`、`initializer_list` 或 view，描述对象的 backing storage 可能已经失效。
   当前策略：send/write 在实际 initiation/post 阶段无溢出地缓存 `total_bytes`，completion path 不再读取 `BufferSequence`；文档补充 payload lifetime 和 deferred token 下 view 的约束。只有在明确要支持临时 SGL view 时，才考虑 materialize/copy 描述元素；不复制 payload。
   阻塞性：决策已冻结，不再阻塞 RDMA-018；实现仍需补可计数 range 与 deferred token 测试。

3. 是否新增错误码？
   用户已确认（2026-07-16）：新增专用公共错误码，并委托本轮复核它们是否应为 ND/ibv 共有语义。
   复核结论：是。`rdma_errc` 定义于公共 `rdma_error.hpp`，本来就是 ND/ibv 共享的 library-level error category；`nd_errc` 和 ibv/system error 继续表示 backend/native 失败。以下新错误追加到 `rdma_errc`，两端使用同一枚举值、message 和判定语义，不复用 native category。
   `buffer_too_large`：任一 SGE length 超过 backend 可表示的 native 字段上限，或 total bytes 的安全累加溢出/超过库明确支持的 operation 上限。它不表示 SGE 个数超限；后者继续使用 `too_many_sge`。ND/ibv 必须在 native narrowing/cast 或 post 之前返回该错误。
   `invalid_config`：用户显式提供的 `rdma_config_t` 非零约束超过所选设备 capabilities，或字段组合在该 backend/device 上内部不兼容。它不用于 native 创建阶段的随机 provider failure；后者保留原始 backend/system error。
   稳定性：两者属于稳定公共 API。实现时只在枚举末尾追加值以保持既有数值，不改变既有 error code；同步更新 message、文档以及 ND/ibv 对照测试。
   当前策略：本期新增 `buffer_too_large`、`invalid_config`。`operation_not_connected` 与未绑定 QP 的错误交付语义移入独立 QP 生命周期计划，本期 RDMA-006 move-accept 不新增该错误码。
   阻塞性：公共语义已冻结，不再阻塞 RDMA-011、RDMA-012；backend 原生能力上限的取值仍需实现时核验。

4. cancellation_slot late emit 是否作为必须修复项？
   用户已确认（2026-07-16）：本期不做，但 RDMA-019 保持 P1 correctness/lifetime 风险，不再与性能优化混为一类。后续必须作为控制面 cancellation 正确性专项处理。
   范围边界：只覆盖控制面/CM op 的 per-op cancellation，例如 connect、accept、get_connection、wait_disconnect。不能包含 queue_pair 数据面 `async_send/async_recv/async_read/async_write` 的单 IO 操作取消；本期不引入 data-plane per-WR cancellation 语义。
   具体问题：当前 ND cancellation functor 保存 `HANDLE + OVERLAPPED*`，ibv cancellation functor 保存 `reactor_data/fd + op pointer key`。op 完成或销毁后，用户持有的 `cancellation_signal` 里的 slot handler 不会自动清空；如果用户之后再 emit，同一个 functor 仍可能拿旧裸指针去取消。
   风险窗口：多数时候 late emit 只会找不到 pending op；但如果 allocator 复用了相同 op/OVERLAPPED 地址，且 handle/fd 仍有效，就可能取消一个与该 signal 无关的新 op。ND 侧还会把已释放 OVERLAPPED 地址传给 `CancelIoEx`。
   Asio socket 对照：reactive socket 会把 `reactor_op_cancellation` emplace 到 handler 的 cancellation slot，并把 op 的 `cancellation_key_` 设为该 slot functor 对象地址；取消时用 `cancel_ops_by_key(..., this)`。这样 late emit 不会因为 op 指针复用而命中无关 op。Windows IOCP socket 也把 slot 中的 cancellation wrapper 作为可取消对象；`CancelIoEx` 使用 wrapper 自身的 OVERLAPPED 地址，而不是已释放 target op 地址。
   可实现性结论：可以参考 Asio 做到，但不是两个 backend 都照搬同一份实现。ibv 路径基本可以直接照 Asio reactive socket 的 key 方案；ND 路径因为直接提交 OVERLAPPED，且 connect 有多阶段 continuation，更适合先做 generation/liveness token。完整 submit wrapper 方案需要把 ND 控制面 native submit、completion forwarding、continuation re-arm 一起重构。
   后续方向：ibv 侧可改成 Asio reactive socket 风格：`op->cancellation_key_` 使用 slot cancellation functor 的地址，不再使用 op 指针。ND 侧需要在 generation/liveness token 和独立 submit wrapper 之间做设计取舍；独立 submit wrapper 方案需要作为更大重构评估。
   阻塞性：不阻塞本期已选交付，但这是显式风险接受；必须进入后续 correctness backlog，并在发布说明/剩余风险中可见。

5. ibv unsignaled/batching 是否保持每个 async op 必有独立 completion？
   用户已确认（2026-07-16）：本期不做。保持当前 all-signaled/per-op completion 语义，不调整 `sq_sig_all`，不新增 fire-and-forget/unsignaled API。
   具体问题：当前 `sq_sig_all = 1`，同时 send/write/read wrapper 也设置 `IBV_SEND_SIGNALED`，所以每个 send/read/write 都产生 CQE。这最简单，能保证每个 async op 都自然收到 completion；但高吞吐时 CQE 数量、poller CPU、CQ depth 和 cache miss 会成为瓶颈。
   为什么不能直接改：如果某些 WR unsignaled，就没有一一对应 CQE；但 Asio async op 仍要求每个 op 最终调用 handler。要做 batching，就必须在库内部维护 unsignaled op 队列，用周期性 signaled WR 的 completion 推进并批量完成之前的 op，还要处理错误、flush、断连、顺序和尾部没有 marker 的情况。
   后续 batch op 专项再评估的 API 分层：
   - 保留现有 `async_send/async_write`：始终 signaled，始终调用 completion handler，适合通用 Asio 语义。
   - 新增 `try_post_send` / `try_post_write` 或 `post_send_fire_and_forget` / `post_write_fire_and_forget`：同步返回 `error_code` 或 `result<size_t>`，只表示 WR 是否成功提交到 SQ，不表示远端是否收到或写入完成。
   - 可选新增 `rdma::fire_and_forget` tag：调用点必须显式写出该 tag，避免 `detached`、`deferred`、`use_awaitable` 等 token 被误判。
   - 后续如要 batching async completion，再另设 `throughput_mode` / `unsignaled_batch_size` 配置，内部维护 pending op 队列和 signaled marker，不和 fire-and-forget 混为一谈。
   后续语义限制：
   - 优先只支持 send/write。recv/read 通常需要 CQE 才知道数据到达、字节数、远端错误和本地 buffer 何时可复用，不适合普通 fire-and-forget。
   - fire-and-forget 返回成功只代表 post 成功；WR 后续可能被 flush、QP error 或远端错误影响，库不会为该 WR 单独回调。
   - 用户必须自己保证 buffer/MR 在 HCA 可能访问期间有效；因为没有 per-WR completion，安全复用时间只能由上层协议、后续 signaled marker、QP drain/disconnect 或显式 flush 规则确定。
   - 需要 SQ backpressure 策略：如果连续 unsignaled post 填满 SQ，后续 post 只能同步返回错误，或要求用户周期性调用 signaled marker 来回收/观察进度。
   - 如果提供 signaled marker API，例如 `async_flush_send_queue()` 或 `async_signal()`, marker completion 只证明它之前同一 QP send queue 上的 WR 已按 verbs ordering 推进，不等价于每个 fire-and-forget WR 的独立错误回调。
   后续实现要点：
   - ibv QP 创建应考虑 `sq_sig_all = 0`，否则单个 WR 的 `IBV_SEND_SIGNALED` 开关没有意义。
   - 普通 async op 显式设置 `IBV_SEND_SIGNALED`；fire-and-forget path 不设置。
   - fire-and-forget path 不创建 `rdma_*_op`，`wr_id` 可以为 0 或调试用 cookie，completion path 不应期待能 resolve 到 op。
   - CQ poller 需要能忽略或诊断没有 op 的 completion，避免 marker/特殊 WR 影响普通 completion 分发。
   后续测试验收：
   - 编译/API 测试确保 `async_write(..., asio::detached)` 仍走 signaled async op。
   - mock verbs 测试验证普通 async WR 带 `IBV_SEND_SIGNALED`，fire-and-forget WR 不带。
   - 压测验证连续 fire-and-forget 的 SQ 满时错误可见，周期性 marker 能恢复可观察进度。
   阻塞性：RDMA-026 从本期移出，后续和 batch op 一起设计；不阻塞 P0-P3 本期修复。

6. 测试环境怎么安排？
   用户已确认（2026-07-16）：按 unit/mock 先行、真机/provider 做 backend 验收。
   具体拆分：
   纯 unit/compile：拼写 rename、MR range overflow、SGE length 边界、error_code mapping、poll overload 语义、BufferSequence completion 不再遍历、状态 guard。
   mock/native hook：本期覆盖 `CreateOverlappedFile` 失败和 ND provider path 错误分类；`req_notify_cq` 失败、`ibv_poll_cq` 返回负值及其 QP quiescence 验收移入 ibv QP 生命周期专项。控制面 cancellation late emit race 随 RDMA-019 延期，本期不建设该测试。
   真机/provider：ND IOCP/event CQ 行为、ibv comp_channel/CQ re-arm、真实 connect/accept/send/recv/read/write smoke 和 stress。
   默认建议：先补不依赖 RDMA 硬件的 unit/mock 测试，确保 P0/P1 能快速回归；ND 真机和 ibv 真机测试作为每个 backend 完成后的验收，而不是阻塞所有小修。
   阻塞性：不阻塞文档和 unit/mock 修复；会影响每个 backend 的最终验收。

7. 连接前 pre-post receive 与 QP 生命周期怎么处理？
   用户已确认（2026-07-16）：本期不做；本期 RDMA-006 只实现 async move-accept。ND/ibv 的 pre-post receive 对齐、ibv self-created QP、QP ownership/flush/drain 放入独立计划 `ibv_queue_pair_lifecycle_and_prepost_receive_plan.md`。
   阻塞性：不阻塞本期 move-accept；阻塞下一期高级 pre-post receive API 和 ibv QP 生命周期重构。

## 按重要等级的执行队列

### P0：先挡崩溃、随机句柄关闭和边界越界

目标：任何错误使用都应通过 handler/error_code 返回，不应 crash、关闭随机句柄或接受越界 MR range。

1. RDMA-001 / RDMA-002：修 ND handle deleter、`CreateOverlappedFile` 未初始化返回值和 adopt 顺序。
2. RDMA-004：修 ND/ibv MR range check，消除 `offset + length` 溢出和任意指针相减 UB。
3. RDMA-005：修 ND/ibv `async_accept` 在 connector 未 open/assign 时缺少 guard。

验收重点：失败路径不关闭随机句柄，MR 越界输入不产生 UB，connector 未 open/assign 时 accept 不崩溃。

### P1：修生命周期、状态机和会导致 hang 的问题

目标：异步 op 生命周期清楚，连接状态机不会卡死；需要 QP coordinated teardown 的 CQ terminal failure 从本期拆出。

1. RDMA-006：新增 ND/ibv async move-accept overload，由库创建并在完成时返回已绑定 QP。
2. RDMA-017：`async_read/write` lambda 改为按值捕获 `remote_addr`。
3. RDMA-018：明确并修复 non-owning BufferSequence / `initializer_list` 的悬空风险。
4. RDMA-007：ND accept 非取消失败统一把 connector 推到 terminal/closed。
5. RDMA-012：SGE segment length 和 total bytes 做溢出/窄化校验。
6. RDMA-003：ND `slice(base, len)` 语义修正；可和 RDMA-004 同 patch。

验收重点：move-accept event/poll API、deferred token、临时 remote_addr和 accept 失败都有可观察测试；本期不要求 RDMA-010/RDMA-021 coordinated teardown 或 RDMA-019 late-emit race 测试。

### P2：统一错误语义、配置校验和诊断质量

目标：ND/ibv、event/poll、throwing/error_code overload 的行为一致；用户配置错误尽早暴露。

1. RDMA-008：ND event/poll `ND_CANCELED` 映射统一。
2. RDMA-009 / RDMA-020：ND/ibv completion_queue poll overload 的 throw/`ec.clear()` 语义统一。
3. RDMA-022：ND event-mode `RequestContext` 判空与 poll-mode 一致。
4. RDMA-011：`use_device(io, explicit_device, config)` 早期校验能力兼容性。
5. RDMA-014：ND provider path 错误分类检查 `err` 而不是 `res`。
6. RDMA-015：公共 API typo 直接 rename 为正确拼写。
7. RDMA-025：降低 ND poller 重复分配；原 RDMA-023 已并入 P1 的 RDMA-018。

验收重点：错误码对照表、config derive tests、poll overload tests、ND/ibv 行为对齐。

### P3：整理、文档和性能专项

目标：减少维护成本，补齐文档，评估不适合混入正确性修复的大性能改动。

1. RDMA-016：清理普通拼写错误。
2. RDMA-013：明确 `small_sglist` allocation 语义，或实现真正 no-throw `ec`。
3. RDMA-024：评估 event-mode CQ poller per-turn budget，改善极端负载公平性。
4. 补文档：queue_pair `bound` / `native_handle` / `connected` 的区别，BufferSequence lifetime，错误码策略。
5. RDMA-026：仅记录为后续 batch op 专项；本期不实现、不 benchmark、不调整现有 all-signaled 语义。

验收重点：文档和低风险性能项的 benchmark 数据，不要求和 P0/P1 同一批合入；RDMA-026 只要求本期明确边界和后续方向。

### 显式延期的 correctness/lifetime 风险

1. RDMA-019：风险等级保持 P1，本期经用户确认不实现、不建设 late-emit race 测试；不得把它列入 P3 性能/整理项。后续控制面 cancellation 专项必须处理 stale slot、地址复用误取消和 ND `CancelIoEx` 悬空 OVERLAPPED 风险。
2. ibv QP 生命周期与 pre-post receive：本期只做 RDMA-006 move-accept，下一期按独立计划推进 self-created QP、ownership、flush/drain 与 provider 验收。
3. RDMA-010/RDMA-021：临时 `fail_pending()` 已撤回；notification fallback、CQ terminal state、bound-QP registry、pending WR tracking、QP ERROR/销毁、late CQE 和 native quiescence 必须在上述 ibv QP 生命周期专项中一起完成。

## 按问题分类的实施包

### A. Windows/ND 句柄和 provider 失败路径

- 覆盖条目：RDMA-001、RDMA-002、RDMA-014、RDMA-025。
- 推荐顺序：先修 handle deleter 和 `CreateOverlappedFile`，再修 provider path 错误分类，最后评估 poller op 复用。
- 主要文件：`nd_impl_types.hpp`、`nd_device_impl.ipp`、ND listener/connector/CQ 初始化路径、`nd_service_io_completion.ipp`。
- 测试策略：mock/注入失败路径，确认不 adopt 未初始化 handle；ND 真机 smoke 测 CQ/listener/connector。

### B. MR/buffer/SGL 边界和生命周期

- 覆盖条目：RDMA-003、RDMA-004、RDMA-012、RDMA-018（含原 RDMA-023）。
- 推荐顺序：先抽 range check helper，再做 SGE length 校验；BufferSequence 先选短期策略，最后缓存 total bytes。
- 主要文件：`rdma_buffer.hpp`、`nd_mr.hpp`、`ibv_mr.hpp`、`nd_buffer.hpp`、`ibv_buffer.hpp`、`rdma_verbs_op.hpp`。
- 测试策略：纯 unit 覆盖 overflow、foreign pointer、临时 initializer_list/deferred、超大 segment。

### C. connector/QP 状态机和 op 生命周期

- 覆盖条目：RDMA-005、RDMA-006、RDMA-007、RDMA-017。
- 推荐顺序：先实现 move-accept 的公共 completion signature 与 QP 按值所有权，再修 ND accept 状态，最后处理 `remote_addr` 捕获。
- 主要文件：ND/ibv queue_pair、connector service、connect/accept/wait op。
- 测试策略：move-accept event/poll API 与失败路径测试 + ND accept 状态机测试；pre-post receive 不在本实施包内。

### D. completion queue、poller 和错误语义

- 本期覆盖条目：RDMA-008、RDMA-009、RDMA-020、RDMA-022、RDMA-024；延期条目：RDMA-010、RDMA-021。
- 推荐顺序：本期统一 poll overload、ND cancel mapping 和 poller budget；ibv notify/poll terminal error 随 QP 生命周期专项处理。
- 主要文件：ND/ibv completion_queue、io_completion_service、completion status mapping helper。
- 测试策略：本期做 event/poll overload 与错误码对齐测试；下一期对 native poll/notify 失败做 QP teardown、late CQE、buffer/MR lifetime 和 callback-once 测试。

### E. API、配置和命名兼容

- 覆盖条目：RDMA-011、RDMA-013、RDMA-015、RDMA-016。
- 推荐顺序：先确认错误码和 public typo 策略，再做 config 早期校验。
- 主要文件：config derive/use_device、`rdma_commons.hpp`、`small_sglist.hpp`、ibv verbs post path。
- 测试策略：编译兼容测试、config capability tests、benchmark。

### F. 后续专项：控制面 cancellation correctness 和 batch op

- 覆盖条目：RDMA-019、RDMA-026。
- 本期定位：经用户确认均不实现。RDMA-019 是显式延期的 P1 correctness/lifetime 风险；RDMA-026 是 P3 性能专项，二者不得使用同一风险分类。
- 后续方向：RDMA-019 作为控制面 cancellation correctness 专项处理 stale slot / late emit；RDMA-026 作为 batch op / unsignaled batching 专项处理 fire-and-forget、signaled marker、错误传播和 benchmark。

## 推荐里程碑

### M0a：规格冻结

产出：记录用户已确认决策；冻结公共错误码、move-accept completion signature、BufferSequence lifetime、MR 地址模型假设、本期/后续专项边界。

出口准则：本期条目不再依赖产品语义选择；每项都有证据状态、完成条件、兼容性影响、依赖决策和目标平台。

### M0b：首批 P0 最小测试 seam

产出：只建设首批 P0 立即需要的 mock/helper seam，不预先为全部 P1/P2 做大测试框架。优先覆盖 `CreateOverlappedFile` 失败契约、MR range helper 和 connector 未 open guard。

出口准则：RDMA-001/002、RDMA-004、RDMA-005 能先写失败测试再修复；无法 mock 的 provider 行为有明确真机验收项。

### M1：P0 安全修复

产出：ND handle 安全、MR range 安全、connector accept guard 安全。

建议包含：RDMA-001、RDMA-002、RDMA-004、RDMA-005。

出口准则：失败返回、未 open connector、MR 越界输入均不会 crash 或产生 UB；ND/ibv 同类错误码基本一致。

### M2：P1 生命周期和状态机修复

产出：data-plane operation lifetime 清楚、ND accept 状态机一致；ibv CQ terminal teardown 留给下一期 QP 生命周期专项。

建议包含：RDMA-003、RDMA-006、RDMA-007、RDMA-012、RDMA-017、RDMA-018。

出口准则：move-accept 的 event/poll overload 返回已绑定 QP；deferred token、临时 remote_addr、accept 失败都有测试；RDMA-010/RDMA-021 作为显式剩余风险链接到下一期可执行规格。

### M3：错误语义和诊断统一

产出：ND/ibv、event/poll、throw/error_code overload 行为对齐；配置错误早暴露。

建议包含：RDMA-008、RDMA-009、RDMA-011、RDMA-014、RDMA-015、RDMA-020、RDMA-022。

出口准则：completion_queue throwing/error_code overload 语义一致；ND event/poll cancel mapping 一致；显式 device + config 的错误能在 `use_device` 阶段暴露。

### M4：性能和整理

产出：低风险性能优化、文档、命名清理和长期性能设计。

建议包含：RDMA-013、RDMA-016、RDMA-024、RDMA-025。

出口准则：低风险性能项有 benchmark 或 allocation 观测；RDMA-019 作为显式延期的 P1 correctness 风险进入后续专项，RDMA-026 进入性能专项；本期不改变控制面 cancellation 和每个 async op 都完成的语义。

## 建议回归命令

具体命令取决于本地生成目录和平台，建议至少覆盖：

- Windows + ND：unit tests、ND socket echo、ND completion queue poll/event tests。
- Linux + ibv：unit tests、ibv connector/listener/queue_pair tests、ibv poll-mode completion queue tests。
- 通用：`tests/unit/*config*`、MR/buffer/small_sglist 相关新增单测。

本报告生成阶段未运行编译或测试；以上命令应在修复实现后执行。
