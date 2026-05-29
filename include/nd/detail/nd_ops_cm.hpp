#pragma once

#include "nd/nd_types.hpp"
#include "nd/nd_error.hpp"

// interfaces simular with rdma-cm
namespace asio::rdma::detail {

// conncetor interfaces
inline IND2Connector* create_connector(IND2Adapter* adapter,
                                       HANDLE overlapped_handle,
                                       asio::error_code& ec) {
  assert(adapter);
  IND2Connector* result{nullptr};
  auto const hr = adapter->CreateConnector(IID_IND2Connector, overlapped_handle,
                                           reinterpret_cast<LPVOID*>(&result));
  ec = static_cast<nd_errc>(hr);
  return result;
}

inline result_type bind_addr(IND2Connector* connector,
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

inline result_type accept(IND2Connector* connector, IND2QueuePair* qp,
                          ULONG inbound_read_limit, ULONG outbound_read_limit,
                          void const* private_data, ULONG private_data_size,
                          OVERLAPPED* overlapped, asio::error_code& ec) {
  assert(connector);
  assert(qp);
  auto const hr =
      connector->Accept(qp, inbound_read_limit, outbound_read_limit,
                        private_data, private_data_size, overlapped);

  ec = static_cast<nd_errc>(hr);
  return hr;
}

inline result_type connect(IND2Connector* connector, IND2QueuePair* qp,
                           sockaddr const* addrin, size_t address_size,
                           ULONG inbound_read_limit, ULONG outbound_read_limit,
                           const void* private_data, ULONG data_size,
                           OVERLAPPED* overlapped, asio::error_code& ec) {
  assert(connector);
  assert(qp);
  auto const hr = connector->Connect(
      qp, addrin, static_cast<size_type>(address_size), inbound_read_limit,
      outbound_read_limit, private_data, data_size, overlapped);

  ec = static_cast<nd_errc>(hr);
  return hr;
}

inline result_type disconnect(IND2Connector* connector, OVERLAPPED* overlapped,
                              asio::error_code& ec) {
  assert(connector);
  auto const hr = connector->Disconnect(overlapped);
  ec = static_cast<nd_errc>(hr);
  return hr;
}

// listener interfaces
inline IND2Listener* create_listener(IND2Adapter* adapter,
                                     HANDLE overlapped_handle,
                                     asio::error_code& ec) {
  assert(adapter);
  IND2Listener* result{nullptr};
  auto const hr = adapter->CreateListener(IID_IND2Listener, overlapped_handle,
                                          reinterpret_cast<LPVOID*>(&result));
  ec = static_cast<nd_errc>(hr);
  return result;
}

inline result_type listen(IND2Listener* listener, int backlog,
                          asio::error_code& ec) {
  assert(listener);
  auto const hr = listener->Listen(static_cast<ULONG>(backlog));
  ec = static_cast<nd_errc>(hr);
  return hr;
}

inline result_type get_connection_request(IND2Listener* listener,
                                          IND2Connector* connector,
                                          OVERLAPPED* overlapped,
                                          asio::error_code& ec) {
  assert(listener);
  assert(connector);
  auto const hr =
      listener->GetConnectionRequest(connector, overlapped);
  ec = static_cast<nd_errc>(hr);
  return hr;
}

inline result_type bind_addr(IND2Listener* listener, sockaddr const* addrin,
                             std::size_t addr_size, asio::error_code& ec) {
  assert(listener);
  auto const hr = listener->Bind(addrin, static_cast<size_type>(addr_size));
  if (hr != ND_SUCCESS) {
    assert(hr != ND_PENDING);  // TODO ... test
  }
  ec = static_cast<nd_errc>(hr);
  return hr;
}

inline nd_connector_state_ptr create_connector_state(
    nd_adapter_ptr const& adapter, nd_config_t const& config,
    asio::error_code& ec) {
  assert(is_valid_adapter(adapter));

  // create overlapped handle for notification of IO completion
  unique_handle_t overlapped_handle{};
  overlapped_handle.reset(create_overlapped_file(adapter->adapter_.Get(), ec));
  if (ec) {
    ASIO_ERROR_LOCATION(ec);
    return nullptr;
  }

  // create network-direct connector interface
  nd2_connector_ptr connector{};
  connector.Attach(
      create_connector(adapter->adapter_.Get(), overlapped_handle.get(), ec));
  if (ec) {
    ASIO_ERROR_LOCATION(ec);
    return nullptr;
  }

  // create verbs completion queue
  nd2_completion_queue_ptr cq{};
  native_cq_init_attr cq_init_attr{
      .overlapped_handle_ = overlapped_handle.get(),
      .processor_group_ = 0,
      .processor_affinity_ = 0,
  };
  cq.Attach(verbs_ops::create_cq(adapter->adapter_.Get(), config.cqe_,
                                 cq_init_attr, ec));
  if (ec) {
    ASIO_ERROR_LOCATION(ec);
    return nullptr;
  }

  // create verbs queue pair
  nd2_queue_pair_ptr qp{};
  native_qp_init_attr qp_init_attr{
      .qp_context_ = nullptr,
      .rcq_ = cq.Get(),
      .icq_ = cq.Get(),
      .max_send_wr_ = config.max_send_wr_,
      .max_recv_wr_ = config.max_recv_wr_,
      .max_send_sge_ = config.max_send_sge_,
      .max_recv_sge_ = config.max_recv_sge_,
      .max_inline_data_ = config.max_inline_data_,
  };
  qp.Attach(verbs_ops::create_qp(adapter->pd_.get(), qp_init_attr, ec));
  if (ec) {
    ASIO_ERROR_LOCATION(ec);
    return nullptr;
  }

  // exceptional-safty codes
  auto shared_state = std::make_shared<nd_connector_state_t>();
  shared_state->overlapped_handle_ = std::move(overlapped_handle);
  shared_state->connector_ = std::move(connector);
  shared_state->cq_ = std::move(cq);
  shared_state->qp_ = std::move(qp);
  shared_state->config_ = config;
  shared_state->adapter_ = adapter;
  return shared_state;
}

inline nd_connector_state_ptr create_connector_state(
  nd_adapter_ptr const& adapter, nd_config_t const& config) {
  asio::error_code ec{};
  auto result = create_connector_state(adapter, config, ec);
  asio::detail::throw_error(ec);
  return result;
}

inline nd_listener_state_ptr create_listener_state(
    nd_adapter_ptr const& adapter, nd_config_t const& config,
    asio::error_code& ec) {
  assert(is_valid_adapter(adapter));

  // create overlapped handle for notification of IO completion
  unique_handle_t overlapped_handle{};
  overlapped_handle.reset(create_overlapped_file(adapter->adapter_.Get(), ec));
  if (ec) {
    ASIO_ERROR_LOCATION(ec);
    return nullptr;
  }

  // create network-direct listener interface
  nd2_listener_ptr listener{};
  listener.Attach(
      create_listener(adapter->adapter_.Get(), overlapped_handle.get(), ec));
  if (ec) {
    ASIO_ERROR_LOCATION(ec);
    return nullptr;
  }

  ec.clear();
  auto result = std::make_shared<nd_listener_state_t>();
  result->overlapped_handle_ = std::move(overlapped_handle);
  result->listener_ = std::move(listener);
  result->config_ = config;
  result->adapter_ = adapter;
  return result;
}

inline nd_listener_state_ptr create_listener_state(
    nd_adapter_ptr const& adapter, nd_config_t const& config) {
  asio::error_code ec{};
  auto const result = create_listener_state(adapter, config, ec);
  asio::detail::throw_error(ec);
  return result;
}

inline bool is_config_valid(nd_adapter_ptr const& adapter,
                            nd_config_t const& config) {
  assert(is_valid_adapter(adapter));
  // TODO ...
  return true;
}

}