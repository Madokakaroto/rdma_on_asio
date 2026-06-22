# 重构计划 -- device 兼顾 v4/v6 双地址族(nd 单 adapter + 双地址)

> 目标:让**一个 `use_device` 的 device 同时支持 v4 与 v6 地址**,对齐 asio socket / ibv 的语义,
> 消除 nd 平台上"v4 与 v6 是两个 device、互斥"的现状。
> 采用**方案 A:按 `AdapterId` 归并 -- 同一块物理网卡只 `OpenAdapter` 一次(一个 PD/资源域),
> 该 adapter 实例同时挂 v4/v6 两个本地地址**;control plane(connector/listener)按地址族选择要绑定的本地地址。
>
> 状态:**主体已实现,验收补强中**。nd 已按 AdapterId 归并为单 device 双地址族,控制面按
> `open(ps)` / endpoint family 选择本地地址;ibv/nd 的 `get_first_available_device` 已统一去掉端口空间参数。
> 待补齐项集中在 dual-family echo 的自动化验收、v6 硬件覆盖、ibv 多网卡 caveat 验证。
> 相关:`io_completion_service_split_plan.md`(device_service / io_completion_service 拆分)、
> `queue_pair_semantics_plan.md`(QP bind/poll 语义)、`rdma_error_unification_plan.md`(错误码)。

---

## 1. 背景与问题

asio 的一个 `io_context` 上,v4 与 v6 是 socket/endpoint 的运行期属性,同一个 `io_context` 可同时服务两族。
但在本库的封装下:

- **nd(Windows NetworkDirect):** 一个 device(`nd_device_t = nd_adapter_t`)只携带**一个本地地址**
  (`name_`,见 [nd_device_impl.hpp:408](../include/rdma/nd/detail/nd_device_impl.hpp#L408)),
  发现层对每个地址各 `OpenAdapter` 一次,把同一块网卡的 v4/v6 拆成 `v4_adapters_` / `v6_adapters_`
  两个列表(见 [nd_impl_types.hpp:127-128](../include/rdma/nd/detail/nd_impl_types.hpp#L127-L128)、
  [nd_device_impl.hpp:471-491](../include/rdma/nd/detail/nd_device_impl.hpp#L471-L491))。
  于是 `use_device` 注册一个 device 就锁定了一个地址族 -- v4 与 v6 互斥。
- **ibv(Linux libibverbs):** device 只是 `{context, pd}`,与地址族**无关**;族在 rdma_cm connect 时由目标
  sockaddr 决定(`resolve_addr` 源地址传 `nullptr`,见
  [ibv_service_connector.hpp:396-398](../include/rdma/ibv/detail/ibv_service_connector.hpp#L396-L398))。
  **ibv 的 device 早已双族**;唯一的族绑定在 listener 的 `bind_endpoint_ = ps.any_endpoint(0)`
  ([ibv_service_listener.hpp:110](../include/rdma/ibv/detail/ibv_service_listener.hpp#L110)),
  这与 asio acceptor 一致(绑 `0.0.0.0` 的 acceptor 本就不收 v6),不是 device 的限制。

**结论:** 问题只在 nd;ibv 已满足目标,仅需验证 + API 对齐。

---

## 2. 根因与证据(为什么不需要改 NetworkDirect 库)

ND2 SPI **本身就支持**"一块网卡 open 一次、双地址共享 CQ/MR/PD",当前的两个 adapter 完全是本库发现层的产物。

1. **`AdapterId` 是物理网卡身份。** `ND2_ADAPTER_INFO.AdapterId`(UINT64,
   [nddef.h:57-80](../third_party/networkdirect/src/ndutil/nddef.h#L57-L80))文档定义为
   "Vendor defined unique ID of the adapter (**similar to a MAC address**)"
   ([docs/IND2Adapter.md:111-113](../third_party/networkdirect/docs/IND2Adapter.md#L111-L113))。
   `IND2Provider::ResolveAddress(local_addr, &adapterId)`
   ([ndspi.h:720-725](../third_party/networkdirect/src/ndutil/ndspi.h#L720-L725))对同一网卡的 v4/v6 地址返回
   **同一个 `AdapterId`** -- 文档明言其目的就是"sharing common resources such as completion queues and
   memory regions between connections that are active on **different IP addresses of the same adapter**"
   ([docs/IND2Provider.md:98](../third_party/networkdirect/docs/IND2Provider.md#L98));内核按
   `IF_PHYSICAL_ADDRESS HwAddress`(MAC)解析 adapter id(`NDV_RESOLVE_ADAPTER_ID`,
   [ndioctl.h:273-276](../third_party/networkdirect/src/ndutil/ndioctl.h#L273-L276))。

2. **两次 `OpenAdapter` = 两个资源域(不可互通)。** `ND_OPEN_ADAPTER` 以 `AdapterId` 为**输入**、返回一个
   per-open 的 `AdapterHandle`([ndioctl.h:114-120](../third_party/networkdirect/src/ndutil/ndioctl.h#L114-L120));
   CQ 建在 `AdapterHandle` 上([ndioctl.h:128-135](../third_party/networkdirect/src/ndutil/ndioctl.h#L128-L135)),
   QP 引用 `ReceiveCqHandle`+`InitiatorCqHandle`+`PdHandle`
   ([ndioctl.h:148-158](../third_party/networkdirect/src/ndutil/ndioctl.h#L148-L158))。
   每次 `OpenAdapter` 铸造**新的** `AdapterHandle`,故 v4 open 与 v6 open 是两个独立资源域:跨域的 CQ/PD/MR/QP
   会被驱动拒绝(等价于 Linux 上 `ibv_open_device` 两次 -> 两个 `ibv_context`)。
   **这排除了"双 adapter"方案(会让 MR 变成 family-scoped、shared CQ 按族拆、QP 延迟创建)。**

3. **一个 `IND2Adapter` 实例天然支持双族。** `IND2Adapter::QueryAddressList`
   ([docs/IND2Adapter.md:9](../third_party/networkdirect/docs/IND2Adapter.md#L9)、
   [:262-263](../third_party/networkdirect/docs/IND2Adapter.md#L262-L263))
   "Returns the **IPv4 and IPv6** addresses that are supported by the adapter instance";
   adapter 上的 `CreateCompletionQueue` / `CreateMemoryRegion` / `CreateQueuePair` / `CreateConnector` /
   `CreateListener` **全部与地址族无关**;族只在 `IND2Connector::Bind(sockaddr*)` /
   `IND2Listener::Bind(sockaddr*)` 进入(我方调用点
   [nd_ops_cm.hpp:26](../include/rdma/nd/detail/nd_ops_cm.hpp#L26)、
   [:137](../include/rdma/nd/detail/nd_ops_cm.hpp#L137))。

4. **不要走 ndutil 框架的 `NdOpenAdapter`。** 它是按**地址**做 key 的
   ([ndsupport.h:73-79](../third_party/networkdirect/src/ndutil/ndsupport.h#L73-L79)),会把 `AdapterId` 藏起来、
   重新引入逐地址模型。我方发现层目前正确地直接用 `IND2Provider` SPI
   ([nd_device_impl.hpp:290-298](../include/rdma/nd/detail/nd_device_impl.hpp#L290-L298)),保持不变。

> **唯一需上机确认的假设:** 同一网卡 `ResolveAddress(v4)` 与 `ResolveAddress(v6)` 返回相同 `AdapterId`
> (文档保证 + 内核按 MAC 解析,但厂商 user-mode 实现需实测一次,见 Phase 5)。

---

## 3. 设计决策:方案 A(单 adapter + 双地址)

| 维度 | 方案 A(选定) | 方案 B(双 adapter,已否决) |
|------|----------------|------------------------------|
| OpenAdapter 次数 | 每 `AdapterId` 一次 | 每地址一次(v4/v6 各一) |
| PD / CQ / MR / QP | 单一资源域,**数据面完全不变** | 两个资源域:MR family-scoped、双 CQ+双 poller、QP 须延迟创建 |
| 族绑定点 | control plane `Bind(本地地址)` 按族选 | 选 adapter 实例 |
| 改动范围 | 发现层 + 控制面族选择 | 数据面 + 控制面 + QP 生命周期(大) |
| 风险 | 低(贴合 SPI 设计意图) | 高(且需验证跨域资源是否可共享) |

数据面不变是方案 A 的关键收益:shared CQ 仍建在单个 `device->adapter_`
([nd_service_io_completion.hpp:71-82](../include/rdma/nd/detail/nd_service_io_completion.hpp#L71-L82)),
QP 仍在 `device->pd_`+cq 上创建([nd_service_verbs.hpp:60-86](../include/rdma/nd/detail/nd_service_verbs.hpp#L60-L86)),
MR 仍注册在 `device->pd_`([nd_mr.hpp:113](../include/rdma/nd/nd_mr.hpp#L113)),
shared-CQ poller 的单 poller / lock-free 语义不受影响。

---

## 4. 目标 API(跨平台)

```cpp
// 发现:不再传端口空间,仅按 device config 选;返回的 device 双族
rdma_device_ptr dev = rdma_device_manager_t::instance()
                          .get_first_available_device(/*config*/ {});

use_device(io, dev);          // 一个 device 同时支持 v4/v6

// control plane:族由端口空间 / endpoint 决定(connector/listener 各自)
rdma_listener<tcp> lis(io);
lis.open(tcp::v6());          // listener 按 open 的端口空间族选本地地址(对齐 ibv)
lis.bind(port); lis.listen();

rdma_connector<tcp> conn(io);
co_await conn.async_connect(qp, v4_or_v6_endpoint, ...);  // 按 endpoint 族选本地 bind 地址
```

要点:
- `get_first_available_device` **去掉 `PortSpace` 首参**(需求 #3),两后端统一为 `(config = {})`。
- device **双族**(需求 #4):nd 单 adapter 双地址;ibv 本就如此。
- **族在 control plane 选择**(需求 #5):listener 用 `open(ps)` 的端口空间族;connector 用 `async_connect`
  的 endpoint 族(实践中二者一致)。这与 ibv 现状同构(ibv listener 已用 `ps.any_endpoint(0)` 选族)。

> **核心不变量:一个 `io_context` 绑定且仅绑定一个 device。** `use_device` 第二次调用即返回
> `already_registered`([ibv_use_device.hpp:23-24](../include/rdma/ibv/ibv_use_device.hpp#L23-L24)、
> [nd_use_device.hpp:20-21](../include/rdma/nd/nd_use_device.hpp#L20-L21)),`device_service` 只持有一个
> `device_`([ibv_service_device.hpp:47](../include/rdma/ibv/detail/ibv_service_device.hpp#L47)、
> [nd_service_device.hpp:43](../include/rdma/nd/detail/nd_service_device.hpp#L43))。
> **这正是本 plan 成立的前提**:既然一个 io_context 只能有一个 device,要让它同时服务 v4/v6,这个 device
> 就**必须**是双族的(否则只能换 io_context)。也因此 connector/listener 用的 device 永远无歧义 ==
> `device_svc_.get_device()`,族选择就是从这唯一一个 device 上挑本地地址(nd)或让 sockaddr 穿透(ibv)。
> 多网卡 = **多个 io_context、各绑一个 device**,不是一个 io_context 跨多 device。

---

## 5. 数据结构改动(nd)

```cpp
// nd_impl_types.hpp -- device 现在 = 一个 adapter 实例 + 一组本地地址
struct nd_adapter_t {                 // 仍别名 nd_device_t
  nd2_adapter_ptr      adapter_;      // 仅一个 OpenAdapter
  std::unique_ptr<native_pd_t> pd_;   // 单一资源域
  UINT64               adapter_id_;   // 归并 key(ResolveAddress 返回值)
  std::optional<nd2_sockaddr_t> v4_addr_;   // 该 adapter 的 v4 本地地址(可空)
  std::optional<nd2_sockaddr_t> v6_addr_;   // 该 adapter 的 v6 本地地址(可空)
  std::string          name_;         // 展示用(保留:device_manager 测试断言非空)
  native_context_config_t info_;      // ND2_ADAPTER_INFO(含 AdapterId)
};

struct nd_provider_t {
  nd_provider_factory_ptr factory_;
  nd2_provider_ptr        provider_;
  std::vector<nd_adapter_ptr> devices_;   // 取代 v4_adapters_/v6_adapters_,按 AdapterId 去重
};
```

`nd_adapter_t` 增一个按 endpoint 族取本地地址的 helper:
`nd2_sockaddr_t const* local_addr_for(int family) const`(无则返回 `nullptr`)。

---

## 6. 分阶段实施计划

### Phase 0 -- 跨平台契约(先定,不写大代码)
- 确定:`get_first_available_device(config)` 去 `ps`;device 双族;族在 control plane 选择。
- 新增/确认错误码:device 不含所需族地址时返回 `rdma_errc::address_family_not_supported`
  (或复用既有码,见 `rdma_error_unification_plan.md`)。
- 验收:本文档评审通过。

### Phase 1 -- nd 发现层按 AdapterId 归并(核心)
改动文件:[nd_impl_types.hpp](../include/rdma/nd/detail/nd_impl_types.hpp)、
[nd_device_impl.hpp](../include/rdma/nd/detail/nd_device_impl.hpp)、
[nd_device.hpp](../include/rdma/nd/nd_device.hpp)。
- `nd_adapter_t`/`nd_provider_t` 按 §5 改。
- `open_adapter`([:286-306](../include/rdma/nd/detail/nd_device_impl.hpp#L286-L306)):拆出"按 `AdapterId`
  open 一次"与"按族挂地址"两步;`adaptor_id` 不再丢弃,存入 `adapter_id_`。
- `open_adapters`([:471-491](../include/rdma/nd/detail/nd_device_impl.hpp#L471-L491)):
  对 `enumerate_addr_list` 的每个(经 `is_valid_addr` 过滤的)地址 `ResolveAddress -> adapterId`,
  **按 adapterId 分组**,每组 `OpenAdapter` 一次构造 `nd_device_t`,把组内 v4/v6 地址填入 `v4_addr_`/`v6_addr_`,
  写入 `provider->devices_`。(可选实现 B:open 后用 `adapter->QueryAddressList()` 自取地址。)
- `nd_device_manager_t::get_first_available_device`
  ([:31-44](../include/rdma/nd/nd_device.hpp#L31-L44)):去 `ps`,遍历所有 provider 的 `devices_`,
  返回首个满足 config 的 device;`for_each_device`([:46-57](../include/rdma/nd/nd_device.hpp#L46-L57))每 device 迭代一次。
- 验收:`tests/nd/test_nd_device_manager.cpp` 改造后,同一网卡只出现一个 device、且 `v4_addr_`/`v6_addr_` 至少一个非空。

### Phase 2 -- nd 控制面按族选本地地址
改动文件:[tcp.hpp](../include/rdma/tcp.hpp)、
[nd_service_connector.hpp](../include/rdma/nd/detail/nd_service_connector.hpp)、
[nd_service_listener.hpp](../include/rdma/nd/detail/nd_service_listener.hpp)。
- `tcp` 暴露族:新增 `int family() const noexcept { return impl_.family(); }`;**删除 `get_adapters`**
  ([tcp.hpp:49-56](../include/rdma/tcp.hpp#L49-L56),发现层不再需要它)。
- connector `start_connect_op`([:419-459](../include/rdma/nd/detail/nd_service_connector.hpp#L419-L459)):
  把 `local_ep = make_address(adapter_->name_)`([:433](../include/rdma/nd/detail/nd_service_connector.hpp#L433))
  改为按 `endpoint` 族从 `device->local_addr_for(family)` 取本地地址 bind;无则 `address_family_not_supported`。
- listener:`open(ps)` 记下端口空间族;`bind`([:132-140](../include/rdma/nd/detail/nd_service_listener.hpp#L132-L140))
  按该族选 `device->local_addr_for(family)` 替代 `make_address(adapter_->name_)`
  ([:139](../include/rdma/nd/detail/nd_service_listener.hpp#L139))。
- 验收:nd echo 在 v4 与 v6 各跑通(需硬件)。

### Phase 3 -- `get_first_available_device` 去 `ps`(两后端)+ 迁移调用点
- ibv 侧同步去 `ps`([ibv_device.hpp:37-47](../include/rdma/ibv/ibv_device.hpp#L37-L47))。
- **不保留过渡重载**(决策 §9.1):直接把签名改为 `(config = {})`,**一次性迁移全部调用点**(见 §7),
  靠编译失败兜住遗漏。
- 更新 `CLAUDE.md` 示例(`get_first_available_device(tcp::v4(), {})` -> `({})`)。
- 验收:全量编译 + 回归绿。

### Phase 4 -- ibv 对齐与验证(细节见 §8)
- ibv 数据面天然双族(MR/PD/CQ 与族无关),无数据面改动;去 `ps`(并入 Phase 3)。
- **验证(不是想当然)**:同一 `use_device` 的 device 上,v6 echo **真正往返成功**(RoCE/RoCEv2,GID 表含 v6),
  不只是 resolve 成功 -- 见 §8.3。
- (可选增量)ibv listener 单实例双栈;多网卡下把 connector/listener 钉在 use_device 的 device 上 -- 见 §8.2/§8.5,列为后续增量,不阻塞主线。
- 验收:ibv v4/v6 双族往返通过。

### Phase 5 -- 测试 + 硬件假设验证

**现状(零覆盖):** 没有任何 v6 数据面往返测试,所有 echo/connect/send-recv 都写死 `tcp::v4()`
(如 [tests/ibv/test_ibv_echo.cpp:35](../tests/ibv/test_ibv_echo.cpp#L35) / [:93](../tests/ibv/test_ibv_echo.cpp#L93));
仅有的 v6 出现是 endpoint 构造([tests/unit/rdma/tcp.cpp:19-21](../tests/unit/rdma/tcp.cpp#L19-L21))
和将被删除的 per-family `get_first_available_device(tcp::v6(), ...)`
([test_nd_device_manager.cpp:32-36](../tests/nd/test_nd_device_manager.cpp#L32-L36)、
[test_ibv_device_manager.cpp:32-36](../tests/ibv/test_ibv_device_manager.cpp#L32-L36))。本阶段补齐这个核心缺口。

**新增 `tests/rdma/test_rdma_dual_family_echo.cpp`(可移植,两后端共用一份源码,`--server`/`--client` 形态,
对齐 [test_rdma_echo.cpp](../tests/rdma/test_rdma_echo.cpp) 的结构):**

核心用例 = **同一个 device、同一次 `use_device`,先用 v4 地址完成一次 echo,再用 v6 地址完成一次 echo**:

```cpp
// 一次发现 + 一次 use_device(证明"一个 device 双族")
auto dev = rdma_device_manager_t::instance().get_first_available_device({});
rdma::use_device(io_ctx, dev);                 // 仅注册一次,不再 per-family

// run_server(co):顺序两轮,复用同一 device
co_await echo_round(io_ctx, dev, tcp::v4(), port);   // 第 1 轮:v4 listener.open(tcp::v4()) + accept + echo
co_await echo_round(io_ctx, dev, tcp::v6(), port);   // 第 2 轮:v6 listener.open(tcp::v6()) + accept + echo
// run_client(co):
co_await echo_once(io_ctx, dev, v4_endpoint);        // 连 v4,收发校验
co_await echo_once(io_ctx, dev, v6_endpoint);        // v4 完成后再连 v6,收发校验
```

断言:
- 两轮 echo 的收发内容均正确,且**第二轮(v6)不需要重新 `use_device`** -- 证明同一 device 同时支持 v4/v6;
- 顺序性:v6 轮在 v4 轮**完成之后**发起(用例名 `v4_then_v6`)。
- **生命周期**(见 §8.4):connector 一次性、listener `open` 每次新建 cm_id,故第二轮(v6)必须**新建 connector
  + 新建/重开 listener**,不能复用第一轮对象;QP 同理每轮新建。`use_device` 全程只一次。
- 负路径(同源码内):若 device 缺某族本地地址,`open(tcp::vX())` / `async_connect(vX_ep)` 返回
  `address_family_not_supported`(Phase 0/2 引入的码),用例对该路径做条件断言(有该族地址才跑对应轮)。
- 客户端入参:`--client-v4 HOST4 --client-v6 HOST6`(或单 `--client HOST4 HOST6`),两轮分别用对应地址。

**device_manager 测试改造**(`tests/{nd,ibv}/test_*_device_manager.cpp`):删掉 per-family 的
`test_get_first_available_device_v4/v6` 双用例,改为 `get_first_available_device({})` 单次,断言返回 device 的
`v4_addr_`/`v6_addr_` 至少一个非空(双族都 present 时两者都非空)、`name_` 仍非空。

**硬件验证 AdapterId 假设:** 改造 `ndadapterinfo` 风格小程序,打印同一网卡 v4/v6 地址各自 `ResolveAddress`
的 id,确认相等。

- 验收:`test_rdma_dual_family_echo` 的 `v4_then_v6` 用例在 v4 与 v6 上顺序往返均通过(需双族地址的硬件);
  device_manager 测试断言双族;AdapterId 假设上机确认。

---

## 7. 影响面 / 迁移清单(来自全量 grep)

- **去 `ps` 的 `get_first_available_device` 调用点**(约 40+ 处,遍布
  `tests/{ibv,nd,rdma,benchmark,stress,unit}`):统一 `get_first_available_device(tcp::v4(), {})` -> `({})`。
  典型:[tests/rdma/test_rdma_echo.cpp:174](../tests/rdma/test_rdma_echo.cpp#L174)、
  [tests/benchmark/send_recv.cpp](../tests/benchmark/send_recv.cpp)(8 处)、
  [tests/benchmark/read_write.cpp](../tests/benchmark/read_write.cpp)(4 处)、
  device_manager 测试的 v4/v6 双用例需合并。
- **`name_`(单地址)使用点**:
  - nd 控制面 [nd_service_connector.hpp:433](../include/rdma/nd/detail/nd_service_connector.hpp#L433)、
    [nd_service_listener.hpp:139](../include/rdma/nd/detail/nd_service_listener.hpp#L139) -> 改为 `local_addr_for(family)`。
  - 写入点 [nd_device_impl.hpp:408](../include/rdma/nd/detail/nd_device_impl.hpp#L408) -> 保留 `name_` 作展示。
  - 测试断言 `tests/{nd,ibv}/test_*_device_manager.cpp` -> `name_` 仍非空,保持。
  - ibv 的 `name_`([ibv_device_impl.hpp:39](../include/rdma/ibv/detail/ibv_device_impl.hpp#L39))是设备名(mlx5_0),语义不同,**不动**。
- **`v4_adapters_`/`v6_adapters_`**:[nd_impl_types.hpp:127-128](../include/rdma/nd/detail/nd_impl_types.hpp#L127-L128)、
  [nd_device.hpp:50-53](../include/rdma/nd/nd_device.hpp#L50-L53)、
  [tcp.hpp:53-55](../include/rdma/tcp.hpp#L53-L55)、
  [nd_device_impl.hpp:486-488](../include/rdma/nd/detail/nd_device_impl.hpp#L486-L488) -> 全部改用 `devices_`。
- **`tcp::get_adapters`**:仅 [nd_device.hpp:36](../include/rdma/nd/nd_device.hpp#L36) 使用 -> 删除该方法,改 `family()`。
- **`open(tcp::v4())` 等控制面调用**:语义不变(端口空间仍传),但现在会真正用于选族(此前 nd 侧被忽略)。
- **新增文件**:`tests/rdma/test_rdma_dual_family_echo.cpp`(v4-then-v6 同 device 顺序往返,见 Phase 5)+
  对应 `tests/rdma/CMakeLists.txt` 注册;`tests/{nd,ibv}/test_*_device_manager.cpp` 的 v4/v6 双用例合并为单次断言双族。

---

## 8. ibv 平台补充

> 在 ibv 平台核对后,纠正前文"ibv 已双族、验证即可"的轻描淡写。ibv **数据面确实天然双族**
> (MR/PD/CQ 与族无关),但以下 ibv 特有点这份(以 nd 为主的)plan 需补上。

### 8.1 family 在 ibv 是"穿透",不是"选 adapter" -- 大部分 nd 改动不适用
- nd 要给 device 加 `v4_addr_/v6_addr_` 并显式 `Bind` 本地地址;**ibv 不需要**:
  - connector:族由 `async_connect` 目标 endpoint 的 sockaddr 携带,`resolve_addr(cm_id, nullptr, dst)`
    ([ibv_service_connector.hpp:396-398](../include/rdma/ibv/detail/ibv_service_connector.hpp#L396-L398)、
    [ibv_ops_cm.hpp:105-111](../include/rdma/ibv/detail/ibv_ops_cm.hpp#L105-L111))。
  - listener:族由 `open(ps)` 的 `ps.any_endpoint(0)` 决定(绑 `0.0.0.0` 或 `::`)
    ([ibv_service_listener.hpp:110](../include/rdma/ibv/detail/ibv_service_listener.hpp#L110))
    -- 这正是本 plan 想让 nd 模仿的"按 port space 选族",ibv 已同构。
- ibv device 结构只有 `{context_, pd_, name_}`,`name_` 是设备名(mlx5_0)**不是 IP**
  ([ibv_impl_types.hpp:71-75](../include/rdma/ibv/detail/ibv_impl_types.hpp#L71-L75));ibv **不存、也不需要**本地 IP。
- 故 **§5 数据结构改动、§6 Phase 1/2 基本是 nd-only**;ibv 侧只剩"去 ps(Phase 3)+ 验证(Phase 4)+(可选)多网卡设备绑定"。

### 8.2 多网卡设备绑定歧义(ibv 真正的缺口,单网卡 v4/v6 不触发)
- 前提:**一个 io_context 只绑一个 device**(见 §4 核心不变量),故 io_context 内**没有第二个 device 可回退**。
  风险不是"在多个 device 间选错",而是"内核把某条连接路由到了**这唯一 device 之外**的网卡 -> create_qp 必然失败"。
- QP 在 **device_service 注册的那个 PD** 上、于 cm_id 上创建
  ([ibv_queue_pair.hpp:91](../include/rdma/ibv/ibv_queue_pair.hpp#L91)、
  [ibv_service_verbs.hpp:64,83](../include/rdma/ibv/detail/ibv_service_verbs.hpp#L64-L83));
  ibv 要求 **cm_id 的 verbs context 必须与该 PD 同属一块 device**,否则 `rdma_create_qp` 失败。
- connector 现在 `resolve_addr(src=nullptr)`,内核**按路由挑本地 device** -- 可能不是 `use_device` 的那个;
  listener 绑 wildcard,连接可能从**另一块网卡**进来(child cm_id 的 context != 注册 PD)。两者都会让
  `create_qp` 在 PD/context 不匹配时失败。
- **单网卡 v4/v6(本 plan 目标场景)永远命中同一 device,不触发此问题。** 但要写明这是**单网卡假设**;
  多网卡下"把 connector/listener 钉在 use_device 的 device 上"是独立课题:需给 resolve_addr 传一个属于该
  device 的 src 地址 / 给 listener 绑该 device 的本地 IP,而 ibv device 当前没存 IP(要反查 netdev/GID 表)。**列为后续增量。**

### 8.3 RoCE/rdma_cm 的 IPv6 是"需验证",不是"想当然"
- RoCEv2 支持 IPv6,但跑通依赖:GID 表里有 v6 GID 条目、gid_type 匹配、`resolve_route` over v6 正常。
- Phase 4/5 的 ibv 验收必须**显式**包含:同一 `use_device` 的 device 上 v6 echo **真正往返成功**(不只是 resolve 成功)。

### 8.4 dual-family echo 测试在 ibv 的生命周期注意(对 nd 同样成立)
- connector 是**一次性**的(连过/断过即 `connector_terminal`,见
  [ibv_service_connector.hpp:224-234](../include/rdma/ibv/detail/ibv_service_connector.hpp#L224-L234));listener `open` 每次建新 cm_id。
- 故 `v4_then_v6` 测试第二轮(v6)必须**新建 connector**(以及新建/重开 listener),不能复用第一轮对象。
  这条需在 Phase 5 测试设计里写明。

### 8.5 listener 单实例双栈是什么 / 两后端支持度不对称
- **含义:** 一个 `rdma_listener` 对象**同时**接受 v4 与 v6 入连接(类比 TCP acceptor 绑 `::` + `IPV6_V6ONLY=0` 的双栈)。
- **当前(简单)模型:** 一个 listener `open(tcp::v4())`/`open(tcp::v6())` **只收一族**;要同收两族就开**两个** listener。
- **ibv:原生支持,近乎免费。** rdma_cm 暴露了 socket V6ONLY 的对应物 `RDMA_OPTION_ID_AFONLY`
  (系统头 `rdma_cma.h`,注释 `~IPV6_V6ONLY`;经 `rdma_set_option` 设置)。一个 cm_id 绑 `::` 且 V6ONLY 关
  -> **单个 listening cm_id** 在**同一条 event channel** 上同时收到 v4(-mapped) 与 v6 的 CONNECT_REQUEST,
  现有 `async_get_connection` 一条循环即可处理,**无需多路复用**。
- **nd:原生不支持。** `IND2Listener::Bind` 是**单族**的;`IN6ADDR_ANY` 只在 v6 内 multi-homed
  ("cannot span multiple adapters",[IND2Listener.md:45-46](../third_party/networkdirect/docs/IND2Listener.md#L45-L46)),
  没有 AFONLY/V6ONLY 旋钮。要同收两族,必须 `rdma_listener` **内部持两个 IND2Listener**(各绑该 adapter 的 v4/v6 地址),
  并把 `async_get_connection` **多路复用**两个源(竞争等待、先到先返回、取消/重新 arm)、teardown 管两个。
- **结论(不对称):** 单实例双栈在 **ibv 近乎免费**(绑 `::` + 设 AFONLY),在 **nd 是重活**(双 IND2Listener + 多路复用)。
- **本期 deferred:** 双族 echo 目标不需要它(顺序 `open(v4)`/`open(v6)` 即可)。ibv 因近乎免费可在 Phase 4 顺手做;
  nd 留作后续增量。
- **运行期 caveat:** AFONLY 是 API 级能力;v4-mapped 连接在 RoCEv2(GID 寻址)下能否真正建链需上机验证(见 §8.3)。

---

## 9. 决策记录

1. **`get_first_available_device` 不保留过渡重载** -- 直接改签名为 `(config = {})`,一次性迁移全部调用点
   (Phase 3),靠编译失败兜住遗漏。
2. **device 选择阶段不按族过滤** -- `get_first_available_device(config)` 只看 caps 返回首个满足者;族由 control
   plane 决定(贴合 asio)。若该 device 缺所需族地址,在 control plane 报 `address_family_not_supported`(见决策 4)。
3. **listener 单实例双栈:默认不做,支持度不对称(详见 §8.5)** -- 默认模型"一 listener 一族,同收两族开两个 listener"。
   **ibv 原生支持**(绑 `::` + `RDMA_OPTION_ID_AFONLY`,近乎免费,可在 Phase 4 顺手做);**nd 原生不支持**
   (需内部双 IND2Listener + 多路复用,留作后续增量)。双族 echo 目标用顺序 `open(v4)`/`open(v6)` 即可,不阻塞主线。
4. **`local_addr_for` 找不到匹配族地址:报错(不回退 any)** -- 显式返回 `address_family_not_supported`
   (Phase 0/2 引入的码),不静默回退到 any 地址。

---

## 10. 2026-06-22 实现状态复查

已完成:
- nd device 发现层已由 `v4_adapters_` / `v6_adapters_` 改为按 `AdapterId` 归并的 `devices_`;
  `nd_adapter_t` 持有 `v4_addr_` / `v6_addr_` 与 `local_addr_for(family)`。
- nd connector/listener 已在控制面按 endpoint / `open(ps)` 的 family 选择本地地址,缺族时返回
  `rdma_errc::address_family_not_supported`。
- nd/ibv 的 `get_first_available_device(config)` API 已统一,现有调用点不再传 `tcp::v4()` / `tcp::v6()`。
- `tcp` 已删除 nd-only adapter selection 语义,改为暴露 `family()`。
- 已新增跨平台 `test_rdma_dual_family_echo` 可执行文件,用于手动跑 v4-then-v6 同 device echo。

2026-06-22 手动验证:
- Windows/ND 单机上同一 device 报告 `v4=yes v6=yes`。
- `test_rdma_dual_family_echo` 使用 `10.234.66.130` 与 link-local IPv6
  `fe80::de0e:a231:21f7:331%9` 手动跑通 v4-then-v6 echo,两轮 client/server 均打印 `OK`。

仍需补强:
- `test_rdma_dual_family_echo` 目前只编译,未纳入 CTest 自动编排;需要一个 server/client harness
  或单进程双端测试,让 dual-family 往返能成为 CI/CTest 的硬验收。
- 该测试的 round 失败目前主要靠日志表达;纳入自动化前需把失败传回 `main` 的 exit code,并明确“缺少某族地址”
  是 skip 还是 fail。
- 手动验证时 server 两轮 echo 完成后没有在 20 秒内自然退出;自动化前需修正 teardown/退出路径,避免 CTest
  或脚本挂住。
- v6 硬件验证还应扩展到非 link-local/global IPv6 和 ibv 环境;当前仅覆盖 Windows/ND link-local IPv6。
- ibv 单网卡双族模型已经对齐;多网卡下 rdma_cm 路由与 `use_device` 绑定 device 不一致的问题仍属于后续增量。
