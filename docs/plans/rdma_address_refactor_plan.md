# RDMA Address Refactor Plan

## Principles

1. Delete `include/rdma/rdma_address.hpp`.
2. Public/test code uses only Asio-level address types:
   - `asio::ip::address`
   - `asio::rdma::tcp`
   - `asio::rdma::tcp::endpoint`
3. No public API accepts, returns, or names `sockaddr`.
4. Local address ownership belongs to `rdma_device`.
5. Native address structures may exist only inside backend discovery or
   bind/connect interop code.

## Target API

Each backend device exposes the same member API:

```cpp
asio::ip::address get_v4_address() const;
asio::ip::address get_v6_address() const;
```

The generic `rdma_device` alias continues to select the active backend type:

```cpp
using rdma_device = nd_device_t;   // Windows / NetworkDirect
using rdma_device = ibv_device_t;  // Linux / verbs
```

There is no `get_address(asio::rdma::tcp)` overload. Code that already has an
`asio::rdma::tcp` value may branch locally and call `get_v4_address()` or
`get_v6_address()`, but `tcp.hpp` must not include or implement any device
address logic.

The old repository-local API name `query_local_rdma_address` should be removed.
Do not replace it with repository-local `query_local_address()` wrappers. Tests
should get a device from the device manager and call `get_v4_address()` or
`get_v6_address()` directly, or use a narrowly-scoped test helper that still
requires an explicit `asio::rdma::tcp` argument and dispatches to the v4/v6
device methods.

## ND Backend

`nd_adapter_t` should store Asio addresses as its long-lived address model:

```cpp
std::optional<asio::ip::address> v4_address_;
std::optional<asio::ip::address> v6_address_;
```

Remove these persistent native-address members from `nd_adapter_t`:

```cpp
std::optional<nd2_sockaddr_t> v4_addr_;
std::optional<nd2_sockaddr_t> v6_addr_;
nd2_sockaddr_t const* local_addr_for(int native_family) const noexcept;
```

`nd2_sockaddr_t` remains only as a temporary discovery/interoperability type for
NetworkDirect calls such as `QueryAddressList`, `ResolveAddress`, `OpenAdapter`,
and adapter naming.

Listener/connector bind paths should use cached Asio addresses:

1. Listener bind: choose `get_v4_address()` or `get_v6_address()` from the
   opened port space, then build `tcp::endpoint(address, port)`.
2. Connector bind: choose `get_v4_address()` or `get_v6_address()` from the
   remote endpoint address, then build `tcp::endpoint(address, 0)`.
3. Pass the temporary endpoint's `data()` / `size()` to NetworkDirect.

This keeps native address handling inside ND service implementation code without
making it part of the device model.

## IBV Backend

`ibv_device_t` should become a fuller device wrapper rather than mostly exposing
a raw `ibv_context*`:

```cpp
native_context_t* context_ = nullptr;
unique_ibv_pd_ptr pd_;
native_device_attr_t attr_{};
std::string name_;
std::optional<asio::ip::address> v4_address_;
std::optional<asio::ip::address> v6_address_;
```

The native context remains available for verbs resources, but local address
querying uses the cached Asio addresses.

IBV local address discovery should happen during `detail::get_devices()`:

1. Build device wrappers from `rdma_get_devices()` as today.
2. Enumerate local OS interface addresses with `getifaddrs`.
3. Skip null, down, loopback, and non-IPv4/IPv6 addresses.
4. For each candidate address:
   - create a temporary `rdma_cm_id`;
   - call `rdma_bind_addr`;
   - on success, inspect `cm_id->verbs`;
   - attach the candidate IP to the matching `ibv_device_t`.
5. Cache at most one IPv4 and one IPv6 address per device.
6. Drop devices that do not resolve any local IP address and continue
   processing the rest.

Reason: `ibv_context` does not contain IP addresses, and libibverbs/librdmacm do
not provide a direct `ibv_context -> IP address` API. The RDMA CM bind check is
the direct validation that an OS IP address maps to a given verbs context.

## Include / Ownership Changes

1. Remove `#include "rdma/rdma_address.hpp"` from `include/rdma/rdma.hpp`.
2. Keep `get_v4_address` / `get_v6_address` available through the active backend
   device header already included by `rdma/rdma_types.hpp`.
3. Avoid introducing a new public address utility header.
4. Do not include backend device address implementation headers from `tcp.hpp`.

## Implementation Steps

1. Add `get_v4_address()` / `get_v6_address()` and cached Asio address fields
   to ND device.
2. Convert ND discovery so it fills `v4_address_` / `v6_address_`.
3. Update ND connector/listener bind paths to build temporary `tcp::endpoint`
   values from the v4/v6 device address.
4. Remove `nd_adapter_t::local_addr_for` and persistent `nd2_sockaddr_t`
   address members.
5. Add `get_v4_address()` / `get_v6_address()` and cached Asio address fields
   to IBV device.
6. Move IBV address mapping into IBV device discovery and filter out devices
   with no discovered local IP.
7. Remove `rdma_address.hpp` from the umbrella include and delete the file.
8. Update tests and benchmarks to use `device->get_v4_address()` /
   `device->get_v6_address()`.
9. Remove all `query_local_rdma_address` references.

## Verification

Windows / ND:

```powershell
cmake --preset default
cmake --build --preset debug
ctest --preset unit
ctest --preset echo
ctest --preset native
ctest --preset performance
```

Linux / IBV:

```bash
cmake --preset default
cmake --build --preset debug
ctest --preset unit
ctest --preset echo
ctest --preset native
```

IBV discovery changes must be verified on a verbs/rdma_cm machine.

## Decisions

1. Missing requested address: `get_v4_address()` on a v6-only device, or
   `get_v6_address()` on a v4-only device, throws
   `rdma_errc::address_family_not_supported`.
2. Repository tests use `get_v4_address()` unless a test is specifically
   validating IPv6.
