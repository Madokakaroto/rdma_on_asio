# IBV CMake / Linking Plan

## Status

**决定:ibv 保持现状,不跟 nd 对齐。** 本文件记录这个决定及其依据。

与已落地的 `docs/nd_cmake_refactor_plan.md` 对比后,结论是:nd 的那套(native 构建模块
`NetworkDirect.cmake` + `nd_autolink.hpp` 源码级 autolink)**不应**照搬到 ibv —— 不是没做,
而是经探查确认对 ibv **不适用且无收益**。

## 决定

ibv 后端的库处理维持当前形态:

- 用 CMake 自带的 `pkg_check_modules` 找系统 rdma-core(libibverbs + librdmacm);
- 每个 target 链 `PkgConfig::IBVERBS` / `PkgConfig::RDMACM`;
- 尊重 `ASIO_NO_DEFAULT_LINKED_LIBS` 的语义(见下,对 ibv 基本是空操作)。

**不做**:不建 `ibv_autolink.hpp`,不建 `IbVerbs.cmake` 这类"准备产物"的模块。

## 现状(就是目标态)

root `CMakeLists.txt` 的 ibv 分支([CMakeLists.txt:46-51](../../CMakeLists.txt#L46-L51)):

```cmake
if(RDMA_BACKEND STREQUAL "ibv")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(IBVERBS REQUIRED IMPORTED_TARGET libibverbs)
  pkg_check_modules(RDMACM  REQUIRED IMPORTED_TARGET librdmacm)
endif()
```

各 target:

```cmake
target_link_libraries(<t> PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM)
```

ibv 后端是 header-only(无 `src/ibverbs.cpp` wrapper),系统头 `<infiniband/verbs.h>` /
`<rdma/rdma_cma.h>` 直接被 detail 头包含 —— **没有 vendored 头、没有 native 产物要构建**。

## 为什么不跟 nd 对齐(三条均为已验证事实)

1. **源码级 autolink 在 ibv 的主编译器(gcc)上不成立。** 本机实测:GCC 13.3 对
   `#pragma comment(lib, "ibverbs")` **直接忽略**,只产生 `-Wunknown-pragmas`,**零链接效果**。
   只有 Clang 会把它降级为 ELF dependent-libraries,且还需 lld 类链接器消费(GNU ld 一般不消费)。
   故给 ibv 写 autolink 头在 gcc 下是 no-op、在 clang 下是条需 `-fuse-ld=lld` 的脆弱路径。

2. **asio 自己在 POSIX 上就不做源码 autolink。** asio 的全部 autolink 被
   `ASIO_WINDOWS || __CYGWIN__` + `_MSC_VER || __BORLANDC__` 包住
   ([socket_types.hpp:47-56](../../third_party/asio/include/asio/detail/socket_types.hpp#L47-L56),
   另 [connect_pipe.ipp:30-34](../../third_party/asio/include/asio/impl/connect_pipe.ipp#L30-L34) 的
   `bcrypt.lib`);POSIX `#else` 分支**一条 pragma 都没有** —— Linux socket 在 libc,库链接交给
   构建系统。**"对齐 asio"在 Linux 上的正解,恰恰是"不在源码里 autolink"。** 注意 asio 的守卫里
   **没有 `__clang__`**,即 asio 从不信任 clang 的 dependent-libraries —— 我们若用它就是超出 asio。

3. **nd 与 ibv 的 cmake 模块本就该不同(find vs build)。** nd 是 vendored 源码:
   `rdma_networkdirect_init` 要 `mc.exe` 生成 `ndstatus.h` + `MSBuild` 编 `ndutil.vcxproj` 出
   `ndutil.lib`(**构建**)。ibv 是系统装好的 rdma-core:`pkg_check_modules` 只是**定位**
   (系统上确认无 `.pc` 之外的 NetworkDirect 安装物,也无 `FindNetworkDirect.cmake`/Config)。
   `find_package`/`pkg_check_modules` 只"找"不"编",天然套不到 nd 的 vendored-build 流程,反之
   nd 那套"准备产物 + autolink 私有 lib"对系统库的 ibv 也纯属多余。

## 关于 `ASIO_NO_DEFAULT_LINKED_LIBS`

这个宏对 **ibv/Linux 基本是空操作**:ibv 源码没有要 autolink 的东西(asio POSIX 侧也没有),
链接一律走 CMake/PkgConfig。因此:

- **不**需要为它造 `ibv_autolink.hpp`;
- 行为契约自然满足:无论是否定义该宏,ibv 的库都由构建系统(CMake)链 —— 这与 asio 在 POSIX 上
  "由构建系统链 pthread 等"完全一致。

(nd 那边 `nd_autolink.hpp` 在 `_MSC_VER` 下发 `#pragma comment(lib, "ndutil.lib")` 是 Windows/MSVC
专属、且服务 vendored native `ndutil.lib` 的特例,不构成 ibv 必须对齐的理由。)

## 唯一可选的小清理(纯去重,不是"对齐")

各 `tests/*/CMakeLists.txt` 里重复的
`target_link_libraries(<t> PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM)` 可以收敛进一个薄 helper,
例如 `rdma_configure_ibv_backend(<t>)`。这**只是减少重复**,与"跟 nd 对齐 / autolink"无关,做不做都行,
不影响本决定。

## 明确不做的事

- 不新增 `include/rdma/ibv/detail/ibv_autolink.hpp`。
- 不新增 `cmake/IbVerbs.cmake` 这类"准备产物"模块(现有 3 行 `pkg_check_modules` 足够)。
- 不在 ibv 源码里发任何 `#pragma comment(lib)`(gcc 会警告且无效)。
- 不为 ibv 复刻 nd 的 separate-compilation autolink/manual-link 宏测试矩阵(其 autolink 腿在 gcc 上
  本就无法通过)。

## 依据归档(便于以后复查)

- gcc 忽略 `#pragma comment(lib)`:本机 `gcc 13.3` 实测 `-Wunknown-pragmas`,无链接效果。
- asio autolink 仅 Windows+MSVC/Borland:
  [socket_types.hpp:47-56](../../third_party/asio/include/asio/detail/socket_types.hpp#L47-L56)、
  [connect_pipe.ipp:30-34](../../third_party/asio/include/asio/impl/connect_pipe.ipp#L30-L34)。
- nd 是 build-from-vendored-source:`cmake/NetworkDirect.cmake`(`find_program(mc.exe)` + 生成
  `ndstatus.h` + `MSBuild ndutil.vcxproj`),见 `docs/nd_cmake_refactor_plan.md`。
