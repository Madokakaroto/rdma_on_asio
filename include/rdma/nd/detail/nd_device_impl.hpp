#pragma once

#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "asio/ip/tcp.hpp"
#include "rdma/nd/nd_types.hpp"
#include "rdma/nd/nd_error.hpp"
#include "rdma/nd/detail/nd_impl_types.hpp"

namespace asio::rdma::detail {

template <template <typename...> typename Container>
struct to_action {};

template <typename Fn>
struct filter_map_action {
  Fn fn;
};

template <typename KeyFn>
struct sort_by_action {
  KeyFn key_fn;
};

template <typename Pred>
struct chunk_by_action {
  Pred pred;
};

template <template <typename...> typename Container>
to_action<Container> to() {
  return {};
}

template <typename Fn>
filter_map_action<std::decay_t<Fn>> filter_map(Fn&& fn) {
  return filter_map_action<std::decay_t<Fn>>{std::forward<Fn>(fn)};
}

template <typename KeyFn>
sort_by_action<std::decay_t<KeyFn>> sort_by(KeyFn&& key_fn) {
  return sort_by_action<std::decay_t<KeyFn>>{std::forward<KeyFn>(key_fn)};
}

template <typename Pred>
chunk_by_action<std::decay_t<Pred>> chunk_by(Pred&& pred) {
  return chunk_by_action<std::decay_t<Pred>>{std::forward<Pred>(pred)};
}

template <std::ranges::input_range Range,
          template <typename...> typename Container>
auto operator|(Range&& range, to_action<Container>) {
  using value_type = std::ranges::range_value_t<Range>;

  Container<value_type> result;
  if constexpr (std::ranges::sized_range<Range>) {
    if constexpr (requires(Container<value_type>& c,
                           std::ranges::range_size_t<Range> n) {
                    c.reserve(n);
                  }) {
      result.reserve(std::ranges::size(range));
    }
  }

  std::ranges::copy(range, std::back_inserter(result));
  return result;
}

template <std::ranges::input_range Range, typename Fn>
auto operator|(Range&& range, filter_map_action<Fn> action) {
  using input_type = std::ranges::range_value_t<Range>;
  using optional_type = std::invoke_result_t<Fn&, input_type const&>;
  using output_type = std::decay_t<decltype(*std::declval<optional_type&>())>;

  std::vector<output_type> result;
  if constexpr (std::ranges::sized_range<Range>) {
    result.reserve(std::ranges::size(range));
  }

  for (input_type const& item : range) {
    auto value = std::invoke(action.fn, item);
    if (value) {
      result.push_back(std::move(*value));
    }
  }
  return result;
}

template <std::ranges::input_range Range, typename KeyFn>
auto operator|(Range&& range, sort_by_action<KeyFn> action) {
  auto result = std::forward<Range>(range) | to<std::vector>();
  std::ranges::sort(result, [&](auto const& a, auto const& b) {
    return std::invoke(action.key_fn, a) < std::invoke(action.key_fn, b);
  });
  return result;
}

template <std::ranges::input_range Range, typename Pred>
auto operator|(Range&& range, chunk_by_action<Pred> action) {
  using value_type = std::ranges::range_value_t<Range>;

  std::vector<std::vector<value_type>> chunks;
  if constexpr (std::ranges::sized_range<Range>) {
    chunks.reserve(std::ranges::size(range));
  }

  auto it = std::ranges::begin(range);
  auto const last = std::ranges::end(range);
  if (it == last) {
    return chunks;
  }

  std::vector<value_type> current;
  current.push_back(*it);
  auto previous = it++;
  for (; it != last; previous = it++) {
    if (std::invoke(action.pred, *previous, *it)) {
      current.push_back(*it);
    } else {
      chunks.push_back(std::move(current));
      current.clear();
      current.push_back(*it);
    }
  }
  chunks.push_back(std::move(current));
  return chunks;
}

ASIO_DECL bool is_valid_proto(WSAPROTOCOL_INFOW const& proto);

ASIO_DECL void enumerate_protos(std::vector<WSAPROTOCOL_INFOW>& out_protos,
                                asio::error_code& ec);
ASIO_DECL std::vector<WSAPROTOCOL_INFOW> enumerate_protos();
ASIO_DECL std::wstring get_provider_path(WSAPROTOCOL_INFOW const& proto,
                                         asio::error_code& ec);
ASIO_DECL auto create_provider_factory(std::wstring provider_path,
                                       WSAPROTOCOL_INFOW const& proto,
                                       asio::error_code& ec)
    -> nd_provider_factory_ptr;
ASIO_DECL auto create_provider(nd_provider_factory_t const& factory,
                               asio::error_code& ec) -> nd2_provider_ptr;
ASIO_DECL void enumerate_addr_list(nd_provider_t const& provider,
                                   std::vector<nd2_sockaddr_t>& addr_list,
                                   asio::error_code& ec);
ASIO_DECL auto enumerate_addr_list(nd_provider_t const& provider)
    -> std::vector<nd2_sockaddr_t>;
ASIO_DECL void query_adapter_addr_list(nd2_adapter_ptr const& adapter,
                                       std::vector<nd2_sockaddr_t>& addr_list,
                                       asio::error_code& ec);
ASIO_DECL auto query_adapter_addr_list(nd2_adapter_ptr const& adapter)
    -> std::vector<nd2_sockaddr_t>;
ASIO_DECL UINT64 resolve_adapter_id(nd2_provider_ptr const& provider,
                                    sockaddr const* addrin,
                                    std::size_t addr_size,
                                    asio::error_code& ec);
ASIO_DECL ND2_ADAPTER_INFO query_adapter_info(nd2_adapter_ptr const& adapter,
                                              asio::error_code& ec);
ASIO_DECL std::string query_adapter_name(ND2_ADAPTER_INFO const& info,
                                         sockaddr const* addrin,
                                         std::size_t addr_size,
                                         asio::error_code& ec);
// Open one physical adapter by its AdapterId (a single OpenAdapter == one PD
// resource domain). Local v4/v6 addresses are attached afterwards in
// open_adapters; addr is the triggering address, used only for the display name.
ASIO_DECL nd_adapter_ptr create_device(nd_provider_ptr const& provider,
                                       nd2_sockaddr_t const& addr,
                                       UINT64 adapter_id, asio::error_code& ec);
ASIO_DECL nd_adapter_ptr open_device(
    nd_provider_ptr const& provider, UINT64 adapter_id,
    std::span<nd2_sockaddr_t const> provider_addresses, asio::error_code& ec);
ASIO_DECL std::vector<nd_adapter_ptr> discover_provider_devices(
    nd_provider_ptr const& provider);
ASIO_DECL std::vector<nd_provider_ptr> get_providers(asio::error_code& ec);
ASIO_DECL std::vector<nd_provider_ptr> get_providers();
ASIO_DECL void open_adapters(std::vector<nd_provider_ptr>& providers);
ASIO_DECL bool is_valid_adapter(nd_adapter_ptr const& adapter,
                                ND2_ADAPTER_INFO const& config);
ASIO_DECL bool is_valid_adapter(nd_adapter_ptr const& adapter,
                                nd_config_t const& config);
ASIO_DECL bool is_valid_adapter(nd_adapter_ptr const& adapter);

template <typename CreateFn>
HANDLE create_overlapped_file_checked(CreateFn&& create,
                                      asio::error_code& ec) {
  HANDLE result = nullptr;
  auto const hr = std::forward<CreateFn>(create)(&result);
  if (FAILED(hr)) {
    ec = make_nd_error_code(hr);
    return nullptr;
  }
  if (!is_closable_handle(result)) {
    ec = make_error_code(rdma_errc::invalid_handle);
    return nullptr;
  }
  ec.clear();
  return result;
}

ASIO_DECL HANDLE create_overlapped_file(native_context_t* context,
                                        asio::error_code& ec);

ASIO_DECL bool is_supported_addr_family(nd2_sockaddr_t const& addr) noexcept;

ASIO_DECL std::optional<asio::ip::address> to_ip_address(
    nd2_sockaddr_t const& addr);

ASIO_DECL std::vector<nd2_sockaddr_t> copy_socket_address_list(
    SOCKET_ADDRESS_LIST const& addr_list);

ASIO_DECL void attach_device_addresses(
    nd_adapter_ptr const& device, std::span<nd2_sockaddr_t const> addresses);

// AdapterId + the local address that resolved to it; the unit grouped by
// adapter id in discover_provider_devices.
struct resolved_nd_address {
  UINT64 adapter_id = 0;
  nd2_sockaddr_t address;
};

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/detail/impl/nd_device_impl.ipp"
#endif
