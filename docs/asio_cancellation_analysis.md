# asio 取消机制详解(学习向)

> 目的:把 asio 的异步操作取消机制讲透,作为 rdma_on_asio 取消设计的理论基础,也作为一份可独立阅读的
> 学习材料。所有引用都基于本仓库 vendored 的 asio(include 前缀 `third_party/asio/include/`,由
> `CMakeLists.txt:57` 加入搜索路径),给出确切 file:line,方便对照源码精读。
>
> 落地到 rdma_on_asio 的具体设计见三篇分期计划:
> `plans/cancellation_stage1_object_plan.md`(对象级)、`plans/cancellation_stage2_control_single_op_plan.md`(控制面单操作)、
> `plans/cancellation_stage3_data_single_op_plan.md`(数据面单操作)。

---

## 0. 一句话先抓住本质

> **asio 能取消的,是"还排在某个软件队列里等待执行"的异步操作;"取消"就是在它真正执行之前,把它从队列
> 里摘出来、以 `asio::error::operation_aborted` 完成。** 一旦操作真正执行了(数据到了、事件触发了),
> 它就从队列里消失了,再也无法取消。

记住这条不变式,后面所有机制都是它的工程化展开。理解它也就理解了为什么 RDMA 数据面无法做真正的单操作
取消 —— 因为 WR 一 post 就直接进了硬件,根本没有"在软件队列里等待"这个阶段。

---

## 1. 全景:asio 有两套取消

两套机制互相独立,但最终都让被取消的操作以 `operation_aborted` 这个 `error_code` 调用其 handler。

| | 对象级 `cancel()` | 单操作级 `cancellation_slot` |
|---|---|---|
| 引入时间 | 早期就有 | C++20 协程时代(asio 1.19, 2021)|
| 粒度 | 取消该 IO 对象上**全部**在途操作 | 取消**某一个**指定操作 |
| 触发方式 | 直接调对象的 `cancel()` 成员 | 给操作关联一个 slot,通过配对的 signal `emit()` |
| 组合性 | 差(无法只取消其中一个)| 好(`cancel_after`/`co_spawn`/`parallel_group`/`\|\|` 全建立在它之上)|
| 底层动作(epoll)| `reactor_.cancel_ops(fd)` | `reactor_.cancel_ops_by_key(fd, key)` |
| 底层动作(IOCP)| `CancelIoEx(handle, NULL)` | `CancelIoEx(handle, overlapped)` |

为什么会有两套?对象级的 `socket.cancel()` 简单粗暴,在"一个对象同时只有一两个在途操作"的旧式回调代码
里够用;但协程时代一个对象上常常并发多个操作(读写并行、`||` 竞速、超时包装),需要"只取消这一个而不动
别的",于是有了单操作级的 cancellation_slot。**新代码几乎只通过上层封装(见 §4)间接使用 slot,很少手写。**

---

## 2. 核心不变式与操作生命周期

一个 reactor 驱动的异步操作(以 epoll 上的 `async_read_some` 为例)生命周期:

```
async_read_some(buf, token)
   │  分配一个 reactor_op,挂进 epoll_reactor 里该 fd 的 op_queue_[read_op]
   ▼
[ 排队等待 ]  ← 仅在这个阶段可取消:把 op 从 op_queue 摘出 → operation_aborted
   │  fd 变为可读,reactor 调 op->perform()(真正 recv)
   ▼
[ 执行中 / 已完成 ]  ← 已离开队列,无法取消
   │  post 到 scheduler
   ▼
handler(ec, n) 被调用
```

可取消窗口 = "排队等待"那一段。`operation_aborted` 的语义就是"这个操作没有真正发生过(或被中途放弃),
当作未完成处理"。

> 对 RDMA 的启示:socket 在"等就绪"时停在**软件队列**里,所以可摘;RDMA 的 send/recv 一旦 `ibv_post_*`
> 就进了**硬件的 SQ/RQ**,没有这个软件等待阶段,因此没有"摘队列"可言 —— 这是数据面取消的根本约束。

---

## 3. cancellation_signal / slot / state 的对象模型

三个核心类型(`asio/cancellation_signal.hpp`、`asio/cancellation_state.hpp`):

- **`cancellation_signal`** —— "发射端"。持有一个 slot,提供 `emit(cancellation_type)`。
- **`cancellation_slot`** —— "接收端"。可以 `emplace<Handler>(args...)` 就地构造一个取消处理器,
  `is_connected()` 判断是否已挂处理器,`clear()` 清除。
- **`cancellation_state`** —— 协程/组合操作里持有 signal 并按 `cancellation_type` 过滤、转发的状态机
  (见 §4、§9)。

关键源码:
- `cancellation_slot::emplace<Handler>(args...)`(cancellation_signal.hpp:142-156):在 slot 托管的内存里
  placement-new 一个 `detail::cancellation_handler<Handler>`,存入 `*handler_`,返回对它的引用。
- `cancellation_slot::is_connected()`(cancellation_signal.hpp:185-188):就是判断 `handler_ != 0`。
- `cancellation_signal::emit(type)`(cancellation_signal.hpp:95-99):调用 `handler_->call(type)`,
  最终分派到 emplace 进去的那个处理器的 `operator()(cancellation_type_t)`。

所以"取消"在最底层就是:**emit → slot 里登记的处理器的 `operator()(type)` 被调用**,由这个处理器去执行
真正的取消动作(摘队列 / CancelIoEx)。

---

## 4. 操作如何关联到 slot —— 关联机制与上层封装

异步操作通过 `asio::get_associated_cancellation_slot(handler)`(`asio/associated_cancellation_slot.hpp`,
被 reactive_socket_service_base.hpp:24、win_iocp_socket_service_base.hpp:22 包含)从 handler 取出关联的
slot。handler 怎么带上 slot?几种途径,从底到高:

1. **`bind_cancellation_slot(slot, token)`** —— 手动把一个你自己持有的 `cancellation_signal` 的 slot
   绑到这次操作:
   ```cpp
   asio::cancellation_signal sig;
   socket.async_read_some(buf, asio::bind_cancellation_slot(sig.slot(), handler));
   sig.emit(asio::cancellation_type::total);   // 只取消这一个
   ```
2. **`co_spawn`** —— 启动协程时内部建一个 `cancellation_state`;协程里每个 `co_await` 的操作自动关联到它
   的 slot。`co_spawn` 返回的句柄(或 `cancellation_signal`)emit 时,**当前正在 await 的那个操作**被取消。
   `co_spawn` 默认启用 `terminal` 类型取消。
3. **`cancel_after(op, duration)` / `cancel_at(op, time_point)`** —— 把操作包一层定时器,到点自动 emit。
   这是给单个操作加超时的惯用法。
4. **`awaitable_operators` 的 `||`** —— `co_await (op1 || op2)`,谁先完成就 emit 取消另一个。
5. **`experimental::parallel_group` / `make_parallel_group`** —— 一组操作,按策略取消未完成者。

上层封装(2-5)都建立在(1)的 slot/signal 之上;它们替你管理 signal 的生命周期和 emit 时机。这正是
"为什么很少手写 slot"的原因。

---

## 5. cancellation_type —— 三档语义

`asio/cancellation_type.hpp:56-65`:
```cpp
enum class cancellation_type : unsigned int {
  none     = 0,
  terminal = 1,
  partial  = 2,
  total    = 4,
  all      = 0xFFFFFFFF
};
```
位运算符在 :73-151;语义注释在 :33-45:

- **`total`** —— 操作**没有产生任何可观察副作用**,可以当作从未发生。最"干净"。
- **`partial`** —— 操作产生了**部分副作用**,但对象处于一个**已知且仍可继续使用**的状态。
- **`terminal`** —— 操作可能已产生副作用,取消后**唯一安全的后续动作是 close/destroy**(对象可能不可用)。

**每个操作自己决定honor哪几档**:取消处理器的 `operator()` 用位掩码 guard。socket 的 reactor/IOCP 操作
三档全接(摘队列在它看来无副作用),例如 `reactor_op_cancellation::operator()`
(reactive_socket_service_base.hpp:716-726):
```cpp
void operator()(cancellation_type_t type) {
  if (!!(type & (cancellation_type::terminal
               | cancellation_type::partial
               | cancellation_type::total)))
    reactor_->cancel_ops_by_key(descriptor_, *reactor_data_, op_type_, this);
}
```
一个只支持 `total` 的操作就只对 `type & total` 反应、其余 no-op。

> 对 RDMA 数据面的启示(见 stage3):已 post 的 WR 只能 honor `terminal`(= flush 整 QP),不能 honor
> `partial`/`total`,因为声称"无副作用/对象仍可用"都是假的。

---

## 6. 完整链路 A:对象级 `cancel()`

### epoll(Linux)

`reactive_socket_service_base::cancel`(声明 reactive_socket_service_base.hpp:109-110,实现
impl/reactive_socket_service_base.ipp:160-176):
```cpp
asio::error_code reactive_socket_service_base::cancel(
    base_implementation_type& impl, asio::error_code& ec) {
  if (!is_open(impl)) { ec = asio::error::bad_descriptor; return ec; }
  reactor_.cancel_ops(impl.socket_, impl.reactor_data_);   // 委托给 reactor
  ec = asio::error_code();
  return ec;
}
```
`epoll_reactor::cancel_ops`(impl/epoll_reactor.ipp:349-371):
```cpp
void epoll_reactor::cancel_ops(socket_type, per_descriptor_data& descriptor_data) {
  if (!descriptor_data) return;
  mutex::scoped_lock descriptor_lock(descriptor_data->mutex_);
  op_queue<operation> ops;
  for (int i = 0; i < max_ops; ++i)                        // 遍历 read/write/except 三个队列
    while (reactor_op* op = descriptor_data->op_queue_[i].front()) {
      op->ec_ = asio::error::operation_aborted;            // 置 aborted
      descriptor_data->op_queue_[i].pop();
      ops.push(op);
    }
  descriptor_lock.unlock();
  scheduler_.post_deferred_completions(ops);               // 投递完成
}
```
把该 fd 上**所有**等待 op 摘出、置 aborted、投递。

### IOCP(Windows)

`win_iocp_socket_service_base::cancel`(声明 :123,实现 impl/win_iocp_socket_service_base.ipp:235-326)
核心是 `CancelIoEx(sock_as_handle, 0)`(:248-277)—— `lpOverlapped == 0` 即取消该 handle 上**全部** I/O;
`ERROR_NOT_FOUND`(无可取消项)被吞掉以与其它平台对齐。之后若曾惰性建过 reactor,再
`r->cancel_ops(...)`(:315-323)。

---

## 7. 完整链路 B:单操作级取消

### epoll —— slot 注册 + 按 key 取消

注册发生在发起处。`async_send` 的那段(reactive_socket_service_base.hpp:293-310):
```cpp
associated_cancellation_slot_t<Handler> slot
  = asio::get_associated_cancellation_slot(handler);

typedef reactive_socket_send_op<...> op;
typename op::ptr p = { addressof(handler), op::ptr::allocate(handler), 0 };
p.p = new (p.v) op(success_ec_, impl.socket_, impl.state_, buffers, flags, handler, io_ex);

if (slot.is_connected())                                   // 关联了 slot 才装
  p.p->cancellation_key_ =                                 // 把"取消处理器的地址"作为 key
    &slot.template emplace<reactor_op_cancellation>(
        &reactor_, &impl.reactor_data_, impl.socket_, reactor::write_op);
```
- `cancellation_key_` 是 `reactor_op` 的成员(reactor_op.hpp:34,ctor 初始化为 0 在 :56)。
- 这套六行 idiom 在每个 reactor 操作里重复(wait :237-243、recv/recvfrom/recvmsg/sendto
  :307-310/:342-343/:413-414/:453-454/:524-525/:563-564)。

取消处理器 `reactor_op_cancellation`(reactive_socket_service_base.hpp:703-733):
```cpp
class reactor_op_cancellation {
public:
  reactor_op_cancellation(reactor* r, reactor::per_descriptor_data* p, socket_type d, int o)
    : reactor_(r), reactor_data_(p), descriptor_(d), op_type_(o) {}
  void operator()(cancellation_type_t type) {
    if (!!(type & (cancellation_type::terminal | cancellation_type::partial
                 | cancellation_type::total)))
      reactor_->cancel_ops_by_key(descriptor_, *reactor_data_, op_type_, this);  // key == this
  }
  // ... reactor_, reactor_data_, descriptor_, op_type_
};
```
`epoll_reactor::cancel_ops_by_key`(impl/epoll_reactor.ipp:373-400):
```cpp
void epoll_reactor::cancel_ops_by_key(socket_type, per_descriptor_data& descriptor_data,
    int op_type, void* cancellation_key) {
  if (!descriptor_data) return;
  mutex::scoped_lock descriptor_lock(descriptor_data->mutex_);
  op_queue<operation> ops; op_queue<reactor_op> other_ops;
  while (reactor_op* op = descriptor_data->op_queue_[op_type].front()) {
    descriptor_data->op_queue_[op_type].pop();
    if (op->cancellation_key_ == cancellation_key) {       // 只匹配这个 key
      op->ec_ = asio::error::operation_aborted; ops.push(op);
    } else other_ops.push(op);                             // 其余原样 requeue
  }
  descriptor_data->op_queue_[op_type].push(other_ops);
  descriptor_lock.unlock();
  scheduler_.post_deferred_completions(ops);
}
```
只扫该 fd 的**那一个** op_type 队列,只摘 key 匹配者,其余放回 —— 这就是"单操作"粒度的来源。

### IOCP —— `CancelIoEx(handle, op)`

注册(win_iocp_socket_service_base.hpp:300-319):分配 op 后,若 slot 已连接,
`o = &slot.emplace<iocp_op_cancellation>(impl.socket_, o)` 把 op 包成取消目标。
`iocp_op_cancellation`(win_iocp_socket_service_base.hpp:648-685):
```cpp
class iocp_op_cancellation : public operation {       // 它本身就是个 operation(继承 OVERLAPPED)
public:
  iocp_op_cancellation(SOCKET s, operation* target)
    : operation(&iocp_op_cancellation::do_complete), socket_(s), target_(target) {}
  static void do_complete(void* owner, operation* base, const error_code& ec, std::size_t n) {
    static_cast<iocp_op_cancellation*>(base)->target_->complete(owner, ec, n);  // 转发给被包的 op
  }
  void operator()(cancellation_type_t type) {
    if (!!(type & (terminal | partial | total))) {
      HANDLE h = reinterpret_cast<HANDLE>(socket_);
      ::CancelIoEx(h, this);     // 'this' 即那个 OVERLAPPED -> 只取消这一个 op
    }
  }
};
```
精髓:取消对象**本身就是 OVERLAPPED**,既当 `CancelIoEx` 的 key,又当完成时转发给真正 op 的桥。
还有一个 `reactor_op_cancellation` 变体(:734-792):若 `use_reactor` 走 `cancel_ops_by_key`,否则
`CancelIoEx(handle, this)`。

---

## 8. 取消与完成的竞态 —— 为什么 late emit 安全

`emit` 可能发生在操作其实已经完成之后(比如定时器到点的同一瞬间数据刚好到了)。asio 的安全性来自两道:

1. **协程/组合操作里 slot 会被复位**:`cancellation_state` 在两次 `co_await` 之间会复位 slot,所以上一个
   已完成操作的取消处理器不再挂着。
2. **即使取消处理器真的 fire 了,它的动作也是 no-op**:`cancel_ops_by_key` 在队列里找不到该 key 的 op
   (已离开队列)→ 什么都不做;`CancelIoEx(handle, op)` 对一个已完成的 OVERLAPPED 返回
   `ERROR_NOT_FOUND` → 被吞掉。

所以"先完成后取消"不会重复回调、不会误伤。**这也是 rdma 数据面 flush 方案要复用的安全性论据**:late emit
要么 slot 已清、要么 flush 落到一个已无在途 WR 的 QP 上(无害)。

---

## 9. 在协程里的传播(co_spawn / awaitable / ||)

- `co_spawn(ex, awaitable, token)` 内部建 `cancellation_state`;协程体每个 `co_await` 的操作通过其
  `associated_cancellation_slot` 关联到这个 state 的 slot。`co_spawn` 默认开启 **terminal** 取消
  (`enable_terminal_cancellation`)。
- `this_coro::cancellation_state` / `throw_if_cancelled` 可在协程里查询/响应取消。
- `co_await (a || b)`(awaitable_operators):内部用 parallel-group 语义,先完成者触发对另一个 emit。
- `cancel_after(op, d)`:等价于 `(op || timer(d))` 的超时糖,到点 emit `terminal`(可配)。

对调用方而言:写 `co_await cancel_after(socket.async_read(buf), 5s)` 就得到"5 秒读不到就以
operation_aborted 放弃"——底层正是 §7 的 slot → cancel_ops_by_key。

---

## 10. 给库作者的清单:如何让自定义异步操作支持取消

要让你自己的 `async_xxx`(像 rdma 的 connect/send)接入 asio 取消生态:

1. 操作的 op 对象继承体系里要有一个可用的 **key 字段**(`reactor_op` 自带 `cancellation_key_`;自定义
   op 自备一个 `void*`)。
2. 发起时:`auto slot = get_associated_cancellation_slot(handler); if (slot.is_connected()) { op->key = op;
   slot.emplace<YourCanceller>(...needed refs...); }`。
3. `YourCanceller::operator()(cancellation_type_t type)`:用位掩码 guard 你**真正能honor**的档位,
   在里面执行真正的取消动作(摘队列 / 取消系统调用 / 等)。
4. 取消动作对"已完成/已离队"的 op 必须是 **no-op**(保证 late-emit 安全,见 §8)。
5. 完成路径保证 handler **恰好被调用一次**(取消路径与正常完成路径互斥,靠 op 离队/置 ec 实现)。
6. 文档写清你honor哪些 `cancellation_type` 及其副作用语义。

---

## 11. 映射到 rdma_on_asio(指引)

| asio 概念 | rdma_on_asio 对应 | 详见 |
|---|---|---|
| 对象级 `socket.cancel()`(epoll cancel_ops)| 控制面 `connector/listener::cancel()`(已用 `reactor_.cancel_ops` on cm fd)| stage1 |
| 对象级 `socket.close()`(terminal teardown)| **数据面没有独立 cancel**:中止数据面 = `connector::disconnect()`(同步;已自带 flush,令在途 op 以 `operation_aborted` 完成)。带外断开通知见 `connector::async_wait_disconnect`(disconnect 重构新增,commit dadc61c)| stage1 |
| 单操作 epoll(slot + `cancel_ops_by_key`)| 控制面 connect/accept/get_connection 接 slot(op 本就是 `reactor_op`,自带 key)| stage2 |
| 单操作 IOCP(`CancelIoEx(handle, op)`)| nd 控制面单操作取消(需在 impl 里保留 op 指针)| stage2 |
| 单操作 + `terminal` 副作用语义 | 数据面 send/recv 的 per-op 取消(nice-to-have):`terminal` 的终点是 **disconnect**(不是本地 flush);read/write 不支持 | stage3 |
| "可取消 = 在软件队列等待" | 控制面 op 停在 reactor 队列 ⇒ 可摘;数据面 WR 进硬件 ⇒ 不可摘,只能整连接 teardown | 三篇皆引 |

> **关键对称**:socket 把多个 I/O 复用在一个可重用 fd 上,所以 `cancel`(中止操作、对象仍可用)与
> `close`(销毁)是两件事。RDMA 的 QP/cm_id **本身约等于连接**,中止其在途操作基本等于拆连接 ——
> 所以 socket 那种"可复用的 cancel"在数据面**没有对应物**。本地 `modify_qp(ERR)` flush 只是
> `rdma_disconnect` 的真子集(且不通知对端 -> 静默 desync),因此**不单独暴露**:数据面要"取消",就用
> `disconnect()`(同步,已经在 flush)。
>
> 一句话收束:rdma 的**控制面**契合 asio 取消模型(op 停在 reactor 队列里、可摘);**数据面**碰到 asio
> 模型的边界(WR 进硬件、无软件等待阶段),其"取消"本质是连接 teardown(disconnect),而非 socket 式的
> 可复用 cancel。
