# RDMA 内存管理(注册堆 + allocator)—— 迭代计划

> **依赖** `sgl_buffer_plan.md` 的 **D1 值语义 buffer 元素 + `rdma_buffer_view`(段序列)**。
> 本计划是其上的**可选大特性**:提供"分配即注册"的内存来源(registered heap),并在其上薄封装 STL/pmr allocator。
> SGL / `asio::buffer` 适配 / `rdma_memory_region` 接口调整在 `sgl_buffer_plan.md`,与本计划**正交**:
> 自带 `rdma_memory_region` 就能完整用 S/G,本计划只是多一条"库帮你分配并注册"的来源。

## 1. 定位:与 `rdma_memory_region` 互补,统一于 registrar

某块缓冲的来源**二选一**:① 自带内存 → `rdma_memory_region` 注册(`sgl_buffer_plan.md`);② 从 RDMA 堆分配(本计划)。
二者不是"库里只能留一个",而是**独立互补**,且在实现上统一于 `mr_registrar`(§3)。

| 维度 | MR-direct(`mr.slice`/`mr.adapt`) | RDMA 堆 / allocator(`rdma::buffer(container)`) |
|---|---|---|
| 覆盖的内存 | **任意**:栈、堆、mmap、第三方/文件映射、GPU(GPUDirect)、已注册区域 | **仅**用该 allocator 分配的容器 |
| 注册时机 | 用户显式 `ibv_reg_mr`(MR 生命周期自管) | 容器构造即注册(arena 惰性提交) |
| 扩容/realloc | vector 扩容→存储失效→**需重新注册**(易踩坑) | arena 内再分配;旧 tile 不动、lkey 稳定;只新增 tile |
| lkey/rkey 取得 | 每个 buffer 配一个 MR 显式取 | 随容器走(经 heap 的 registrar 按地址解析) |
| 易用性 | 低(每段手动配 MR) | **高**("RDMA-able 容器" + `rdma::buffer(v)` 即用) |
| 透明度/可控 | **高**(无隐藏注册、无全局状态) | 中(arena 预留、容量上限) |
| 与 STL/pmr 集成 | 无 | **有**(标准 Allocator / `pmr::memory_resource`) |

**`rdma_memory_region` 必须保留公开。** allocator 只能注册**它自己分配**的内存;栈缓冲、第三方/框架内存、GPU 显存、
`mmap` 文件、既有大缓冲这些 allocator 够不着 —— 只能走 `rdma_memory_region(dev, ptr, len)` 注册既有内存(零拷贝是
RDMA 核心用法)。加之 MR 是 arena 的底层、且提供注册时机/access flags/生命周期的精细控制。**分层定位**:MR = 低层
基元(任意内存、全控制);堆 + `rdma::buffer` = 高层 ergonomic;文档主推后者,但 MR 不隐藏、不弃用。

**统一点**:两种来源都只是往同一个 `mr_registrar` 里 `add_region`,再由它做 地址→lkey 解析 + SGE 切段。
MR-direct = "registrar 注册一个外部 span"(`rdma_memory_region` 即单-region 门面);heap = "registrar 注册自分配的
tile"。两条路线**最终产出同一个值语义 buffer 元素 `{addr,len,lkey}`**,`async_*` 不感知来源。

## 2. 设计决定 D3:组合式 RDMA 内存分配器 = heap ⊕ registrar

不把"注册"硬塞进 STL allocator(会被 `allocator<T>` 的按类型/rebind/realloc-churn 语义绊住),而是**分层组合**:

```
A. mr_registrar    —— 注册权威:把"span -> MR"注册起来,维护 地址 -> {lkey,rkey} 查找,
                      把 [ptr,len) 按 MR/tile 边界切成 SGE 段。泛化 rdma_memory_region。
B. tiled_va_arena  —— RDMA 无关的 heap:连续 VA 窗口预留 + 固定 tile 惰性提交 + 子分配(bump/pool);
                      发 on_commit / on_release(tile) 钩子。
C. rdma_heap       —— A ⊕ B:把 arena.on_commit 接到 registrar.add_region(tile)。提供
                      allocate/deallocate + buffer(ptr,len)/remote_buffer(ptr,len)(转交 registrar 切段)。
D. rdma_memory_resource / rdma_allocator<T> —— 在 C 之上的薄 pmr/STL 适配。
```

**返回什么(结合 RDMA 角色):** 本地 SGL → 值语义**段序列**(`rdma_*_buffer {addr,len,lkey}`,单 tile 1 段、跨 tile
多段);远端广告 → `remote_buffer` 切成 `rdma_remote_addr_t {addr,rkey}` 段。lkey/rkey 两条路径,本地只读 lkey。

## 3. 组件分解与组合

### A. `mr_registrar` —— 注册权威 + 地址解析(泛化 `rdma_memory_region`)
```cpp
class mr_registrar {
 public:
  mr_registrar(rdma_device_ptr dev, mr_access_flags access);
  region_id add_region(void* addr, std::size_t len);     // ibv_reg_mr + 记录区间/lkey/rkey
  void      remove_region(region_id);                    // ibv_dereg_mr
  std::uint32_t lkey_of(void const* p) const;            // 命中区间
  rdma_buffer_view  local_segments (void const* p, std::size_t n) const;  // {addr,len,lkey} 段
  rdma_remote_view  remote_segments(void const* p, std::size_t n) const;  // {addr,rkey}    段
  // ~: dereg 全部。解析:固定 tile -> O(1)(index 移位);任意 span -> 区间树 O(log)。
};
```
`rdma_memory_region` = 单 region 的 registrar 门面(`slice/cslice/adapt` 在其上)。

### B. `tiled_va_arena` —— RDMA 无关的 heap(机制见 §4 / §6)
```cpp
class tiled_va_arena {
 public:
  tiled_va_arena(std::size_t reserve_bytes, std::size_t tile_bytes);
  void* allocate(std::size_t n, std::size_t align);      // 紧凑打包,可跨 tile;按需提交 tile
  void  deallocate(void* p, std::size_t n);
  std::function<void(void* tile_addr, std::size_t tile_len)> on_commit;   // 新 tile 提交 -> 注册
  std::function<void(void* tile_addr, std::size_t tile_len)> on_release;  // tile 释放 -> dereg
  void* base() const noexcept; std::size_t tile_bytes() const noexcept;
};
```

### C. `rdma_heap` —— A ⊕ B(组合就是一个钩子)
```cpp
class rdma_heap {
  tiled_va_arena arena_;  mr_registrar reg_;
 public:
  rdma_heap(rdma_device_ptr dev, std::size_t reserve, std::size_t tile, mr_access_flags access)
      : arena_(reserve, tile), reg_(dev, access) {
    arena_.on_commit  = [this](void* a, std::size_t n){ reg_.add_region(a, n); };
    arena_.on_release = [this](void* a, std::size_t n){ reg_.remove_region_at(a); };
  }
  void* allocate(std::size_t n, std::size_t al){ return arena_.allocate(n, al); }
  void  deallocate(void* p, std::size_t n){ arena_.deallocate(p, n); }
  rdma_buffer_view buffer        (void const* p, std::size_t n) const { return reg_.local_segments(p, n); }
  rdma_remote_view remote_buffer (void const* p, std::size_t n) const { return reg_.remote_segments(p, n); }
};
```

### D. `rdma_memory_resource` / `rdma_allocator<T>` —— 薄 pmr/STL 适配
```cpp
class rdma_memory_resource : public std::pmr::memory_resource {
  rdma_heap* heap_;
  void* do_allocate(std::size_t b, std::size_t a) override { return heap_->allocate(b, a); }
  void  do_deallocate(void* p, std::size_t b, std::size_t) override { heap_->deallocate(p, b); }
  bool  do_is_equal(memory_resource const& o) const noexcept override { return this == &o; }
 public:
  explicit rdma_memory_resource(rdma_heap& h) : heap_(&h) {}
  rdma_heap& heap() const noexcept { return *heap_; }
};
// std::pmr::vector<char> v(&res);  (typed rdma_allocator<T> 同理薄封装 heap)
```

### `rdma::buffer` 分派(一律经 registrar 解析)
```cpp
// pmr/STL 容器(其 resource/allocator 是 rdma 的)-> 经其 heap 的 registrar 切段
template <PmrOrRdmaContainer C> auto buffer(C& c)        { return rdma_heap_of(c).buffer(c.data(), bytes(c)); }
template <PmrOrRdmaContainer C> auto remote_buffer(C& c) { return rdma_heap_of(c).remote_buffer(c.data(), bytes(c)); }
// 裸指针 / 外部内存:显式给 registrar(或 heap)
rdma_buffer_view buffer(mr_registrar const& r, void const* p, std::size_t n) { return r.local_segments(p, n); }
// MR-direct:外部 span 先 r.add_region(span),再 buffer(r, ptr, n)。
```
段序列(常见 1 段、跨 tile 多段)直接进 `buffers2sglist`;段数 > `max_sge` 干净报错。

### 落地路径(按层、各层独立可测)
1. **(前置,见 sgl_buffer_plan)** D1 值语义 buffer 元素 + `rdma_buffer_view`(段序列)。
2. **`mr_registrar`**:`add/remove_region` + `lkey_of` + `local/remote_segments`;`rdma_memory_region` 改为单-region 门面。
   测:注册既有 buffer、解析、多 region 切段。
3. **`tiled_va_arena`**(**无设备**):reserve/commit/sub-alloc + on_commit/on_release。测:分配模式、tile 提交计数、增长、free。
4. **`rdma_heap`** = arena ⊕ registrar(钩子接线)。测(RoCE):`allocate → rdma::buffer → async_send`;跨 tile 多段;销毁顺序。
5. **pmr/STL 适配** + `rdma::buffer` 容器分派。测:`std::pmr::vector<char>` → send。
6. 测试 / 文档 / nd 对齐(`mr_registrar` 用 ND register-memory→token;`tiled_va_arena` 与平台无关可直接复用)。

## 4. arena 模型与扩容(最终共识)

> 本节是 §3 中 **B 层 `tiled_va_arena`(heap)** 的内部机制。除"惰性提交 tile 时调 `on_commit` 钩子"这一接缝外,
> 其余与 RDMA 无关;registrar 在钩子里完成 `ibv_reg_mr`。

### 背景事实(拍板依据)

- **`ibv_reg_mr` 代价大且 ∝ 区域大小**:逐页 pin(类 `mlock`)+ 给 NIC 编程地址翻译表(MPT/MTT);大区域可达毫秒级
  (见 §5 实测)。→ 原则:**少注册、注册大块、惰性注册**,绝不每次 allocate 都注册。
- **lkey 是 per-MR 的快照**:`ibv_reg_mr` 那一刻把 VA→物理 快照进 NIC 翻译表;**非 ODP 时此表不随后续页表变化更新**。
  → 注册后再 `mmap(MAP_FIXED)`/惰性提交/迁移页,NIC 看不见 → 不能用 VM 技巧伪造"单 lkey 还能惰性增长"。
- **"单 lkey ⟺ 注册时整块 pin(非 ODP)"**:要单 lkey 就得一次注册覆盖整段(当场全 pin、无惰性);要惰性/增量 →
  必然多次注册 → **多 lkey**。两者不可兼得,除非 **ODP**(NIC 接进 MMU-notifier,缺页按需 pin,单 lkey+增长,代价是
  缺页延迟 + 设备支持)。VM 预留只能给"连续 VA",给不了"单 lkey"。
- **没有异步 `ibv_reg_mr`**:`ibv_reg_mr`/`_iova2`/`_dmabuf`/`rereg` 全同步阻塞。mlx5 的 UMR/Fast-Reg
  (`mlx5dv_wr_mkey_configure`,经 SQ + CQ 异步)只**重映射已 pin 的内存**,不做 fault+pin,绕不开 pin 成本。要让 pin 不
  阻塞热路径:**ODP**(便宜注册 + 缺页 pin)或**后台线程预注册下一个 tile**。

### 模型:连续 VA 窗口 + 固定 tile(惰性注册)+ 紧凑打包 + 运行时按 tile 切 SGE

1. **连续 VA 窗口**:`mmap(reserve_bytes, PROT_NONE, MAP_NORESERVE)` 预留(几乎零成本、不提交物理页)。arena 的 VA
   连续且永不搬动。
2. **固定 tile + 惰性注册**:窗口切成固定 `tile_bytes`;增长时对下一段 `mmap(MAP_FIXED, RW)` 提交 + `ibv_reg_mr` →
   新 lkey/rkey。**旧 tile 原封不动、不重注册** → 已分配块的地址、lkey、在途 op 全程稳定。
3. **紧凑打包,允许跨 tile**:分配在窗口内顺序打包,不躲 tile 边界 → **无边界碎片**,单次分配可任意大(≤ `max_sge × tile_bytes`)。
4. **`rdma::buffer` 运行时按 tile 切 SGE**:`[P, P+N)` 跨越的 tile 数 = SGE 段数;`tile_index = (P-base)/tile_bytes`、
   `lkey(i)` 查表,**O(1) per 段**。单 tile 内 → 1 段;跨边界 → 多段。
5. **段数上限 = 设备 `max_send_sge`/`max_recv_sge`(mlx5 = 30)**:跨越 tile 数必须 ≤ 它,否则一个 WR 发不下 → 干净报错。
6. **tile 大小是关键旋钮**(选型见 §5):`跨越 tile 数 ≈ 缓冲大小 / tile_bytes`。mlx5 `max_mr_size` 近无限 → tile 可设很大
   → 绝大多数容器落单 tile(1 段),仅恰好跨界的 2 段。
7. **子分配 + vector churn**:`std::vector` 扩容 = 新块 + 拷贝 + 释放旧块。bump 浪费约 2× 终值;**pool/free-list** 可复用
   (见 Q-F)。建议:① 子分配用 pool;② 文档建议对 RDMA 容器 **`reserve()` 一次到位**。
8. **设备上限 + 干净报错**:`RLIMIT_MEMLOCK` / `max_mr` 封顶;`ibv_reg_mr` 失败 → 干净 `bad_alloc` / `ext_*`。
9. **可选 ODP 模式**:单个 implicit/超大 ODP MR(`IBV_ACCESS_ON_DEMAND`)→ **单 lkey、缺页按需 pin、零重注册**,
   `rdma::buffer` 恒 1 段;代价缺页延迟 + 设备支持。

### 不要做 / 边界

- **绝不**"dereg + realloc 更大 + 重注册"来在线扩容:会搬走内存(容器 `data()` 全失效——allocator 不能移动已交出的块)、
  换 lkey、让在途 WR 踩空。该模式**仅当 arena 完全空闲(无活跃分配、无在途 op)**时可作为显式 `reset(new_size)` 用。
- **`n > max_mr_size`**(mlx5 几乎不可能):单块装不进一个 MR → 默认干净失败;进阶可用多相邻 MR 覆盖 + 多 SGE(受
  `max_sge` 限,边缘情形)。
- **在途契约**:WR 在途时不得 realloc/改动其覆盖的容器 —— 在途 send 引用旧块,vector 扩容已把旧块还给 arena → UAF。
  与"send 一个 buffer 后别动它直到完成"是同一条 RDMA 契约。

## 5. registration 成本实测与 tile 选型

实测 mlx5_0(RoCE),`ibv_reg_mr` 时间(fresh = 页未驻留,注册时 fault+pin;warm = 页已驻留):

| size | reg fresh | reg warm | dereg |
|---|---|---|---|
| 4–256 KB | 0.03–0.10 ms | ~0.03 ms | ~0.02 ms |
| 1 MB | 0.24 ms | 0.05 ms | 0.04 ms |
| 4 MB | 0.77 ms | 0.10 ms | 0.07 ms |
| 16 MB | 3.9 ms | 0.47 ms | 0.22 ms |
| 64 MB | 16.6 ms | 1.7 ms | 0.78 ms |
| 256 MB | 66.9 ms | 4.9 ms | 2.7 ms |
| 1 GB | 277 ms | 17.6 ms | 10.3 ms |
| 4 GB | 1128 ms | 67.7 ms | 41.6 ms |

设备/限制:`max_mr_size = 2^64−1`(实际无限)、`max_mr ≈ 2^24`、`max_sge = 30`、`RLIMIT_MEMLOCK ≈ 11.5 GB`、RAM 96 GB。

**结论:**
- fresh 注册成本由 **fault+pin 主导,~线性(≈4 GB/s,~250 ms/GB)**;warm 快 ~16×(≈15 GB/s)。→ 不能预先注册巨窗口,
  惰性 per-tile 注册是对的。
- 小注册近乎免费(≤256KB,固定开销 ~25–50µs/次);tiny tile 会让 SGE 段数与注册次数双爆。
- **per-tile 提交 stall = 该 tile 的 fresh 注册成本**(inline 在触发它的 `allocate` 上):1MB→0.24ms、4MB→0.77ms、
  16MB→3.9ms、64MB→16.6ms、256MB→67ms。→ tile 不宜过大(延迟尖峰)。
- 约束:`tile_bytes ≥ max_single_buffer / 30`(max_sge)。

**推荐默认 `tile_bytes = 2 MB`(一个 x86 huge page)**:提交 stall ≈ 0.4ms;单 buffer 可达 ~60MB 单 WR(30×2MB);
KB–MB 缓冲落单 tile = 1 段;huge-page 对齐 → 可选 `MAP_HUGETLB` 进一步省 MTT、加快注册。大传输工作负载可调到 16–64MB。

## 6. 实现细节:VA 增长 + 多 MR 注册

> 机制写在一个类里便于通读;按 §3 分层,职责拆给两层:**`tiled_va_arena`(B)** 持 `base_/reserve/tile_bytes/frontier_/mtx_`,
> 做 `mmap` 预留、`mprotect` 提交、子分配,提交新 tile 时调 `on_commit`;**`mr_registrar`(A)** 在钩子里 `ibv_reg_mr` 并持
> `mrs_/lkeys_/rkeys_` 与解析。即下文 `ensure_committed` 的"(1) mprotect"属 arena、"(2) ibv_reg_mr + 写 lkeys_"属 registrar。

### 数据结构
```cpp
class rdma_memory_arena {                          // 示意:合一便于通读(实际拆 arena/registrar 两层)
  rdma_device_ptr dev_;
  void*           base_         = nullptr;          // 预留窗口起点(mmap 返回)
  std::size_t     reserve_bytes_= 0;               // 窗口总大小(= 硬上限,tile 对齐)
  std::size_t     tile_bytes_   = 0;               // 2 的幂,>= 页大小(便于 index 移位)
  unsigned        tile_shift_   = 0;               // log2(tile_bytes_)
  std::size_t     tile_count_   = 0;               // reserve_bytes_ / tile_bytes_
  std::vector<ibv_mr*>               mrs_;          // nullptr = 未提交/未注册
  std::vector<std::atomic<uint32_t>> lkeys_;        // 0 = 未注册;提交时 store
  std::vector<std::atomic<uint32_t>> rkeys_;
  std::size_t     frontier_     = 0;               // bump:已用到的偏移
  std::mutex      mtx_;
  mr_access_flags access_;
};
```
`mrs_/lkeys_/rkeys_` 构造时**预 `resize(tile_count_)`**(TB 窗口 + 256MB tile 也才几千项),之后**永不 realloc** →
读侧(`lkey(i)`)无需锁、无指针失效。

### 构造:预留连续 VA(不提交)
```cpp
base_ = ::mmap(nullptr, reserve_bytes_, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
// PROT_NONE + NORESERVE:只占地址空间,零物理页、零 swap。64-bit 上 VA 廉价,reserve 可设到 TB 级。
// reserve_bytes_ 是硬上限:VA 窗口一旦定了不能就地连续扩。需要更大就得第二个窗口(非连续 -> 退回区间查找)。
```

### 增长:惰性"提交 + 注册" tile(在 `allocate` 内按需触发)
```cpp
void ensure_committed(std::size_t a, std::size_t b) {            // 偏移区间, 持 mtx_
  for (std::size_t i = a >> tile_shift_; i <= (b - 1) >> tile_shift_; ++i) {
    if (mrs_[i]) continue;                                       // 已就绪
    void* t = static_cast<char*>(base_) + (i << tile_shift_);
    ::mprotect(t, tile_bytes_, PROT_READ | PROT_WRITE);          // (1) 提交[arena]
    ibv_mr* mr = ::ibv_reg_mr(dev_->pd(), t, tile_bytes_, to_ibv_access(access_));  // (2) 注册[registrar]
    if (!mr) { throw_bad_alloc_or_ext(errno); }
    mrs_[i] = mr;
    rkeys_[i].store(mr->rkey, std::memory_order_release);
    lkeys_[i].store(mr->lkey, std::memory_order_release);        // 最后 publish lkey
  }
}
```
要点:`ibv_reg_mr` 注册大窗口的**子区间**完全合法;**注册即 pin 整个 tile** → tile 大小同时是 pin 粒度;
**旧 tile 永不动**(只对 `mrs_[i]==nullptr` 干活)。

### allocate / 解析 / 销毁
```cpp
void* allocate(std::size_t n, std::size_t align) {              // 紧凑打包,可跨 tile
  std::lock_guard lk(mtx_);
  std::size_t off = align_up(frontier_, align);
  if (off + n > reserve_bytes_) throw std::bad_alloc{};         // 窗口用尽(硬上限)
  ensure_committed(off, off + n);
  frontier_ = off + n;
  return static_cast<char*>(base_) + off;
}
std::size_t tile_index(void const* p) const noexcept { return (static_cast<char const*>(p) - (char const*)base_) >> tile_shift_; }
std::uint32_t lkey(std::size_t i) const noexcept { return lkeys_[i].load(std::memory_order_acquire); }  // 无锁
~rdma_memory_arena() { for (auto* mr : mrs_) if (mr) ::ibv_dereg_mr(mr); if (base_) ::munmap(base_, reserve_bytes_); }
```
- **deallocate**:bump 版 no-op(整 arena 生命周期回收);pool 版归还 free-list(Q-F)。
- **跨线程读 `lkey(i)`** 无锁:buffer 指针从分配线程传到使用线程本就需同步,顺带 publish `lkeys_[i]`(atomic acq/rel)。
- **销毁顺序**:registrar 先 `ibv_dereg_mr` 所有 MR,arena 后 `munmap`;前置契约 arena outlive 所有在途 WR。
- **并发**:`allocate`/`ensure_committed` 持 `mtx_`(注册罕见);热路径小分配可加 per-thread free-list。
- **huge pages(可选)**:`tile_bytes_` 对齐 huge page、`MAP_HUGETLB`/THP → 更少页 pin、更小 MTT、更快注册。

## 7. 边界澄清:与 asio 的 allocator 正交

asio 有 **associated allocator** 协议(`get_associated_allocator` / `bind_allocator`)+ 具体的
`asio::recycling_allocator<T>`,但它管的是**异步操作对象(handler/op)的簿记内存**,**不是 I/O 数据缓冲**。我们的 op
(`op::ptr::allocate(handler)`)已走这条 → op 簿记的分配 asio 已处理。本计划的 `rdma_allocator`/注册堆管的是**注册数据缓冲**
(带 lkey/rkey),与之**正交**,不要混淆。`recycling_allocator` 的线程本地回收思路可作 arena 子分配(pool)的参考。

## 8. 开放问题(需 review 拍板)

- **Q-E(arena 形态 / 扩容)**:**已定** —— 连续 VA 窗口 + 固定 tile 惰性注册 + 紧凑打包(可跨 tile)+ `rdma::buffer`
  按 tile 运行时切 SGE。待拍板**默认值**:`tile_bytes`(§5 建议 2MB)、`reserve_bytes`、是否默认开 ODP。
- **Q-F(子分配策略)**:bump(只增不回收,配合容器整体生命周期)还是 pool(支持 deallocate 复用)?倾向:先 bump,pool 可选。
- **Q-G(`rdma::buffer` 命名/入口)**:`rdma::buffer` + `rdma::remote_buffer`(与 asio 对称),文档强调它做来源分派、不是 `asio::buffer`。
- **Q-H(access flags 默认)**:arena 注册默认带哪些访问位?倾向:构造参数显式指定,默认 local-only,需被对端 RDMA 时显式开 `REMOTE_*`。
