#ifndef RDMA_ND_DETAIL_IMPL_ND_OPS_CM_IPP
#define RDMA_ND_DETAIL_IMPL_ND_OPS_CM_IPP

#include "rdma/nd/detail/nd_ops_cm.hpp"

#include "asio/detail/push_options.hpp"

// interfaces simular with rdma-cm
namespace asio::rdma::detail {

// conncetor interfaces
IND2Connector* create_connector(IND2Adapter* adapter,
                                HANDLE overlapped_handle,
                                asio::error_code& ec) {
  assert(adapter);
  IND2Connector* result{nullptr};
  auto const hr = adapter->CreateConnector(IID_IND2Connector, overlapped_handle,
                                           reinterpret_cast<LPVOID*>(&result));
  ec = static_cast<nd_errc>(hr);
  return result;
}

result_type bind_addr(IND2Connector* connector,
                      sockaddr const* addrin, std::size_t addr_size,
                      asio::error_code& ec) {
  assert(connector);
  auto const hr = connector->Bind(addrin, static_cast<size_type>(addr_size));
  if (hr != ND_SUCCESS) {
    assert(hr != ND_PENDING);  // TODO ... test
  }
  ec = static_cast<nd_errc>(hr);
  return hr;
}

result_type accept(IND2Connector* connector, IND2QueuePair* qp,
                   ULONG inbound_read_limit, ULONG outbound_read_limit,
                   void const* private_data, ULONG private_data_size,
                   OVERLAPPED* overlapped, asio::error_code& ec) {
  assert(connector);
  assert(qp);
  auto const hr =
      connector->Accept(qp, inbound_read_limit, outbound_read_limit,
                        private_data, private_data_size, overlapped);

  if (hr != ND_SUCCESS && hr != ND_PENDING) {
    ec = static_cast<nd_errc>(hr);
  } else {
    ec.clear();
  }
  return hr;
}

result_type connect(IND2Connector* connector, IND2QueuePair* qp,
                    sockaddr const* addrin, size_t address_size,
                    ULONG inbound_read_limit, ULONG outbound_read_limit,
                    const void* private_data, ULONG data_size,
                    OVERLAPPED* overlapped, asio::error_code& ec) {
  assert(connector);
  assert(qp);
  auto const hr = connector->Connect(
      qp, addrin, static_cast<size_type>(address_size), inbound_read_limit,
      outbound_read_limit, private_data, data_size, overlapped);

  if (hr != ND_SUCCESS && hr != ND_PENDING) {
    ec = static_cast<nd_errc>(hr);
  } else {
    ec.clear();
  }
  return hr;
}

result_type disconnect(IND2Connector* connector, OVERLAPPED* overlapped,
                       asio::error_code& ec) {
  assert(connector);
  auto const hr = connector->Disconnect(overlapped);
  if (hr != ND_SUCCESS && hr != ND_PENDING) {
    ec = static_cast<nd_errc>(hr);
  } else {
    ec.clear();
  }
  return hr;
}

// Arm a disconnect NOTIFICATION: the overlapped completes when the connection is
// disconnected (peer or self). Backs async_wait_disconnect (on_disconnect).
result_type notify_disconnect(IND2Connector* connector,
                              OVERLAPPED* overlapped,
                              asio::error_code& ec) {
  assert(connector);
  auto const hr = connector->NotifyDisconnect(overlapped);
  if (hr != ND_SUCCESS && hr != ND_PENDING) {
    ec = static_cast<nd_errc>(hr);
  } else {
    ec.clear();
  }
  return hr;
}

// listener interfaces
IND2Listener* create_listener(IND2Adapter* adapter,
                              HANDLE overlapped_handle,
                              asio::error_code& ec) {
  assert(adapter);
  IND2Listener* result{nullptr};
  auto const hr = adapter->CreateListener(IID_IND2Listener, overlapped_handle,
                                          reinterpret_cast<LPVOID*>(&result));
  ec = static_cast<nd_errc>(hr);
  return result;
}

result_type listen(IND2Listener* listener, int backlog,
                   asio::error_code& ec) {
  assert(listener);
  auto const hr = listener->Listen(static_cast<ULONG>(backlog));
  ec = static_cast<nd_errc>(hr);
  return hr;
}

result_type get_connection_request(IND2Listener* listener,
                                   IND2Connector* connector,
                                   OVERLAPPED* overlapped,
                                   asio::error_code& ec) {
  assert(listener);
  assert(connector);
  auto const hr =
      listener->GetConnectionRequest(connector, overlapped);
  if (hr != ND_SUCCESS && hr != ND_PENDING) {
    ec = static_cast<nd_errc>(hr);
  } else {
    ec.clear();
  }
  return hr;
}

result_type bind_addr(IND2Listener* listener, sockaddr const* addrin,
                      std::size_t addr_size, asio::error_code& ec) {
  assert(listener);
  auto const hr = listener->Bind(addrin, static_cast<size_type>(addr_size));
  if (hr != ND_SUCCESS) {
    assert(hr != ND_PENDING);  // TODO ... test
  }
  ec = static_cast<nd_errc>(hr);
  return hr;
}

}

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_ND_DETAIL_IMPL_ND_OPS_CM_IPP
