#pragma once

#include "asio/cancellation_type.hpp"
#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "rdma/nd/nd_types.hpp"
#include "rdma/nd/nd_error.hpp"

// Interfaces similar to rdma_cm.
namespace asio::rdma::detail {

// Connector interfaces.
ASIO_DECL IND2Connector* create_connector(IND2Adapter* adapter,
                                          HANDLE overlapped_handle,
                                          asio::error_code& ec);

ASIO_DECL result_type bind_addr(IND2Connector* connector,
                                sockaddr const* addrin, std::size_t addr_size,
                                asio::error_code& ec);

ASIO_DECL result_type accept(IND2Connector* connector, IND2QueuePair* qp,
                             ULONG inbound_read_limit, ULONG outbound_read_limit,
                             void const* private_data, ULONG private_data_size,
                             OVERLAPPED* overlapped, asio::error_code& ec);

ASIO_DECL result_type connect(IND2Connector* connector, IND2QueuePair* qp,
                              sockaddr const* addrin, size_t address_size,
                              ULONG inbound_read_limit, ULONG outbound_read_limit,
                              const void* private_data, ULONG data_size,
                              OVERLAPPED* overlapped, asio::error_code& ec);

ASIO_DECL result_type disconnect(IND2Connector* connector, OVERLAPPED* overlapped,
                                 asio::error_code& ec);

// Arm a disconnect NOTIFICATION: the overlapped completes when the connection is
// disconnected (peer or self). Backs async_wait_disconnect (on_disconnect).
ASIO_DECL result_type notify_disconnect(IND2Connector* connector,
                                        OVERLAPPED* overlapped,
                                        asio::error_code& ec);

// listener interfaces
ASIO_DECL IND2Listener* create_listener(IND2Adapter* adapter,
                                        HANDLE overlapped_handle,
                                        asio::error_code& ec);

ASIO_DECL result_type listen(IND2Listener* listener, int backlog,
                             asio::error_code& ec);

ASIO_DECL result_type get_connection_request(IND2Listener* listener,
                                             IND2Connector* connector,
                                             OVERLAPPED* overlapped,
                                             asio::error_code& ec);

ASIO_DECL result_type bind_addr(IND2Listener* listener, sockaddr const* addrin,
                                std::size_t addr_size, asio::error_code& ec);

// Per-op cancellation via CancelIoEx (mirrors ibv's cancel_ops_by_key pattern).
// Stored in the handler's cancellation_slot; when the slot fires (terminal /
// partial / total), CancelIoEx aborts the specific OVERLAPPED IO.
struct nd_cm_op_cancellation {
  HANDLE handle_;
  LPOVERLAPPED op_;

  void operator()(asio::cancellation_type_t type) {
    if (!!(type & (asio::cancellation_type::terminal |
                   asio::cancellation_type::partial |
                   asio::cancellation_type::total))) {
      ::CancelIoEx(handle_, op_);
    }
  }
};

template <typename CancellationSlot>
void arm_nd_cancellation(CancellationSlot slot, HANDLE handle,
                         LPOVERLAPPED op) {
  if (slot.is_connected()) {
    slot.template emplace<nd_cm_op_cancellation>(
        nd_cm_op_cancellation{handle, op});
  }
}

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/detail/impl/nd_ops_cm.ipp"
#endif
