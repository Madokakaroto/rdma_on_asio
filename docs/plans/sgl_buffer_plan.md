# 数据面 Scatter/Gather —— 迭代计划

> **状态:已实现并在 RoCE 上验证(Phase 1-4)。** 共享层 `asio::rdma::const_buffer`/`mutable_buffer` +
> `asio::rdma::buffer(...)` 工厂 + `mr.remote_addr()`;`buffers2sglist` 改 forward 迭代器;post 前 `num_sge`
> 校验返回 `rdma_errc::ext_too_many_sge`;`tests/rdma/test_rdma_sgl.cpp`(gather/scatter 多 MR + too-many-sge)
> 通过,全量回归绿。nd 镜像改动已就位(Windows 待验证)。

> 目标:完善 ibv 数据面 `async_send`/`async_recv`/`async_read`/`async_write` 的 scatter/gather(多段 SGL)
> 接口。跨平台契约保持统一(nd 镜像,Windows 端后验证)。
>
> **范围(已拆分)**:本计划只覆盖 **buffer/SGL 接口 + `rdma_memory_region` 接口调整**,用**自带
> `rdma_memory_region`**(bring-your-own 内存)即可完整使用,**不依赖**任何分配器/堆。
> "库帮你分配并注册内存"(注册堆 + registrar + STL/pmr allocator)是**另一个、建立在本计划值语义元素之上**的特性,
> 见 [`rdma_memory_management_plan.md`](rdma_memory_management_plan.md)。

## 0. 现状(已具备 vs. 缺口)

**底层已经能做多段 S/G** —— 不是从零开始:
- `ibv_buffer.hpp:11-26` 的 `buffers2sglist(bs, sglist)` **已经遍历整个 buffer 序列**,每个元素填一条 SGE
  (`addr/length/lkey`)。
- `ibv_service_verbs.hpp` 的 `do_post_{send,recv,read,write}` 用 `buffers2sglist` + `sglist.size()` 传
  `num_sge`,**不是写死 1**;`ibv_ops_verbs.hpp:137` `wr.num_sge = sge_count`。
- `ibv_impl_types.hpp:141-179` `ibv_sglist_t` 有 SBO(`inline_sge_count = 8`)→ 超出落堆。
- `rdma_remote_addr_t{addr_, token_}`(`rdma_commons.hpp:57-60`)承载 read/write 的远端 addr+rkey。

**真正的缺口:**

1. **没有可用于容器的 buffer 元素 —— S/G 的真正拦路虎。**
   `ibv_memory_region::const_buffer`/`mutable_buffer`(`ibv_mr.hpp:103-167`)持有
   `ibv_memory_region const& mr_`,且 `operator= = delete`(:118/:151)。
   后果:**不可赋值、无默认构造** → 放不进 `std::vector`/`std::array`/`initializer_list`。
   单个 `slice()` 凭 ADL 友元(`buffer_sequence_begin/end` 返回 `&one` / `&one+1`)能当"1 元素序列"用,
   但**无法把多段拼成一个序列**。这才是多段 S/G 用起来别扭的根因。

2. **遍历假定随机访问迭代器。** `buffers2sglist` 用 `begin + loop`(指针算术),forward-iterator 的序列
   (如 `std::list` 等)用不了。

3. **不校验 `num_sge` 上限。** 超过设备 `max_send_sge`/`max_recv_sge` 时,直接在 `ibv_post_*` 失败返回裸
   errno,而非一个干净的库级错误。

## 1. 关键设计决定(需 review 拍板)

### D1 —— SGL buffer 元素值语义化(只带 lkey)+ 全部下沉为跨平台共享代码(本计划的基石)

把 SGL 的元素从 `{ibv,nd}_memory_region::{const,mutable}_buffer`(嵌在 MR 内、持 `MR const&`)改为
**一个跨平台共享类型 `asio::rdma::const_buffer` / `asio::rdma::mutable_buffer`,按值持有 `{addr, length, lkey}`**:

- **跨平台共享,不分后端**:元素是纯 `{addr, length, lkey}`(lkey 只是 uint32,无任何后端类型),故
  `asio::rdma::const_buffer`/`mutable_buffer`、工厂 `asio::rdma::buffer(...)`、buffer-sequence concept
  **全部放共享层 `include/rdma/rdma_buffer.hpp`**(concept 已在此),ibv 与 nd 都**产出同一类型**,不再各自实现
  嵌套类。唯一保留后端特化的是 `buffers2sglist`(填各自的 native SGE,只读 `addr()/length()/local_key()`)。
- 去掉 MR 引用后,元素变成**可拷贝/可赋值/可默认构造** → 能放进
  `std::vector`/`std::array`/`initializer_list`。**这正是解锁真正 S/G 的关键。**
- **工厂 `asio::rdma::buffer(...)`(对标 `asio::buffer`,跨平台)**:`buffer(mr)` 覆盖整段、`buffer(mr, off, n)`
  取子段;由 MR 的 const 性选 `mutable_buffer`(非 const mr)/ `const_buffer`(const mr)。它是 MR 类型的模板,
  只触碰 MR 的可移植接口(`addr()/length()/local_key()`),故天然跨平台。MR 成员 `slice()/cslice()` 保留
  (返回同一共享类型),与 `buffer()` 等价,二者并存。
- **只带 lkey(已核实,Q-A)**:元素的唯一角色是"本地操作数 / 本地 SGE 描述符"。`struct ibv_sge` 只有
  `{addr, length, lkey}`(`verbs.h:1144`);`buffers2sglist`(`ibv_buffer.hpp`)对每个元素也只读
  `addr()/length()/local_key()`。`async_send`/`async_recv`,以及单边 `async_read`/`async_write` 的**本地**
  散列表,都只消费 lkey。
- **rkey / 广告是另一个角色,不进 buffer 元素**:read/write 的**远端目标** rkey 是 WR 里独立的
  `wr.wr.rdma.rkey`(`verbs.h:1167`;`ibv_ops_verbs.hpp:180`),来自对端广告的 `rdma_remote_addr_t`
  (**对端**那块内存),与本地 buffer 序列无关。"把自己内存广告给对端"交给 MR 级助手
  `ibv_memory_region::remote_addr(offset, length) -> rdma_remote_addr_t{base+offset, rkey}`。
- 保留:`addr()` / `length()` / `local_key()`、单元素的 `buffer_sequence_begin/end` 友元、`slice()`/`cslice()`。

代价(API 可见,小):凡是调 `buf.remote_key()` / `buf.get_mr()` 的地方改用 `mr.remote_addr(...)`。

**D1 倾向:采纳。** 不采纳则 S/G 容器始终别扭(只能靠用户自己造满足 `mr_adapted_buffer_sequence` 且元素
非值语义的序列,基本不可行)。**D1 也是 `rdma_memory_management_plan.md` 的前置依赖。**

> **内存来源与 `rdma_memory_region` 的定位**:某块缓冲的来源二选一 —— 自带内存→`rdma_memory_region`(本计划),
> 或从 RDMA 堆分配(另一计划)。`rdma_memory_region` 是低层基元,覆盖**任意**内存(栈/堆/mmap/GPU/第三方),
> 必须保留公开;两条来源最终都产出同一个值语义 buffer 元素。对比与 registrar 统一见
> `rdma_memory_management_plan.md` §1。

## 2. 分阶段实施

### Phase 1 —— buffer 元素值语义化 + 共享化(落 D1)

- **共享层 `include/rdma/rdma_buffer.hpp`**:新增跨平台 `asio::rdma::const_buffer` / `mutable_buffer`,持有
  `{void(* /const*) addr_; std::size_t length_; std::uint32_t lkey_;}`,值语义(默认构造=空、可拷贝/赋值);
  `mutable_buffer → const_buffer` 隐式转换;`addr()/data()`、`length()`、`local_key()`、`rdma_buffer_tag`、
  单元素 `buffer_sequence_begin/end` 友元。新增 `rdma_const_buffer`/`rdma_mutable_buffer` 别名。
- **工厂 `asio::rdma::buffer(...)`**(同文件,模板于 MR 类型):`buffer(MR&)`/`buffer(MR const&)`(整段)、
  `buffer(MR&, off, n)`/`buffer(MR const&, off, n)`(子段,区间校验);返回 mutable/const 由 MR const 性决定。
- `ibv_mr.hpp` / `nd_mr.hpp`:**删除嵌套的 `const_buffer`/`mutable_buffer`**(及其 `get_mr()`/`remote_key()`/
  `is_valid()`);`slice()`/`cslice()`(nd 还有 `slice(void*,len)`)改为返回共享类型,内部
  `{addr+offset, len, local_key()}`。
- `ibv_memory_region` / `nd_memory_region` 增 `rdma_remote_addr_t remote_addr(std::size_t offset,
  std::size_t length) const`(= `{base+offset, rkey}`,区间校验),承担"广告自己内存"的角色。
- `buffers2sglist`(ibv/nd)无需改动(只读 `addr()/length()/local_key()`),验证它接受新共享类型即可。
- 构建 + 全量 RoCE 套件(行为不应变化,回归基线);nd 镜像同步修改(Windows 后验证)。

伪代码:
```cpp
class const_buffer {
 public:
  using rdma_buffer_tag = detail::rdma_const_buffer_tag;
  const_buffer() = default;                                  // empty
  const_buffer(void const* addr, std::size_t len, std::uint32_t lkey)
      : addr_(addr), length_(len), lkey_(lkey) {}
  const_buffer(const_buffer const&) = default;
  const_buffer& operator=(const_buffer const&) = default;    // now assignable
  void const* addr() const noexcept { return addr_; }
  std::size_t length() const noexcept { return length_; }
  std::uint32_t local_key() const noexcept { return lkey_; }   // SGE 唯一需要的 key
  friend const_buffer const* buffer_sequence_begin(const_buffer const& o) noexcept { return &o; }
  friend const_buffer const* buffer_sequence_end(const_buffer const& o) noexcept { return &o + 1; }
 private:
  void const* addr_ = nullptr; std::size_t length_ = 0; std::uint32_t lkey_ = 0;
};
// MR 级广告助手(承担 rkey/远端广告角色,取代 buffer.remote_key()):
rdma_remote_addr_t remote_addr(std::size_t offset, std::size_t length) const;  // {base+offset, rkey}
```

### Phase 2 —— 多段 S/G 易用性(仿 asio buffer sequence,Q-B)

- 验证 Phase 1 后 `std::vector<rdma_const_buffer>` / `std::array<…, N>` 满足 `mr_adapted_buffer_sequence`
  (asio 的自由 `buffer_sequence_begin/end` 对标准容器生效,元素值语义后即可)。
- **多 MR S/G**(每段不同 MR、不同 lkey):每个元素已自带各自的 lkey,直接把不同 MR 的 `slice(...)` 结果
  push 进同一个标准容器即可 —— **仿照 asio 的 buffer sequence,不引入 bespoke `rdma_buffer_list`**(Q-B)。
- post 前校验 `count ≤ effective_config.max_{send,recv}_sge`,超限返回干净错误(`ext_*` /
  `invalid_argument`),而非 HW EINVAL(Q-C)。

### Phase 3 —— 迭代与 concept 健壮性

- `buffers2sglist`:改用 `++it` / `std::next`(支持 forward 迭代器),不再 `begin + loop`。
- 厘清两个序列 concept:`mr_buffer_sequence`(成员 `cbegin/cend`)vs `mr_adapted_buffer_sequence`
  (ADL `buffer_sequence_begin/end`)。op 约束统一采用 asio 风格的 ADL 版;确保**单 buffer 与 std 容器**两者都满足。

### Phase 4 —— 测试 / 文档 / nd 对齐

- `tests/ibv/test_ibv_sgl.cpp`(RoCE):
  - gather-send:N 段(不同 buffer)发出 → 对端单段连续 recv 收齐;scatter-recv:单段发 → N 段散收。
  - 多 MR S/G:各段不同 MR/lkey。
  - 超 `num_sge` 上限被干净拒绝。
- `tests/rdma/` 跨平台版(仅 `rdma_*` 别名)。
- README 数据面小节 + `rdma_buffer.hpp` concept 文档更新。
- **nd 对齐**(镜像,Windows 后验证):`nd_mr` 的元素同样值语义(承载 nd 的 lkey/token);`nd_sglist_t` 已存在。
  契约取交集:S/G 段数上限两后端一致。

## 3. 已定的设计点(原开放问题)

- **Q-A(buffer 元素携带哪些 key)——已定**:元素**只携带 lkey**(见 D1)。已核实 SGE(`struct ibv_sge`)
  与 `buffers2sglist` 都只消费 `{addr, length, lkey}`,故 buffer 元素作为"本地操作数 / SGE 描述符"只需 lkey。
  rkey 是"对端访问你这块内存"的另一角色:read/write 远端目标的 rkey 是 WR 独立字段、来自对端广告的
  `rdma_remote_addr_t`;广告自己内存用 MR 级 `mr.remote_addr(offset,len)`。lkey/rkey 是"本地访问 vs 对端访问"
  之分,不是"双边 vs 单边"之分。
- **Q-B(多 MR 列表类型)——已定**:**仿照 asio buffer sequence**。每个值语义元素自带 lkey,多段(含多 MR)
  直接用满足序列 concept 的标准容器(`std::vector<rdma_const_buffer>` / `std::array<…, N>`),**不引入** bespoke
  `rdma_buffer_list`。
- **Q-C(段上限校验位置)——已定**:`num_sge > max_{send,recv}_sge` 在 **post 前由 service 校验**返回干净错误码
  (与"控制面早退拒绝"风格一致),不交给 HW EINVAL。

## 4. 影响面与风险

- **API 破坏**:Phase 1 的 `buf.remote_key()` / `buf.get_mr()` 迁移到 `mr.remote_addr(...)` 是唯一对外可见的
  破坏,影响面小(grep 可定位);`slice/cslice/local_key/async_*` 签名不变,现有单 buffer 用法零改动。
- **性能**:值语义元素小且可平凡拷贝(`{ptr,len,lkey}`,约 20B);构建 SGE 直接取这三字段;
  `thread_local` scratch sglist 不变。
- **跨平台**:Phase 1 的元素需在 nd 镜像;SGL 下层(nd_sglist_t / post)已就绪。
