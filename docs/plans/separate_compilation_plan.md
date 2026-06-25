# Separate-Compilation Plan -- `ASIO_SEPARATE_COMPILATION` for rdma_on_asio

> 目标:让 rdma_on_asio 全面支持 `ASIO_SEPARATE_COMPILATION`,完全对齐 asio 的设计 ——
> **当定义了 `ASIO_SEPARATE_COMPILATION`(asio 据此关闭 `ASIO_HEADER_ONLY`)时,把所有"普通(非模板)"
> 函数的声明与实现分开**:声明留在 `.hpp`(直接挂 asio 的 `ASIO_DECL`),实现移到相邻 `impl/` 目录下的同名 `.ipp`,
> 由唯一一个源 TU(消费者/测试写的、仅 `#include <rdma/impl/src.hpp>` 的 `.cpp`;**库本身不 ship 这个 `.cpp`**)编译一次。
> **模板(类模板 / 函数模板 / 成员模板)不拆**,仍全部留在头里。
> 默认(header-only)行为不变。
>
> 状态:**计划态(未实现)**。先落基础设施 + shared 试点,再 ibv,再 nd(nd 已有部分 separate-compilation 先例,需对齐)。
> 相关:`nd_cmake_refactor_plan.md`(已有的 `src/networkdirect.cpp` / `impl/networkdirect.hpp` / `nd_autolink.hpp` 先例)、
> `cmake_test_unification_plan.md`(统一 CTest 图,新增的 separate-compilation 测试挂进去)。

---

## 1. asio 的机制(已核对,file:line)

asio 的 optional separate compilation 由三件套构成,全部在 vendored asio 里可查:

1. **开关与 `ASIO_DECL`**(`third_party/asio/include/asio/detail/config.hpp:61-89`):
   - 若既未定义 `ASIO_HEADER_ONLY` 也未定义 `ASIO_SEPARATE_COMPILATION`,则默认 `#define ASIO_HEADER_ONLY 1`(:61-67)。
   - `#if defined(ASIO_HEADER_ONLY)` → `# define ASIO_DECL inline`(:70);否则为空(或 dll import/export,:78-80);
     最后兜底 `#if !defined(ASIO_DECL) # define ASIO_DECL`(:87-89)。
   - 即:**header-only 时 `ASIO_DECL`=`inline`,separate 时为空**。用户只要(项目级)定义 `ASIO_SEPARATE_COMPILATION`,
     `ASIO_HEADER_ONLY` 自动关闭。

2. **每个组件的"声明 .hpp + 实现 .ipp"对**(范例 `asio/error_code.hpp` + `asio/impl/error_code.ipp`):
   - 头里声明挂 `ASIO_DECL`:`extern ASIO_DECL const error_category& system_category();`(`error_code.hpp:29`)。
   - 头**底部**仅在 header-only 时把实现拉进来(`error_code.hpp:35-37`):
     ```cpp
     #if defined(ASIO_HEADER_ONLY)
     # include "asio/impl/error_code.ipp"
     #endif
     ```
   - `.ipp` 自带 include guard + 不重复 `inline`(`impl/error_code.ipp:11-18`):
     ```cpp
     #ifndef ASIO_IMPL_ERROR_CODE_IPP
     #define ASIO_IMPL_ERROR_CODE_IPP
     #include "asio/detail/config.hpp"
     ... // 定义体,函数不再写 inline;linkage 由头里 ASIO_DECL 决定
     #endif
     ```
   - 关键语义:声明上的 `ASIO_DECL`(header-only=`inline`)决定 linkage。header-only 时 `.ipp` 被文本包进头 →
     定义随 `inline` 声明成为 inline(多 TU ODR 安全);separate 时 `.ipp` 只被源 TU 编译一次 → 外部定义,
     其它 TU 只见(非 inline)声明。

3. **唯一源 TU**:`asio/impl/src.hpp`(:12-40):
   ```cpp
   #define ASIO_SOURCE
   #include "asio/detail/config.hpp"
   #if defined(ASIO_HEADER_ONLY)
   # error Do not compile Asio library source with ASIO_HEADER_ONLY defined
   #endif
   #include "asio/impl/error_code.ipp"
   ... // 列出所有 .ipp
   ```
   用户在**一个** `.cpp` 里(项目级定义了 `ASIO_SEPARATE_COMPILATION` 后)`#include <asio/impl/src.hpp>`,
   编译出唯一一份库实现。

4. **模板**:standalone asio **不用** extern-template(无 `ASIO_NO_EXTERN_TEMPLATE` 实际启用),模板实现全部留在
   `.hpp` / `detail/impl/*.hpp`。**拆分规则:非模板定义 → `.ipp`;模板 + `inline` + `constexpr` → 留头里**。

---

## 2. 仓库已有的先例(必须对齐,不要另起炉灶)

nd 后端已经有一套 separate-compilation 机制(见 `nd_cmake_refactor_plan.md`):
- `src/networkdirect.cpp` 是 nd 的源 TU(`#define ASIO_RDMA_NETWORKDIRECT_SOURCE_FILE` + include `rdma/nd/impl/networkdirect.hpp`)。
- `include/rdma/nd/impl/networkdirect.hpp` 是"impl 伞":`#error` 守卫(separate 模式下被非源 TU 直接包含即报错)+ 文本包含 ndutil 实现源。
- `include/rdma/nd/detail/nd_autolink.hpp` 在 `ASIO_SEPARATE_COMPILATION && !ASIO_NO_DEFAULT_LINKED_LIBS && _MSC_VER` 下 `#pragma comment(lib, "ndutil.lib")`,由 `nd_types.hpp` 引入。

本计划**扩展**这套约定到"rdma 自身的非模板实现",而不是替换它。`networkdirect.hpp` 是新 `.ipp` 伞的样板。
(ibv 侧无 vendored native lib,不需要 autolink;见 `ibv_cmake_refactor_plan.md`。)

---

## 3. 设计

### 3.1 直接复用 asio 的 `ASIO_DECL`(不新增 `RDMA_DECL`)

**不引入任何平行宏**。rdma_on_asio 依赖 asio,`ASIO_DECL`(`asio/detail/config.hpp:69-89`)已经:
- header-only(默认,即未定义 `ASIO_SEPARATE_COMPILATION`)→ `inline`;
- separate(定义了 `ASIO_SEPARATE_COMPILATION`)→ 为空;
- 且在 `ASIO_DYN_LINK` + `ASIO_SOURCE` 下自动走 `__declspec(dllexport/dllimport)`。

它本就跟随用户的 `ASIO_SEPARATE_COMPILATION`,还白送 dyn-link 的 import/export 分支 —— 正是我们要的语义。
所以**所有 rdma 非模板声明直接挂 `ASIO_DECL`**,不再有 `RDMA_DECL` / `rdma_config.hpp` / `RDMA_SOURCE`。

唯一注意:用到 `ASIO_DECL` 的头必须能看到 `asio/detail/config.hpp`(rdma 头几乎都已 include 了某个 asio 头;
个别纯 native 头若没有,显式补 `#include "asio/detail/config.hpp"`)。`ASIO_SEPARATE_COMPILATION` 统一由
构建/source-config header 设置(见 §6),不引入独立的 rdma 开关。

### 3.2 文件布局(镜像 asio 的相邻 `impl/`)

每个含可拆分实现的头 `X.hpp`,在其**同级目录**新建 `impl/X.ipp`:

```
include/rdma/rdma_error.hpp                  -> include/rdma/impl/rdma_error.ipp
include/rdma/detail/<x>.hpp                   -> include/rdma/detail/impl/<x>.ipp
include/rdma/ibv/<x>.hpp                      -> include/rdma/ibv/impl/<x>.ipp
include/rdma/ibv/detail/<x>.hpp              -> include/rdma/ibv/detail/impl/<x>.ipp
include/rdma/nd/<x>.hpp                       -> include/rdma/nd/impl/<x>.ipp   (impl/ 已存在)
include/rdma/nd/detail/<x>.hpp               -> include/rdma/nd/detail/impl/<x>.ipp
```

### 3.3 单组件改造模板

`X.hpp`(声明,挂 `ASIO_DECL`,底部按 header-only 拉 .ipp)。自由函数与成员/ctor/dtor 都适用:
```cpp
#include "asio/detail/config.hpp"   // 确保 ASIO_DECL 可见(通常已被其它 asio 头带入)
// ... 类型定义 / 模板 / constexpr / 概念,全部留在这里 ...

// (a) 自由函数:
ASIO_DECL asio::error_code make_error_code(rdma_errc e);

// (b) 非模板类的成员 / ctor / dtor:类内挂 ASIO_DECL 声明,类外定义进 .ipp
class ibv_completion_queue {
  ASIO_DECL ibv_completion_queue(rdma_device_ptr const&, rdma_config_t const&);
  ASIO_DECL std::size_t poll();
  // 模板成员 / 一行访问器仍内联留头
};

#if defined(ASIO_HEADER_ONLY)
# include "rdma/impl/X.ipp"
#endif
```
`impl/X.ipp`(定义,自带 guard,自包含,夹 asio 的 push/pop_options):
```cpp
#ifndef RDMA_IMPL_X_IPP
#define RDMA_IMPL_X_IPP
#include "rdma/X.hpp"                      // 看到声明 + 类型
#include "asio/detail/push_options.hpp"
// 定义体不再写 inline / ASIO_DECL(linkage 由头里声明上的 ASIO_DECL 决定):
asio::error_code make_error_code(rdma_errc e) { ... }
std::size_t ibv_completion_queue::poll() { ... }
#include "asio/detail/pop_options.hpp"
#endif
```

### 3.4 源伞 `include/rdma/impl/src.hpp` —— **合并 asio + rdma,消费者只 include 一个**

为把消费者的配置成本降到最低(决策:见 §9),`rdma/impl/src.hpp` **自己 `#include "asio/impl/src.hpp"`**,
从而一个 include 同时编译 asio 与 rdma 的非模板实现。**关键:不自己 `#define ASIO_SOURCE`** —— 让
`asio/impl/src.hpp` 负责(它定义 `ASIO_SOURCE` 且不 undef,故后续 rdma `.ipp` 也在 `ASIO_SOURCE` 下编译,
dyn-link 时 `ASIO_DECL` 正确 dllexport)。避免 `ASIO_SOURCE` 重定义。

```cpp
#ifndef RDMA_IMPL_SRC_HPP
#define RDMA_IMPL_SRC_HPP
#include "asio/detail/config.hpp"
#if defined(ASIO_HEADER_ONLY)
# error Do not compile rdma_on_asio source with ASIO_HEADER_ONLY defined
#endif
#include "asio/impl/src.hpp"     // 编译 asio 实现 + 定义 ASIO_SOURCE(不要自己再 define)
#include "rdma/rdma.hpp"         // 声明可见(并设定后端宏)
// --- 共享层 .ipp ---
#include "rdma/impl/rdma_error.ipp"
// --- 仅活动后端的 .ipp(按后端宏门控)---
#if defined(ASIO_RDMA_BACKEND_VERBS)
#  include "rdma/ibv/detail/impl/ibv_device_impl.ipp"
#  include "rdma/ibv/detail/impl/ibv_ops_cm.ipp"
#  ... // 见 §4 ibv 清单
#elif defined(ASIO_RDMA_BACKEND_ND)
#  include "rdma/nd/detail/impl/nd_device_impl.ipp"
#  ... // 见 §4 nd 清单(networkdirect.hpp 先例并存)
#endif
#endif
```

**库交付物只有这个头伞 `rdma/impl/src.hpp`** —— **库不 ship 任何 `src/*.cpp`**(对齐 asio:asio 只 ship
`asio/impl/src.hpp`)。那个一行源 `.cpp` 是**消费者**(或本仓库**测试**)写的。

**消费者用法(对齐 asio,最小配置):**
1. 给**所有 TU**定义 `ASIO_SEPARATE_COMPILATION`(构建期 `-D` / `target_compile_definitions`);
2. 自己写**一个** `.cpp`,内容仅 `#include <rdma/impl/src.hpp>` —— 仅此一个 include,asio + rdma 实现都进来。

无需再单独 include `asio/impl/src.hpp`,也无需第二个源文件(asio 实现已被 `rdma/impl/src.hpp` 带入)。
四种 header-only/separate × default/no-default-linked 组合的完整示例见
**`docs/separate_compilation_usage.md`**(README 引用)。

---

## 4. 拆什么 / 留什么(规则 + 清单)

**拆分判据(两个轴,对齐 asio):**
- **模板轴**:仅拆**非模板**函数(自由函数 / 非模板类的成员函数 / ctor / dtor);模板(类/函数/成员模板)、
  `constexpr`、类型/结构体/枚举/概念定义、`std::is_error_code_enum` 特化、`inline` 变量(如 nd 的
  `manual_winsock_init`)一律留头。
- **冷/热轴(本轮新增,重要)**:在"非模板"里再分冷热。
  - **冷**(放心拆 → `.ipp`):device 发现(`*_device_impl`)、error 表(`*_error`)、`*_config_derive`、
    service 生命周期(`construct/destroy/shutdown/register_*/initialize`)、ctor/dtor(MR/CQ)、`use_device`、
    CM 控制面包装(`*_ops_cm`)、`get_v*_address`。
  - **热**(默认**保持 inline 留头**):数据面 leaf wrapper —— `*_ops_verbs` 的
    `post_send/post_recv/post_read/post_write`、`poll_cq`,以及 `fill_native_sge`。它们的调用点是**留在头里的
    模板 op**(`rdma_*_op` / `do_post_*`),header-only 时能内联;一旦外部化,separate 模式下跨 TU 调用**丢失内联**
    (除非 LTO),直接打在 RDMA 收发热路径上。除非显式开启 LTO,**这些不拆**。

> 说明:`inline` 本就 ODR 安全;拆分的收益是**编译时间 + 代码体积**(实现只编一次),不是 ODR。所以
> (a) 琐碎一行的不拆(无收益),(b) 热路径 leaf wrapper 不拆(护内联)。§4.2/§4.3 清单里标了 `ops_verbs`
> 的 post_*/poll_cq 与 fill_native_sge 为"候选",**按此判据默认归入'热,留头'**,仅在 LTO 构建下才考虑拆。

### 4.1 共享层(`include/rdma/`)
| 头 | 拆分候选(→ impl/.ipp) | 备注 |
|----|------------------------|------|
| `rdma_error.hpp` | `rdma_error_category::message`、`get_rdma_error_category`、`make_error_code(rdma_errc)` | **试点首选**;留 enum / category 声明 / `name()` / `is_error_code_enum` 特化 |
| `rdma_commons.hpp` / `rdma_buffer.hpp` / `rdma_types.hpp` / `rdma.hpp` / `tcp.hpp` / `detail/rdma_verbs_op.hpp` / `detail/rdma_op_*.hpp` / `detail/small_sglist.hpp` | 无 / 仅琐碎 | 全是模板/概念/value-type/一行访问器 → **不拆** |

### 4.2 ibv 后端(`include/rdma/ibv/`)
| 头 | 拆分候选 |
|----|----------|
| `ibv_error.hpp` | `make_system_error_code` / `last_system_error` / `throw_error` |
| `detail/ibv_config_derive.hpp` | `cap_of` / `derive_effective_config` / `is_config_compatible`(留 6 个 constexpr 默认值) |
| `detail/ibv_device_impl.hpp` | **最大块**:`create_device`(2)/`is_valid_device`/`to_ip_address`/`sockaddr_size`/`is_candidate_local_address`/`bindable_context_for`/`attach_device_address`/`attach_local_addresses`/`has_local_address`/`get_devices`(2)/`ifaddrs_list` |
| `ibv_device.hpp` | `ibv_device_t::get_v4_address` / `get_v6_address`(留 `ibv_device_manager_t`,其 `for_each_device<Func>` 是成员模板) |
| `detail/ibv_ops_cm.hpp` | 全部 cm 包装:`create_event_channel`/`create_cm_id`/`migrate_id`/`bind_addr`/`listen`/`resolve_addr`/`resolve_route`/`connect`/`accept`/`disconnect`/`get_cm_event`/`drain_cm_events`/`set_nonblocking` |
| `detail/ibv_ops_verbs.hpp` | 全部 verbs 包装:`create_comp_channel`/`create_cq`/`create_qp`/`req_notify_cq`/`get_cq_event`/`ack_cq_events`/`poll_cq`/`reg_mr`/`dereg_mr`/`modify_qp`/`post_{recv,send,rdma,read,write}` |
| `detail/ibv_op_complete.hpp` | `wc_status_to_ec`/`resolve_verbs_op`/`ibv_complete_op::do_complete` |
| `detail/ibv_op_connect.hpp` | **非模板基** `ibv_connect_op_base` 的 `advance`/`claim_closed`/`aborted_by_disconnect`/`do_perform`/`do_process*`/ctor(留模板派生 `ibv_connect_op`) |
| `detail/ibv_op_wait_disconnect.hpp` | `ibv_wait_disconnect_op_base::do_perform` + ctor(留模板派生) |
| `detail/ibv_op_cm.hpp` | `ibv_op_cm::get_cm_event` + 受保护 ctor(留 `cm_op_cancellation` 类型 + `arm_cm_cancellation` 模板) |
| `detail/ibv_service_base.hpp` | ctor/`base_construct`/`base_move_construct`/`base_destroy`/`do_insert`/`do_remove`(留 `base_shutdown<...>` 模板) |
| `detail/ibv_service_device.hpp` | `shutdown`/`register_device`(留琐碎访问器) |
| `detail/ibv_service_io_completion.hpp` | ctor/`shutdown`/`initialize`/`ensure_poller_started`/`ibv_poll_wc_op::do_*`/`poll_into`/`arm_poller`/`on_poll_complete` |
| `detail/ibv_service_verbs.hpp` | `create_qp`(static 非模板)/`finish_event`/ctor(留 `async_*` / `do_post_*` / `is_single_buffer_sequence_v` 等模板) |
| `ibv_queue_pair.hpp` | `bind(io_context&,ec)` / `bind(completion_queue&,ec)`(留 `async_*` 模板、`make_create_qp_fn`、访问器) |
| `ibv_completion_queue.hpp` | ctor/`poll`(2)/`poll_one`(2)/`push_ready`/`dispatch`/`drain_ready`/`pop_ready`(留访问器) |
| `ibv_mr.hpp` | ctor/`throw_reg_mr`/`remote_addr`/`slice`/`cslice`(留 deleter、访问器、`is_in_mr`) |
| `ibv_use_device.hpp` | `use_device`(ec 版 + throwing 版) |
| `ibv_buffer.hpp` | `fill_native_sge`(2)(**边际收益**,可选;`build_native_sglist`/`buffers2sglist` 是模板,留) |
| `detail/ibv_service_{connector,listener}.hpp`、`detail/ibv_op_{accept,get_connection_request}.hpp` | **不拆** —— PortSpace / Handler 模板,全部留头 |

### 4.3 nd 后端(`include/rdma/nd/`)
对称于 ibv。主要拆分候选(详见各文件):
- `nd_error.hpp`(`message`/`get_nd_error_category`/`make_*_error_code`/`throw_error`)、
  `detail/nd_config_derive.hpp`(`derive_effective_config`)、
  **`detail/nd_device_impl.hpp`(最大块**:`get_providers`/`enumerate_*`/`create_device`/`open_device`/`discover_provider_devices`/`resolve_adapter_id`/`query_*`/`open_adapters`/`is_valid_adapter`(3)/`create_overlapped_file`/`is_valid_proto` 等 —— 但其中的 **ranges DSL 模板**`to/filter_map/sort_by/chunk_by` + `operator|` 必须留头)、
  `detail/nd_ops_verbs.hpp`、`detail/nd_ops_cm.hpp`、`nd_completion_queue.hpp`、`nd_mr.hpp`、`nd_use_device.hpp`、
  `detail/nd_service_io_completion.hpp`、`detail/nd_service_verbs.hpp`(`create_qp`)、`detail/nd_service_base.hpp`、
  `detail/nd_asio_manual_init.hpp`(`nd_global_t` ctor/dtor;`manual_winsock_init` inline 变量留头)、
  `detail/nd_op_base.hpp`、`detail/nd_op_complete.hpp`、`detail/nd_op_disconnect.hpp`、
  `detail/nd_op_connect.hpp`(非模板基 `nd_connect_op_base`)、`nd_device.hpp`(`get_v*_address`)。
- **不拆**:`*_service_{connector,listener}`(PortSpace 模板)、`nd_op_{accept,get_connection_request,wait_disconnect}`(Handler 模板)、`nd_queue_pair`/`nd_connector`/`nd_listener`、`nd_types.hpp`/`nd_impl_types.hpp`(类型)、ranges DSL 模板。
- 与现有 `impl/networkdirect.hpp` + `nd_autolink.hpp` 先例并存。

---

## 5. 分阶段实施

- **Phase 0 — 基础设施**:**只新增库交付物 `rdma/impl/src.hpp` 伞**(内含 `asio/impl/src.hpp` + `#error if ASIO_HEADER_ONLY`)。
  **库本身不提供 `src/rdma.cpp`** —— 那个一行源 `.cpp` 由**消费者/测试**编写(对齐 asio:asio 也只 ship `asio/impl/src.hpp`,
  不 ship `src/asio.cpp`)。本仓库的 separate 验证用例在 **测试目录**新增这样一个 `.cpp`(见 §6)。**无新增宏头**(直接用
  `ASIO_DECL`)。先空跑(无 .ipp)确保两态都能配置。
- **Phase 1 — shared 试点**:只改 `rdma_error.hpp` → `impl/rdma_error.ipp`,把它加进 `src.hpp`。两态构建 + 跑 ctest。**这是验证整套机制的最小闭环。**
- **Phase 2 — ibv**:按 §4.2 逐文件拆,每拆几个就两态构建。`ibv_device_impl` / `ibv_ops_*` 是大头。
- **Phase 3 — nd**:按 §4.3 拆,与 `networkdirect.hpp` 先例对齐(Windows agent 验证)。
- **Phase 4 — 测试矩阵**:见 §6,挂进统一 CTest 图。

每阶段内**先拆叶子(ops/error/config_derive),后拆依赖它们的(service/queue_pair/device)**,降低 include 顺序风险。

## 6. CMake / 构建 + 测试矩阵

**消费模型(决策 Q3 + 小提醒):库不提供项目级开关,完全对齐 asio 的消费者驱动模型。**
rdma_on_asio 本身 header-only(无交付的 `.a/.so`),所以 separate compilation 只能是**消费者**的决定 ——
项目级开关只会配置"库自己的构建"(无意义)。库的交付物就是头 + **合并的 `rdma/impl/src.hpp`**(见 §3.4);
消费者按 §3.4 用法(全 TU 定义 `ASIO_SEPARATE_COMPILATION` + 一个 `.cpp` include `rdma/impl/src.hpp`)即可。

- **不在 CMake 里 branch `ASIO_SEPARATE_COMPILATION`**(沿用 nd 计划维护规则);两态都由**测试侧 source-config header**
  设置宏,**测试自带完整的 CMake 脚本**模拟一个消费者的 separate 构建。
- 因 `rdma/impl/src.hpp` 已合并 asio 实现,**不需要**独立的 `src/asio.cpp`;separate 测试的源 TU 只是一个
  `#include <rdma/impl/src.hpp>` 的 `.cpp`(放测试目录即可,不必进 `src/`)。
- 复用 `tests/CMakeLists.txt` 现有 helper(`rdma_add_*`)+ `tests/unit/nd/asio_separate_*` 脚手架,挂进统一 CTest 图
  (labels 如 `unit;sepcomp`):
  - `unit_rdma_header_only_smoke`:默认态编一个含全部公共头的 TU,跑通。
  - `unit_rdma_separate_compilation_smoke`:separate 态编 src TU(`rdma/impl/src.hpp`)+ 一个用例 TU,链接、运行。
  - **ODR 检查** `unit_rdma_separate_odr`:separate 态编**两个**都 include 全部公共头的 TU + src TU,链接 —— 拆分不彻底
    (某非模板实现仍内联在头里)会 "multiple definition"。
  - **完整性检查** `unit_rdma_separate_linkage`:separate 态一个 TU **引用每个拆出的符号**(不含 src TU),链接 ——
    某 `.ipp` **漏进** `src.hpp` 会 "undefined reference"。ODR 抓重复、linkage 抓遗漏,互补。
  - **四组合矩阵**:header-only/separate × default/no-default-linked 各一个 smoke(对齐
    `docs/separate_compilation_usage.md` 的四个例子;Windows/nd 上四个 link 行为最完整,Linux/ibv 上 default/no-default
    在 ibverbs/rdmacm 这一侧无区别 —— 见 usage 文档说明)。
- ibv 在本机(有 RoCE)可端到端验证两态;nd 交 Windows agent。

**两条不变量(务必遵守):**
- **全程序单一模式**(消费者责任):`ASIO_SEPARATE_COMPILATION` 必须**整个可执行 all-or-nothing**。混用 header-only TU
  与 separate TU → ODR 冲突 / 重复或未定义符号。文档说明 + `unit_rdma_separate_odr/linkage` 兜底。
- **`src.hpp` 完整性**:每新增一个 `impl/*.ipp`,必须同步加进 `rdma/impl/src.hpp` 对应后端块(由
  `unit_rdma_separate_linkage` 兜底)。

## 7. 验证

- **两态等价**:header-only 与 separate 两种配置下,全量 ctest 同样通过(本机 ibv/RoCE)。
- **ODR 硬验证**:`unit_rdma_separate_odr`(两 TU + 源 TU)链接无重复符号 —— 证明所有"有体非模板"都已移入 .ipp。
- **编译产物**:separate 态下,公共头被多 TU 包含不再重复实例化非模板实现(可对比目标文件大小/编译时长作为佐证)。
- nd:Windows agent 编译两态 + 跑 nd/dual-family 用例。

> **收益预期(写实)**:本库**模板极重** —— 数据/控制面(services、ops、queue_pair async)全是模板,separate 模式下
> **照样在每个 TU 实例化**。separate compilation 只把**非模板实现体**(device 发现、error 表、CM 包装、ctor 等)从每个
> TU 移走。所以编译时间收益是**局部的**(与 asio 自身同性质:asio 的 `.ipp` 也只是 io_context/error/reactor 这类非模板核心)。
> 不要期待"大幅提速"。

## 8. 风险 / 决策

1. **`.ipp` 的 include 自包含性**:每个 `.ipp` 先 `#include` 其 `.hpp`(拿到声明 + 类型 + 依赖),避免顺序问题。叶子先拆。
2. **大文件**(`ibv_device_impl` / `nd_device_impl`):函数体里用到了头里的模板(如 nd 的 ranges DSL)。`.ipp` include 该头即可见模板,**模板留头、实现进 .ipp** 两不冲突。
3. **琐碎 inline 的边界**:一行访问器/工厂(`tcp::v4()`、`fill_native_sge`、service 访问器)**不拆**(asio 同此)。计划只拆"有实际工作体"的函数,避免无收益的碎片化。
4. **`make_error_code` ADL**:声明留头(挂 `ASIO_DECL`),ADL 仍可见;仅定义体进 `.ipp`。
5. **与 nd 既有 separate-compilation 先例**:不冲突 —— `networkdirect.hpp` 管的是 vendored ndutil 源;本计划管的是 rdma 自身实现。`src.hpp` 伞按后端宏只引入活动后端的 `.ipp`。
6. **不引入平行宏**:直接用 `ASIO_DECL` / `ASIO_SOURCE` / `ASIO_SEPARATE_COMPILATION`,不要 `RDMA_DECL` / `RDMA_SOURCE` /
   `RDMA_SEPARATE_COMPILATION`。语义、dyn-link(dllexport/import)分支全由 asio 提供。
7. **DLL / shared-lib 范围**:本计划主要面向 **static / object 形态的 separate compilation**(`ASIO_DECL` 为空)。
   若将来要把 rdma 编成 shared library,因为复用了 `ASIO_DECL` + `ASIO_SOURCE`,在 `ASIO_DYN_LINK` 下会自动走
   `dllexport`(源 TU)/`dllimport`(消费方),**无需额外改动** —— 这是复用 `ASIO_DECL`(而非自造 `RDMA_DECL`)的额外好处。
8. **热路径内联(见 §4 冷/热轴)**:`*_ops_verbs` 的 `post_*`/`poll_cq` 与 `fill_native_sge` 默认**不拆**以护内联;
   仅在 LTO 构建下才评估外部化。
9. **(未来,本期不做)extern-template**:对最常实例化的 op/service 实例,asio 提供 `ASIO_NO_EXTERN_TEMPLATE` 开关
   (standalone 未启用)。若日后实例化膨胀成为编译瓶颈,可作为进一步手段;本计划不涉及。

## 9. 决策记录(本轮拍板)

1. **asio 耦合**:接受"asio 一起 separate"。复用 `ASIO_DECL`,separate 模式下 asio 与 rdma 一同进入 separate。
2. **拆分范围**:**务实拆分** —— 只拆有实体的冷非模板;琐碎一行 inline + 热数据面 wrapper(`post_*`/`poll_cq`/`fill_native_sge`)保持 inline 留头(见 §4 冷/热轴)。
3. **构建开关**:**不加项目级 CMake 选项**,消费者驱动(asio 模型);两态由测试侧 source-config header 驱动,测试自带完整 CMake 脚本(见 §6)。
4. **LTO**:当前不启用 LTO → 热 wrapper 一律 inline 留头。
5. **合并 src + 库不 ship `.cpp`**:库交付物只有头伞 `rdma/impl/src.hpp`(内含 `asio/impl/src.hpp`)。**库本身不提供
   `src/rdma.cpp`/`src/asio.cpp`** —— 那个一行源 `.cpp` 由消费者/测试编写(对齐 asio)。消费者**只 include 一个**、
   只定义一个宏 `ASIO_SEPARATE_COMPILATION`(见 §3.4)。本仓库的 separate 验证 `.cpp` 放在**测试目录**(见 §6)。
6. **用法文档**:单独写 `docs/separate_compilation_usage.md`,含 **header-only/separate × default/no-default-linked 四种组合**
   的完整示例,并在 README 引用。
