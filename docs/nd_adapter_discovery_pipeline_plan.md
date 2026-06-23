# ND Adapter Discovery Pipeline Refactor Plan

## Goal

Refactor `include/rdma/nd/detail/nd_device_impl.hpp::open_adapters` without changing public APIs or the ibv backend.

The discovery model is AdapterId-first:

```text
providers
  -> provider address seeds
  -> ResolveAddress(address) = AdapterId
  -> per provider, sort/chunk by AdapterId
  -> OpenAdapter(AdapterId) once
  -> attach final local addresses
  -> provider.devices_
```

`IND2Provider::QueryAddressList` remains only the bootstrap seed because ND2 has no direct "enumerate AdapterId" API.

## Decisions

- Keep this as an ND-internal implementation refactor.
- Do not change `get_first_available_device`, `use_device`, connector/listener APIs, or ibv code.
- Prefer a provider-scoped pipeline in production code. This avoids sorting by provider pointer and avoids regrouping devices by provider at the end.
- Keep small range helpers local to `nd_device_impl.hpp` in `asio::rdma::detail`; do not add a separate helper header.
- Keep helpers generic: `to<std::vector>()`, `filter_map`, `sort_by`, and eager `chunk_by`.
- Do not add business-shaped DSL such as `transform_group_values`.
- Materialize discovery results before assigning to `provider->devices_`, so device opening is complete before the mutation point.

## QueryAddressList Gate

We still need to confirm this on target ND providers:

> `IND2Adapter::QueryAddressList` always returns the IPv4/IPv6 local addresses supported by the opened adapter instance, and the result is sufficient as the final source for `v4_addr_` / `v6_addr_`.

Until that is confirmed:

- Prefer adapter-owned addresses from `IND2Adapter::QueryAddressList`.
- If adapter address query fails or returns an empty list, fall back to the provider seed addresses in the same AdapterId group.

After confirmation:

- Remove the fallback path.
- Let provider seed addresses serve only `ResolveAddress` and `create_device()` display/debug naming.

## Implementation Stages

### Stage 1: Local Range Helpers

Add the helper templates directly inside `asio::rdma::detail` in `nd_device_impl.hpp`:

- `to<std::vector>()`
- `filter_map(fn)`
- `sort_by(key_fn)`
- `chunk_by(pred)` eager version

Constraints:

- No separate public/detail header.
- `chunk_by` requires already-sorted input and only merges adjacent equivalent elements.
- No lazy `chunk_by_view`; discovery data is tiny and eager materialization has simpler lifetimes.
- No pipeable `for_each`; use a normal `for` for the final mutation.

### Stage 2: Adapter Address Query

Add:

```cpp
inline void query_adapter_addr_list(
    nd2_adapter_ptr const& adapter,
    std::vector<nd2_sockaddr_t>& addresses,
    asio::error_code& ec);
```

Implementation mirrors provider-level `enumerate_addr_list`:

- First call `adapter->QueryAddressList(nullptr, &buffer_size)`.
- Allocate `SOCKET_ADDRESS_LIST`.
- Call `adapter->QueryAddressList(...)`.
- Copy into `std::vector<nd2_sockaddr_t>`.
- Keep only usable copied addresses; family filtering happens at attach time.

### Stage 3: `open_device`

Add a higher-level primitive:

```cpp
inline nd_adapter_ptr open_device(
    nd_provider_ptr const& provider,
    UINT64 adapter_id,
    std::span<nd2_sockaddr_t const> provider_addresses,
    asio::error_code& ec);
```

Flow:

- `provider_addresses` must be non-empty.
- `create_device(provider, provider_addresses.front(), adapter_id, ec)`.
- Query adapter-owned addresses.
- Attach adapter-owned addresses if available; otherwise attach provider seed addresses.
- Reject devices with neither v4 nor v6 local address.

### Stage 4: Provider-Scoped Discovery

Add:

```cpp
inline std::vector<nd_adapter_ptr> discover_provider_devices(
    nd_provider_ptr const& provider);
```

Pipeline:

```text
enumerate_addr_list(*provider)
  -> filter AF_INET / AF_INET6
  -> filter_map ResolveAddress to {adapter_id, address}
  -> sort_by(adapter_id)
  -> chunk_by(same adapter_id)
  -> open_device(provider, adapter_id, addresses)
  -> to<vector>
```

`open_adapters` becomes:

```cpp
for (auto& provider : providers) {
  provider->devices_ = discover_provider_devices(provider);
}
```

## Validation

- Release build.
- ND device manager test must still find one dual-family device when the host has v4/v6 on the same adapter.
- ND public header / config / error tests.
- Dual-family echo when available.
- `asio_perftest` smoke to confirm discovery changes do not affect hot-path performance.

## Done Criteria

- No ibv changes.
- No public API changes.
- `open_adapters` no longer does a linear search inside the address loop.
- Each `{provider, AdapterId}` is opened at most once.
- Adapter-owned addresses are preferred, with provider seed fallback until `IND2Adapter::QueryAddressList` is fully validated.
