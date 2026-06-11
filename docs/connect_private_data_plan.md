# Connect/Accept private_data 重构计划(收发对称)

> **状态:ibv 已实现并在 RoCE 上验证;nd 已镜像(Windows 待验证)。**
> `async_connect(qp, ep, request_const, reply_mutable, token) -> void(ec, size_t)`;
> `async_get_connection(request_mutable, token) -> void(ec, connector, size_t)`(+ fill 形态);
> `async_accept(qp, reply_const, token) -> void(ec)`;`get_remote_data()` 已删除。出向 connect 请求拷进 op、
> accept 同步不拷、`{}` 跳过 native 路径、超 255 返回 `ext_private_data_too_large`、收端截断报写入字节数。
> 测试 `tests/rdma/test_rdma_private_data.cpp`(非对称矩阵 + 出向临时量生命周期)通过,全量回归绿。
>
> 目标:把 `async_connect` / `async_get_connection` / `async_accept` 的 private_data 重设计为**收发对称**的
> buffer API ——「**发**」用 `const_buffer` 参数、「**收**」用 `mutable_buffer` 出参,**移除 `get_remote_data()`**。
> 顺带厘清出向 pd 的生命周期(connect 拷进 op、accept 同步不拷)。ibv 先落地 + RoCE 验证,nd 镜像(Windows 后验证)。

## 0. 背景与现状(实测)

**当前 API**:`async_connect(qp, ep, const_buffer pd, token) -> void(ec)`、
`async_accept(qp, const_buffer pd, token) -> void(ec)`、`async_get_connection(token) -> void(ec, connector)`;
两侧收到的对端 pd 都经 **`connector::get_remote_data()`** 读(client 读 server 的 reply、server 读 client 的
request)。pd 暂存于 `implementation_type::private_data_buffer_`(定长 `max_private_data_size = 256`)+ `_length_`。

**出向 pd 生命周期(ibv)**:`ibv_connect_op` **只存指针不拷贝**,而 `rdma_connect` 在 ADDR/ROUTE 异步解析**之后**
才发(`ibv_op_connect.hpp:188`)→ 现状要求调用方保活 buffer 到 connect 完成。`rdma_accept` 则在 `async_accept`
内**同步**调用(`ibv_service_connector.hpp:449`),buffer 不必活过该调用。

**rdma_cm 填充实测(RoCE mlx5)**:客户端发**空** request,服务端收到 **56B** 零填充;accept 发空,客户端收到
**196B** 零填充。→ **接收端拿到的是 CM REQ/REP 定长字段(零填充),不是发送端精确长度**;"没发"与"发零"在接收端
不可区分。空 pd 当前已能正常建链 + 数据面往返(实测)。

## 1. 四个数据动作 → 收发对称的归属(核心)

RDMA CM 握手是一个 request/reply 交换,共 4 个数据动作,落在 3 个调用上(② 必须早于 accept——服务端要先看到
request 才能决定收不收,故 request 的接收点天然在 get_connection):

| 动作 | 调用 | 形态 | 方向 | 何时发生 |
|---|---|---|---|---|
| ① 发 request | `async_connect` | `const_buffer` 参数 | 出向 | 发起(异步,实发在 ROUTE_RESOLVED 之后) |
| ④ 收 reply | `async_connect` | `mutable_buffer` **出参** | 入向 | 完成时(ESTABLISHED) |
| ② 收 request | `async_get_connection` | `mutable_buffer` **出参** | 入向 | 完成时(CONNECT_REQUEST) |
| ③ 发 reply | `async_accept` | `const_buffer` 参数 | 出向 | 发起(同步 `rdma_accept`) |

**统一规则:发 = `const_buffer` 参数;收 = `mutable_buffer` 出参。`get_remote_data()` 移除。**

### 新签名(D0)

```cpp
// client: 发 request(1) + 收 reply(4)
async_connect(qp, ep, asio::const_buffer request, asio::mutable_buffer reply, token)
    -> void(error_code, std::size_t reply_len);          // Q-len: reply_len = 写入 reply 的字节数

// server: 收 request(2)
async_get_connection(asio::mutable_buffer request, token)
    -> void(error_code, connector, std::size_t request_len);     // return form
async_get_connection(connector& conn, asio::mutable_buffer request, token)
    -> void(error_code, std::size_t request_len);                // fill form
// server: 发 reply(3)
async_accept(qp, asio::const_buffer reply, token) -> void(error_code);
```

## 2. 设计决定

### D-conv —— 便捷重载:不接收 response pd(简化常见场景)

完整签名让"收"成为 mutable 出参 + `size_t` 完成长度。对**不关心对端 reply** 的客户端,这是负担(callback 多一个
`reply_len`)。故提供便捷重载(注意:带 `reply_len` 的是 **`async_connect`**,即接收 server reply 的一侧,不是
`async_accept` —— accept 只发 reply、完成仍是 `void(ec)`):

- **`async_connect(qp, ep, request, token) -> void(ec)`**:不接收 reply(等价于 reply 传 `{}` + 丢弃 `reply_len`)。
  内部用 associator 转发的 `connect_drop_reply_adapter`(保住 cancellation_slot / allocator / executor),把
  `void(ec, size_t)` 适配成 `void(ec)`。
- **`async_accept(qp, token) -> void(ec)`**:不发 reply(等价于 reply 传 `{}`),纯转发,无 adapter。
- 完整重载(带 reply / 带 const reply)保留。README/demo 默认用**不带 response pd** 的简化形态;
  `test_rdma_private_data` 同时覆盖**带**(矩阵)与**不带**(便捷重载)两种。
- (对称的 `async_get_connection` 无 request 重载属"不接收 request pd",本次不做 —— 用户关注的是 response/reply;
  可作后续对称补充。)

### D1 —— 类型 + "无 / 不收" 的表达

- 「发」一律 `const_buffer`(connect 的 request、accept 的 reply);「收」一律 `mutable_buffer` 出参
  (connect 的 reply、get_connection 的 request)。
- **不加重载**:参数必填。**不发**传 `asio::const_buffer{}`;**不收**传 `asio::mutable_buffer{}`(空出参 →
  完成时不拷、`*_len = 0`)。
- (撤销之前"accept 用 mutable / 无参重载"的设想:accept 是发 reply → const;无参重载不要。)

### D2 —— 出向 private_data 生命周期:connect / accept 分治

| 路径 | native 调用时机 | 库是否拷贝 | 存哪 |
|---|---|---|---|
| **accept(发 reply)** | `rdma_accept` **同步**(当场拷进 CM REP) | **不需要** | 不存(直接用调用方 view;临时量都安全) |
| **connect(发 request)** | `rdma_connect` ADDR/ROUTE 解析后**异步**发 | **需要** | **connect op** 内嵌缓冲 |
| **`{}`(空)** | — | 不拷 | 不存,native 传 `nullptr`/0(跳过整条 pd 路径) |

- connect 发起(同步阶段)把 request `memcpy` 进 `ibv_connect_op` 的内嵌 `std::array<std::byte, cap>` + len;
  `do_process_addr_route` 用 op 缓冲(`len==0 -> nullptr`)。→ 调用方不需保活 request。

### D-recv —— 入向接收 buffer 的归属与生命周期

- ④ reply / ② request 直接拷进**调用方提供的 mutable buffer**,**不再经 impl**。
  → `implementation_type::private_data_buffer_` / `_length_` **删除**;`get_remote_data()` / `assign_with_private_data`
  **删除**。
- 接收 buffer 是调用方的,须活到对应 async 完成(标准 asio recv 契约;协程帧内天然满足)。
- connect op 额外持有 reply 的 `mutable_buffer` view,ESTABLISHED 时 `memcpy`(`min(padded_len, buf.size())`)。
- listener 的 get_connection 适配器:把到达的 request pd 拷进调用方 buffer(替代原先存进新 connector 的 impl),
  再 `assign(handle)`(无 pd),最后 `handler(ec, connector, request_len)`。须保留 associator 转发(cancellation_slot)。

### D3 —— 非对称发送 + 填充长度语义

- **每方向独立**:request(client→server)与 reply(server→client)互不要求,一方发一方不发完全合法。
- **收到长度不可信**:`reply_len` / `request_len` = **写入 buffer 的字节数** = `min(rdma_cm 填充长度, buffer 容量)`;
  这是填充值(实测 56/196),**非发送端精确长度**。需要精确长度的 app 自带长度前缀 / sentinel(README 写明)。
  nd 行为可能不填充(返回精确长度)—— Windows 验证后在文档注明差异。

## 3. 测试计划(tests/rdma 跨平台为主)

- **非对称发送矩阵** `{client_req ∈ {空, "REQ-xyz"}} × {server_reply ∈ {空, "REP-abc"}}`(4 组):
  - 建链成功;接收端 buffer 的**前 N 字节** == 发送端实发 N 字节(N=发送长度;`*_len >= N`,尾部为填充);
  - 发送端为空时只验证建链(不对填充内容断言);数据面往返一次确认可用。
- **request 出向临时量回归**:`async_connect(qp, ep, asio::buffer(std::string("temp")), reply, tok)` —— 临时
  string 在发起返回后析构。断言服务端 `request` buffer 前 N 字节仍是 `"temp"`(证明已拷进 op;D2 实施后必过)。
- **`{}` 行为**:不发(request/reply 传 `const_buffer{}`)、不收(reply/request 传 `mutable_buffer{}`,`*_len==0`)。
- **接收 buffer 容量 < 填充长度**:小 buffer → 截断到容量,`*_len == 容量`,不越界。

## 4. 影响面(较大,API 破坏)

- **公共签名变更**(`ibv_connector` / `ibv_listener` + nd 镜像):
  - `async_connect`:+`mutable_buffer reply` 参数;完成 `void(ec)` → `void(ec, size_t)`(Q-len)。
  - `async_get_connection`(两种形态):+`mutable_buffer request` 参数;完成 +`size_t`(Q-len)。
  - `async_accept`:签名不变(已是 `const_buffer reply`)。
  - **删除 `connector::get_remote_data()`**。
- `implementation_type`:删 `private_data_buffer_` / `private_data_length_`;`assign_with_private_data` → `assign`。
- `ibv_connect_op`:request 出向改内嵌拷贝(D2);+reply mutable view(ESTABLISHED 拷,报 len)。
- `ibv_op_get_connection_request` + listener 适配器:request pd 拷进调用方 buffer + 报 len(保留 associator 转发)。
- `ibv_service_connector::start_accept_op`:仅确保 `size()==0 -> nullptr`(同步,不拷)。
- **全部测试更新**:echo / echo_poll / wait_disconnect / disconnect_cancel / control_cancel / connector_listener /
  rdma_echo* / rdma_sgl —— 均调 connect/accept/get_connection,需加 buffer 参数 + 改结构化绑定 + 去 `get_remote_data()`。
- nd 后端镜像(`nd_connector` / `nd_listener` / service / op)。
- README + 注释(`ibv_op_connect.hpp:46-48` 等)。

## 5. 设计点(已拍板)

- **Q-len —— 已定**:`async_connect` / `async_get_connection` 完成签名带 `std::size_t`(= 写入接收 buffer 的字节数)。
- **Q1 —— 已定**:出向 cap 统一 `max_outgoing_private_data = 255`(ibv `private_data_len` 是 `uint8_t`)。
- **Q4 —— 已定**:出向 `size > 255` 发起时返回干净错误码 `ext_private_data_too_large`(不静默截断)。
- **Q-trunc —— 已定**:接收 buffer 容量 < 填充长度 → 取 `min` 截断,`*_len = 容量`(不报错),文档说明。
