#pragma once

#include <cassert>

#include "rdma/nd/nd_types.hpp"
#include "rdma/nd/nd_error.hpp"
#include "rdma/nd/detail/nd_impl_types.hpp"

namespace asio::rdma::detail {

inline bool is_valid_addr(SOCKADDR const& addr);
inline bool is_valid_proto(WSAPROTOCOL_INFOW const& proto);

inline void enumerate_protos(std::vector<WSAPROTOCOL_INFOW>& out_protos,
                             asio::error_code& ec);
inline std::vector<WSAPROTOCOL_INFOW> enumerate_protos();
inline std::wstring get_provider_path(WSAPROTOCOL_INFOW const& proto,
                                      asio::error_code& ec);
inline auto create_provider_factory(std::wstring provider_path,
                                    WSAPROTOCOL_INFOW const& proto,
                                    asio::error_code& ec)
    -> nd_provider_factory_ptr;
inline auto create_provider(nd_provider_factory_t const& factory,
                            std::error_code& ec) -> nd2_provider_ptr;
inline void enumerate_addr_list(nd_provider_t const& provider,
                                std::vector<nd2_sockaddr_t>& addr_list,
                                asio::error_code& ec);
inline auto enumerate_addr_list(nd_provider_t const& provider)
    -> std::vector<nd2_sockaddr_t>;
inline UINT64 resolve_adapter_id(nd2_provider_ptr const& provider,
                                 sockaddr const* addrin, std::size_t addr_size,
                                 asio::error_code& ec);
inline ND2_ADAPTER_INFO query_adapter_info(nd2_adapter_ptr const& adaptor,
                                           asio::error_code& ec);
inline std::string query_adapter_name(ND2_ADAPTER_INFO const& info,
                                      sockaddr const* addrin, std::size_t addr_size,
                                      asio::error_code& ec);
// Open one physical adapter by its AdapterId (a single OpenAdapter == one PD
// resource domain). Local v4/v6 addresses are attached afterwards in
// open_adapters; addr is the triggering address, used only for the display name.
inline nd_adapter_ptr create_device(nd_provider_ptr const& provider,
                                    nd2_sockaddr_t const& addr,
                                    UINT64 adapter_id, asio::error_code& ec);
inline std::vector<nd_provider_ptr> get_providers(asio::error_code& ec);
inline std::vector<nd_provider_ptr> get_providers();
inline void open_adapters(std::vector<nd_provider_ptr>& providers);
inline bool is_valid_adapter(nd_adapter_ptr const& adapter,
                             ND2_ADAPTER_INFO const& config);
inline bool is_valid_adapter(nd_adapter_ptr const& adapter,
                             nd_config_t const& config);
inline bool is_valid_adapter(nd_adapter_ptr const& adapter);
inline HANDLE create_overlapped_file(native_context_t* context,
                                     asio::error_code& ec);
}

namespace asio::rdma::detail {

HANDLE create_overlapped_file(native_context_t* context, asio::error_code& ec) {
  assert(context);
  HANDLE result;
  auto const hr = context->CreateOverlappedFile(&result);
  ec = static_cast<nd_errc>(hr);
  return result;
}

bool is_valid_addr(SOCKADDR const& addr) {
  switch (addr.sa_family) {
    case AF_INET: {
      sockaddr_in const& addr4 = reinterpret_cast<sockaddr_in const&>(addr);
      /* HACK-alert: reject local or MS default IPv4 addrs */
      return !(addr4.sin_addr.S_un.S_un_b.s_b1 == 169 ||
               addr4.sin_addr.S_un.S_un_b.s_b1 == 127);
    }
    case AF_INET6: {
      sockaddr_in6 const& addr6 = reinterpret_cast<sockaddr_in6 const&>(addr);
      return !(addr6.sin6_addr.u.Byte[0] == 0xfe &&
               addr6.sin6_addr.u.Byte[1] == 0x80);
    }
    default:
      break;
  }
  return false;
}

bool is_valid_proto(WSAPROTOCOL_INFOW const& proto) {
  constexpr auto fi_nd_proto_flag = XP1_GUARANTEED_DELIVERY |
                                    XP1_GUARANTEED_ORDER |
                                    XP1_MESSAGE_ORIENTED | XP1_CONNECT_DATA;
  if ((proto.dwServiceFlags1 & fi_nd_proto_flag) != fi_nd_proto_flag) {
    return false;
  }
  if (!(proto.iAddressFamily == AF_INET || proto.iAddressFamily == AF_INET6)) {
    return false;
  }
  if (proto.iSocketType != -1) {
    return false;
  }
  if (proto.iProtocol || proto.iProtocolMaxOffset) {
    return false;
  }
  return proto.iVersion == NDVER;
}

void enumerate_protos(std::vector<WSAPROTOCOL_INFOW>& out_protos,
                      asio::error_code& ec) {
  DWORD proto_len = 0;
  int err = 0;

  int number_info = ::WSCEnumProtocols(nullptr, nullptr, &proto_len, &err);
  if (number_info == SOCKET_ERROR && err != WSAENOBUFS) {
    ec = make_system_error_code(err);
    return;
  }

  std::size_t const array_size = proto_len / sizeof(WSAPROTOCOL_INFOW);
  std::vector<WSAPROTOCOL_INFOW> result{};
  result.resize(array_size);
  number_info = ::WSCEnumProtocols(nullptr, result.data(), &proto_len, &err);
  if (number_info == SOCKET_ERROR) {
    ec = make_system_error_code(err);
    return;
  }

  auto itr = std::remove_if(result.begin(), result.end(),
                            [](WSAPROTOCOL_INFOW const& elem) {
                              return !is_valid_proto(elem);
                            });
  result.erase(itr, result.end());

  out_protos = std::move(result);
  ec.clear();
}

std::vector<WSAPROTOCOL_INFOW> enumerate_protos() {
  std::vector<WSAPROTOCOL_INFOW> result{};
  std::error_code ec{};
  enumerate_protos(result, ec);
  throw_error(ec);
  return result;
}

std::wstring get_provider_path(WSAPROTOCOL_INFOW const& proto,
                               asio::error_code& ec) {
  int len = 0, err = 0, res = 0;

  res = WSCGetProviderPath((GUID*)&proto.ProviderId, NULL, &len, &err);
  if (res != SOCKET_ERROR || err != WSAEFAULT) {
    ec = make_system_error_code(err);
    return {};
  }
  std::wstring temp{};
  temp.resize(len);
  res = WSCGetProviderPath((GUID*)&proto.ProviderId, temp.data(), &len, &err);
  if (res != 0) {
    if (res == WSAEINVAL) {
      ec = make_error_code(std::errc::invalid_argument);
    }
    else  // WSAEFAULT
    {
      ec = make_error_code(std::errc::not_enough_memory);
    }
    return {};
  }

  len = ExpandEnvironmentStringsW(temp.c_str(), NULL, 0);
  if (len == 0) {
    ec = make_system_error_code(::GetLastError());
    return {};
  }

  std::wstring result{};
  result.resize(len);
  len = ExpandEnvironmentStringsW(temp.c_str(), result.data(), len);
  if (len == 0) {
    ec = make_system_error_code(::GetLastError());
    return {};
  }

  return result;
}

auto create_provider_factory(std::wstring provider_path,
                             WSAPROTOCOL_INFOW const& proto,
                             asio::error_code& ec)
    -> nd_provider_factory_ptr {
  unique_module_t provier_module{LoadLibraryW(provider_path.c_str())};
  if (!provier_module) {
    ec = make_system_error_code(::GetLastError());
    return nullptr;
  }

  dll_can_unload_now unload = reinterpret_cast<dll_can_unload_now>(
      GetProcAddress(provier_module.get(), "DllCanUnloadNow"));
  if (!unload) {
    ec = make_system_error_code(::GetLastError());
    return nullptr;
  }

  dll_get_class_object getclassobj = reinterpret_cast<dll_get_class_object>(
      GetProcAddress(provier_module.get(), "DllGetClassObject"));
  if (!getclassobj) {
    ec = make_system_error_code(::GetLastError());
    return nullptr;
  }

  class_factory_ptr class_factory{};
  HRESULT hr =
      getclassobj(proto.ProviderId, IID_IClassFactory,
                  reinterpret_cast<LPVOID*>(class_factory.GetAddressOf()));
  if (hr != S_OK) {
    ec = make_system_error_code(hr);
    return nullptr;
  }

  auto provider_factory = std::make_shared<nd_provider_factory_t>();
  provider_factory->proto_ = proto;
  provider_factory->module_name_ = std::move(provider_path);
  provider_factory->module_ = std::move(provier_module);
  provider_factory->unload_ = unload;
  provider_factory->factory_ = std::move(class_factory);
  return provider_factory;
}

auto create_provider(nd_provider_factory_t const& factory, asio::error_code& ec)
    -> nd2_provider_ptr {
  nd2_provider_ptr provdier{};
  HRESULT const hr = factory.factory_->CreateInstance(
      nullptr, IID_IND2Provider,
      reinterpret_cast<LPVOID*>(provdier.GetAddressOf()));
  if (hr != S_OK) {
    ec = make_system_error_code(hr);
    return nullptr;
  }
  return provdier;
}

void enumerate_addr_list(nd_provider_t const& provider,
                         std::vector<nd2_sockaddr_t>& addr_list,
                         asio::error_code& ec) {
  ULONG addr_list_buffer_size{0ul};
  provider.provider_->QueryAddressList(nullptr, &addr_list_buffer_size);
  if (addr_list_buffer_size == 0) {
    ec = make_error_code(std::errc::address_not_available);
    return;
  }

  scope_buffer buffer{std::malloc(addr_list_buffer_size)};
  if (!buffer.buffer) {
    ec = make_error_code(std::errc::not_enough_memory);
    return;
  }
  SOCKET_ADDRESS_LIST* temp_addr_list =
      static_cast<SOCKET_ADDRESS_LIST*>(buffer.buffer);
  HRESULT hr = provider.provider_->QueryAddressList(temp_addr_list,
                                                    &addr_list_buffer_size);
  if (hr != ND_SUCCESS) {
    ec = make_nd_error_code(hr);
    return;
  }
  if (temp_addr_list->iAddressCount <= 0) {
    ec = make_error_code(std::errc::address_not_available);
    return;
  }

  auto addr_range = std::ranges::subrange{
    temp_addr_list->Address,
    temp_addr_list->Address + temp_addr_list->iAddressCount}
    | std::views::transform([&](auto const& sock_addr) {
      nd2_sockaddr_t result{};
      // The union's sockaddr_storage member is the capacity bound for the copy.
      assert(static_cast<std::size_t>(sock_addr.iSockaddrLength) <=
             sizeof(result.src_storage_));
      std::memcpy(&result.src_addr_, sock_addr.lpSockaddr,
                  sock_addr.iSockaddrLength);
      result.address_size_ = sock_addr.iSockaddrLength;
      return result;
    });
  std::vector<nd2_sockaddr_t> result{addr_range.begin(), addr_range.end()};
  addr_list = std::move(result);
  ec.clear();
}

auto enumerate_addr_list(nd_provider_t const& provider)
    -> std::vector<nd2_sockaddr_t> {
  std::vector<nd2_sockaddr_t> result{};
  std::error_code ec{};
  enumerate_addr_list(provider, result, ec);
  throw_error(ec);
  return result;
}

// Map a local address to its provider-managed AdapterId. Same physical NIC ->
// same id for both its v4 and v6 addresses (this is how open_adapters groups
// v4/v6 of one NIC into a single device). See nd_dual_family_plan.md.
UINT64 resolve_adapter_id(nd2_provider_ptr const& provider, sockaddr const* addrin,
                          std::size_t addr_size, asio::error_code& ec) {
  UINT64 adapter_id = 0;
  HRESULT hr = provider->ResolveAddress(addrin, static_cast<ULONG>(addr_size),
                                        &adapter_id);
  if (hr != ND_SUCCESS) {
    ec = make_nd_error_code(hr);
    return 0;
  }
  return adapter_id;
}

ND2_ADAPTER_INFO query_adapter_info(nd2_adapter_ptr const& adaptor,
                                    asio::error_code& ec) {
  assert(adaptor);
  ND2_ADAPTER_INFO result = {0};
  result.InfoVersion = ND_VERSION_2;
  ULONG linfo = sizeof(result);
  HRESULT hr = adaptor->Query(&result, &linfo);
  if (hr != ND_SUCCESS) {
    ec = make_nd_error_code(hr);
  }
  return result;
}

std::string query_adapter_name(ND2_ADAPTER_INFO const& info, sockaddr const* addrin,
                               std::size_t addr_size, asio::error_code& ec) {
  std::string result{};
  DWORD addrlen = 0;
#if defined(_MSC_VER) && (_MSC_VER >= 1800)
  int res =
      WSAAddressToStringW(const_cast<LPSOCKADDR>(addrin),
                          static_cast<DWORD>(addr_size), NULL, NULL, &addrlen);
  if (res != 0) {
    ec = make_system_error_code(::WSAGetLastError());
  }
  if (res == SOCKET_ERROR && ec.value() == WSAEFAULT && addrlen != 0) {
    LPWSTR string_buffer = (LPWSTR)_alloca(addrlen * sizeof(WCHAR));
    res = WSAAddressToStringW(const_cast<LPSOCKADDR>(addrin),
                              static_cast<DWORD>(addr_size), NULL,
                              string_buffer, &addrlen);
    if (res != 0) {
      ec = make_system_error_code(::WSAGetLastError());
    }
    else {
      ec.clear();
    }
    if (!ec) {
      res = ::WideCharToMultiByte(CP_ACP, 0, string_buffer, -1, NULL, 0, 0, 0);
      if (res == 0) {
        ec = make_system_error_code(GetLastError());
      }
      else {
        result.resize(res);
        res = ::WideCharToMultiByte(CP_ACP, 0, string_buffer, -1, result.data(),
                                    res, 0, 0);
        if (res == 0) {
          ec = make_system_error_code(::GetLastError());
        }
      }
    }
  }
#else
  int res =
      WSAAddressToStringA(const_cast<LPSOCKADDR>(addrin),
                          static_cast<DWORD>(addr_size), NULL, NULL, &addrlen);
  if (res != 0) {
    ec = make_system_error_code(::WSAGetLastError());
  }

  if (res == SOCKET_ERROR && ec.value() == WSAEFAULT && addrlen != 0) {
    result.resize(addrlen);
    res = WSAAddressToStringA(const_cast<LPSOCKADDR>(addrin),
                              static_cast<DWORD>(addr_size), NULL,
                              result.data(), &addrlen);
    if (res != 0) {
      ec = make_system_error_code(::WSAGetLastError());
    }
    else {
      ec.clear();
    }
  }
#endif
  return result;
}

nd_adapter_ptr create_device(nd_provider_ptr const& provider,
                             nd2_sockaddr_t const& addr, UINT64 adapter_id,
                             asio::error_code& ec) {
  auto result = std::make_shared<nd_adapter_t>();
  nd2_adapter_ptr adapter_ptr{};
  HRESULT hr = provider->provider_->OpenAdapter(
      IID_IND2Adapter, adapter_id,
      reinterpret_cast<LPVOID*>(adapter_ptr.GetAddressOf()));
  if (hr != ND_SUCCESS) {
    ec = make_nd_error_code(hr);
    return nullptr;
  }
  auto const adapter_info = query_adapter_info(adapter_ptr, ec);
  if (ec) {
    return nullptr;
  }
  // Name is just for display; derive it from the triggering address.
  auto const adapter_name = query_adapter_name(
      adapter_info, &addr.src_addr_, addr.address_size_, ec);
  if (ec) {
    return nullptr;
  }
  auto pd = std::make_unique<native_pd_t>();
  pd->context_ = adapter_ptr.Get();
  pd->sync_handle_.reset(create_overlapped_file(adapter_ptr.Get(), ec));
  if (ec) {
    return nullptr;
  }
  result->adapter_ = adapter_ptr;
  result->pd_ = std::move(pd);
  result->adapter_id_ = adapter_id;
  result->name_ = adapter_name;
  result->info_ = adapter_info;
  return result;
}

std::vector<nd_provider_ptr> get_providers(asio::error_code& ec) {
  std::vector<nd_provider_ptr> result{};
  std::vector<WSAPROTOCOL_INFOW> protos{};
  enumerate_protos(protos, ec);
  if (ec) {
    return result;
  }
  auto providers = 
    protos |
    std::views::transform([](auto const& proto) {
      asio::error_code ec{};
      auto result = std::make_shared<nd_provider_t>();
      auto const provider_path = get_provider_path(proto, ec);
      if (ec) {
        return result;
      }
      auto provider_factory =
          create_provider_factory(provider_path, proto, ec);
      if (ec) {
        return result;
      }
      auto provider = create_provider(*provider_factory, ec);
      if (ec) {
        return result;
      }
      result->factory_ = provider_factory;
      result->provider_ = provider;
      return result;
    }) |
    std::views::filter([](auto const& provider) {
      return provider->provider_ != nullptr;
    });

  result = {
    std::ranges::begin(providers),
    std::ranges::end(providers)
  };
  if (result.empty()) {
    ec = rdma_errc::no_available_device;
  }
  return result;
}

std::vector<nd_provider_ptr> get_providers() {
  asio::error_code ec{};
  auto const result = get_providers(ec);
  asio::detail::throw_error(ec);
  return result;
}

// Build one device per physical adapter (per AdapterId), attaching that
// adapter's v4 and/or v6 local addresses. A NIC carrying both a v4 and a v6
// address therefore yields ONE device (one OpenAdapter / one PD), not two.
// See nd_dual_family_plan.md.
void open_adapters(std::vector<nd_provider_ptr>& providers) {
  std::ranges::for_each(providers, [](auto& provider) {
    auto const addr_list = enumerate_addr_list(*provider);
    std::vector<nd_adapter_ptr> devices;
    for (auto const& sock_addr : addr_list) {
      auto const family = sock_addr.src_addr_.sa_family;
      if (family != AF_INET && family != AF_INET6) {
        continue;
      }
      asio::error_code ec{};
      UINT64 const id = resolve_adapter_id(
          provider->provider_, &sock_addr.src_addr_, sock_addr.address_size_, ec);
      if (ec) {
        continue;  // unresolvable address: skip (mirrors old per-adapter filter)
      }
      // Find-or-open the device for this AdapterId.
      auto it = std::ranges::find_if(
          devices, [id](auto const& d) { return d && d->adapter_id_ == id; });
      nd_adapter_ptr device;
      if (it != devices.end()) {
        device = *it;
      } else {
        device = create_device(provider, sock_addr, id, ec);
        if (ec || !device) {
          continue;
        }
        devices.push_back(device);
      }
      // Attach this address to its family slot (first one wins).
      if (family == AF_INET) {
        if (!device->v4_addr_) device->v4_addr_ = sock_addr;
      } else {
        if (!device->v6_addr_) device->v6_addr_ = sock_addr;
      }
    }
    provider->devices_ = std::move(devices);
  });
}

bool is_valid_adapter(nd_adapter_ptr const& adapter,
                      ND2_ADAPTER_INFO const& config) {
  if (!adapter) {
    return false;
  }
  auto const& capabilities = adapter->info_;
  if (config.MaxRegistrationSize != 0 &&
      config.MaxRegistrationSize > capabilities.MaxRegistrationSize) {
    return false;
  }
  if (config.MaxWindowSize != 0 &&
      config.MaxWindowSize > capabilities.MaxWindowSize) {
    return false;
  }
  if (config.MaxInitiatorSge != 0 &&
      config.MaxInitiatorSge > capabilities.MaxInitiatorSge) {
    return false;
  }
  if (config.MaxReceiveSge != 0 &&
      config.MaxReceiveSge > capabilities.MaxReceiveSge) {
    return false;
  }
  if (config.MaxReadSge != 0 && config.MaxReadSge > capabilities.MaxReadSge) {
    return false;
  }
  if (config.MaxTransferLength != 0 &&
      config.MaxTransferLength > capabilities.MaxTransferLength) {
    return false;
  }
  if (config.MaxInlineDataSize != 0 &&
      config.MaxInlineDataSize > capabilities.MaxInlineDataSize) {
    return false;
  }
  if (config.MaxInboundReadLimit != 0 &&
      config.MaxInboundReadLimit > capabilities.MaxInboundReadLimit) {
    return false;
  }
  if (config.MaxOutboundReadLimit != 0 &&
      config.MaxOutboundReadLimit > capabilities.MaxOutboundReadLimit) {
    return false;
  }
  if (config.MaxReceiveQueueDepth != 0 &&
      config.MaxReceiveQueueDepth > capabilities.MaxReceiveQueueDepth) {
    return false;
  }
  if (config.MaxInitiatorQueueDepth != 0 &&
      config.MaxInitiatorQueueDepth > capabilities.MaxInitiatorQueueDepth) {
    return false;
  }
  if (config.MaxSharedReceiveQueueDepth != 0 &&
      config.MaxSharedReceiveQueueDepth >
          capabilities.MaxSharedReceiveQueueDepth) {
    return false;
  }
  if (config.MaxCompletionQueueDepth != 0 &&
      config.MaxCompletionQueueDepth > capabilities.MaxCompletionQueueDepth) {
    return false;
  }
  if (config.InlineRequestThreshold != 0 &&
      config.InlineRequestThreshold > capabilities.InlineRequestThreshold) {
    return false;
  }
  if (config.LargeRequestThreshold != 0 &&
      config.LargeRequestThreshold > capabilities.LargeRequestThreshold) {
    return false;
  }
  if (config.MaxCallerData != 0 &&
      config.MaxCallerData > capabilities.MaxCallerData) {
    return false;
  }
  if (config.MaxCalleeData != 0 &&
      config.MaxCalleeData > capabilities.MaxCalleeData) {
    return false;
  }
  if (config.AdapterFlags != 0) {
    if ((config.AdapterFlags & capabilities.AdapterFlags) !=
      config.AdapterFlags) {
      return false;
    }
  }
  return true;
}

bool is_valid_adapter(nd_adapter_ptr const& adapter,
                      nd_config_t const& config) {
  if (!adapter) {
    return false;
  }
  auto const& capabilities = adapter->info_;
  if (config.cqe_ > capabilities.MaxCompletionQueueDepth) {
    return false;
  }
  if (config.max_send_wr_ > capabilities.MaxInitiatorQueueDepth) {
    return false;
  }
  if (config.max_recv_wr_ > capabilities.MaxReceiveQueueDepth) {
    return false;
  }
  if (config.max_send_sge_ > capabilities.MaxInitiatorSge) {
    return false;
  }
  if (config.max_recv_sge_ > capabilities.MaxReceiveSge) {
    return false;
  }
  if (config.max_inline_data_ > capabilities.MaxInlineDataSize) {
    return false;
  }
  if (config.inbound_read_limit_ > capabilities.MaxInboundReadLimit) {
    return false;
  }
  if (config.outbound_read_limit_ > capabilities.MaxOutboundReadLimit) {
    return false;
  }
  return true;
}

bool is_valid_adapter(nd_adapter_ptr const& adapter) {
  return adapter != nullptr && adapter->adapter_ != nullptr;
}

}
