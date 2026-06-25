# rdma_on_asio -- 编译模式与链接用法(消费者指南)

> 面向**使用 rdma_on_asio 的工程**。模型完全对齐 asio:库是 header-first,**是否分离编译由你(消费者)决定**。
> 两条正交的轴:
> - **A. header-only vs separate compilation** —— 由 `ASIO_SEPARATE_COMPILATION` 控制(对所有 TU,要么都定义、要么都不定义)。
> - **B. default-linked-libs vs 手动链接** —— 由 `ASIO_NO_DEFAULT_LINKED_LIBS` 控制(是否让库在源码里自动 `#pragma comment(lib,...)`)。
>
> 设计与拆分细节见 [`plans/separate_compilation_plan.md`](plans/separate_compilation_plan.md);各后端的链接细节见
> [`plans/ibv_cmake_refactor_plan.md`](plans/ibv_cmake_refactor_plan.md) / [`plans/nd_cmake_refactor_plan.md`](plans/nd_cmake_refactor_plan.md)。
>
> **状态**:header-only(A=off)是当前默认、即开即用;separate compilation(A=on)随
> `plans/separate_compilation_plan.md` 落地。

## TL;DR(最常用:默认 header-only)

```cpp
#include "rdma/rdma.hpp"   // 直接用,无需任何宏、无需 src TU
```
Linux/ibv 链接 `ibverbs` + `rdmacm`(CMake:`PkgConfig::IBVERBS PkgConfig::RDMACM`);Windows/nd 见 `plans/nd_cmake_refactor_plan.md`。

## 关键不变量

1. **全程序单一模式**:`ASIO_SEPARATE_COMPILATION` 要么给**所有 TU**定义,要么都不定义。混用 → ODR 冲突 / 重复或未定义符号。
2. **separate 模式只需一个 include**:`rdma/impl/src.hpp` **已内含 `asio/impl/src.hpp`** —— 在**一个** `.cpp` 里 include 它即可,
   asio + rdma 两套实现都被编译。**不要**再单独建 asio 的源 TU,也**不要**自己 `#define ASIO_SOURCE`。

## 四种组合

| # | A: 编译模式 | B: 链接 | 定义的宏(所有 TU) | 唯一源 TU | 何时用 |
|---|-------------|---------|---------------------|-----------|--------|
| 1 | header-only | 默认链接 | (无) | (无) | **默认**,最省事 |
| 2 | header-only | 手动链接 | `ASIO_NO_DEFAULT_LINKED_LIBS` | (无) | 想自己掌控系统库链接(常见于 Windows 受控构建) |
| 3 | separate    | 默认链接 | `ASIO_SEPARATE_COMPILATION` | `#include <rdma/impl/src.hpp>` | 想减少多 TU 重复编译非模板实现,又要 autolink |
| 4 | separate    | 手动链接 | `ASIO_SEPARATE_COMPILATION` + `ASIO_NO_DEFAULT_LINKED_LIBS` | `#include <rdma/impl/src.hpp>` | separate + 完全手动链接(CI/打包受控) |

> **平台差异(重要)**:轴 B 的"default vs 手动"主要影响 **Windows**(asio 的 `ws2_32/mswsock/bcrypt` autolink,
> 以及 nd 的 `ndutil.lib` autolink)。**Linux/ibv 没有源码级 autolink**(gcc 不支持 `#pragma comment(lib)`),所以
> `ibverbs/rdmacm` **总是**由你的构建链接,组合 1↔2、3↔4 在 Linux 上**链接行为相同**(B 仅影响 asio 的 Windows 库,Linux 上 asio 不 autolink)。详见 `plans/ibv_cmake_refactor_plan.md`。

### 例 1 — header-only + 默认链接(默认)
```cpp
// 任意 .cpp
#include "rdma/rdma.hpp"
```
```cmake
target_link_libraries(app PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM)   # Linux/ibv
# Windows/nd: 见 nd_cmake_refactor_plan.md(asio 自动链 ws2_32 等;ndutil 按该计划)
```

### 例 2 — header-only + 手动链接
```cmake
target_compile_definitions(app PRIVATE ASIO_NO_DEFAULT_LINKED_LIBS)
# Linux/ibv: 与例 1 链接相同(本就手动链 ibverbs/rdmacm)
target_link_libraries(app PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM)
# Windows: 自行链 ws2_32 mswsock bcrypt (+ 按需 ndutil)
```

### 例 3 — separate compilation + 默认链接
```cpp
// src_rdma.cpp —— 整个程序里【唯一】一个这样的 TU
#include <rdma/impl/src.hpp>     // 同时编译 asio + rdma 的非模板实现
```
```cmake
# 给【所有】TU 定义宏(包括上面的 src_rdma.cpp 和你的业务 TU)
add_library(app_obj OBJECT main.cpp ... src_rdma.cpp)
target_compile_definitions(app_obj PRIVATE ASIO_SEPARATE_COMPILATION)
target_link_libraries(app PRIVATE app_obj PkgConfig::IBVERBS PkgConfig::RDMACM)  # Linux/ibv
# Windows/nd: ws2_32 等 + ndutil 由 autolink 解决(separate + 默认链接)
```

### 例 4 — separate compilation + 手动链接
```cpp
// src_rdma.cpp —— 唯一源 TU
#include <rdma/impl/src.hpp>
```
```cmake
target_compile_definitions(app_obj PRIVATE
    ASIO_SEPARATE_COMPILATION ASIO_NO_DEFAULT_LINKED_LIBS)   # 所有 TU
# 全部手动链接:
#   Linux/ibv: PkgConfig::IBVERBS PkgConfig::RDMACM
#   Windows/nd: ws2_32 mswsock bcrypt + ndutil.lib
target_link_libraries(app PRIVATE app_obj PkgConfig::IBVERBS PkgConfig::RDMACM)
```

## 常见错误

- **只给部分 TU 定义 `ASIO_SEPARATE_COMPILATION`** → 链接期重复/未定义符号。务必项目级统一定义。
- **建了两个源 TU(asio 一个、rdma 一个)** → 不需要;`rdma/impl/src.hpp` 已带入 asio 实现,只留一个 TU。
- **自己 `#define ASIO_SOURCE`** → 与 asio 的 `src.hpp` 重定义;交给 `rdma/impl/src.hpp` 处理。
- **在 Linux 上期待 `ASIO_NO_DEFAULT_LINKED_LIBS` 改变 ibverbs/rdmacm 链接** → 不会;Linux 永远由构建显式链接。
