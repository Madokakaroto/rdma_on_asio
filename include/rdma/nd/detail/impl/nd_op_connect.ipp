#ifndef RDMA_ND_DETAIL_IMPL_ND_OP_CONNECT_IPP
#define RDMA_ND_DETAIL_IMPL_ND_OP_CONNECT_IPP

#include <algorithm>
#include <atomic>

#include "rdma/nd/detail/nd_op_connect.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

nd_connect_op_base::nd_connect_op_base(IND2Connector* connector,
                                       std::atomic<connect_state>* state,
                                       asio::mutable_buffer reply,
                                       func_type complete_func)
   : nd_op_base(connector, &nd_connect_op_base::do_process, complete_func)
   , stage_(stage_t::connecting)
   , state_(state)
   , reply_(reply) {
}

nd_connect_op_base::status_t nd_connect_op_base::do_process(
    void* owner, nd_op_base* base, asio::error_code& ec) {
  auto* o = static_cast<nd_connect_op_base*>(base);
  switch (o->stage_) {
    case stage_t::connecting:
      return o->process_complete_connect(owner, ec);
    case stage_t::connected:
      if (!o->mark_connected(ec)) {
        return status_t::completed;
      }
      o->stage_ = stage_t::done;
      return status_t::completed;
    default:
      ec = rdma_errc::invalid_handle;
      o->stage_ = stage_t::error;
      return status_t::completed;
  }
}

nd_connect_op_base::status_t nd_connect_op_base::process_complete_connect(
    void* owner, asio::error_code& ec) {
  // ND exposes the accept/reject private data after Connect completes and
  // before CompleteConnect is called.
  capture_remote_pd();
  this->reset();
  auto const hr = get_connector()->CompleteConnect(this);
  if (FAILED(hr)) {
    ec = static_cast<nd_errc>(hr);
    if (state_) state_->store(connect_state::closed, std::memory_order_release);
    stage_ = stage_t::error;
    return status_t::completed;
  }

  stage_ = stage_t::connected;
  if (hr == ND_PENDING) {
    return status_t::continuation;
  }

  if (!mark_connected(ec)) {
    return status_t::completed;
  }
  stage_ = stage_t::done;
  return status_t::completed;
}

bool nd_connect_op_base::mark_connected(asio::error_code& ec) {
  if (state_) {
    connect_state expected = connect_state::connecting;
    if (!state_->compare_exchange_strong(
            expected, connect_state::connected,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
      ec = asio::error::operation_aborted;
      stage_ = stage_t::error;
      return false;
    }
  }
  return true;
}

void nd_connect_op_base::capture_remote_pd() {
  if (!reply_.data() || reply_.size() == 0) {
    return;
  }
  ULONG pd_size = static_cast<ULONG>(reply_.size());
  auto const hr = get_connector()->GetPrivateData(reply_.data(), &pd_size);
  if (SUCCEEDED(hr) || hr == ND_BUFFER_OVERFLOW) {
    reply_len_ = (std::min)(static_cast<std::size_t>(pd_size),
                            reply_.size());
  }
}

}

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_ND_DETAIL_IMPL_ND_OP_CONNECT_IPP
