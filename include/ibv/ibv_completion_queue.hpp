#pragma once

#include <array>
#include <cassert>

#include "ibv/ibv_device.hpp"
#include "ibv/ibv_error.hpp"
#include "ibv/ibv_types.hpp"
#include "ibv/detail/ibv_config_derive.hpp"
#include "ibv/detail/ibv_impl_types.hpp"
#include "ibv/detail/ibv_op_cq_notify.hpp"
#include "ibv/detail/ibv_ops_verbs.hpp"

namespace asio::rdma {

// Standalone completion queue (mirrors nd_completion_queue). Poll mode: the user
// drives poll()/poll_one() and handlers fire synchronously; no io_context
// involvement. The QP is opened against this CQ via ibv_queue_pair(io, cq, ...).
class ibv_completion_queue {
public:
  static constexpr int poll_batch = 16;

  explicit ibv_completion_queue(ibv_device_ptr const& device,
                                ibv_config_t const& config = {})
      : device_(device) {
    assert(device && device->context_);
    asio::error_code ec;
    auto effective = detail::derive_effective_config(config, device->attr_);
    // Poll mode: no comp_channel (no event-driven notification).
    cq_.reset(detail::verbs_ops::create_cq(
        device->context_, static_cast<int>(effective.cqe_), nullptr, nullptr,
        0, ec));
    asio::detail::throw_error(ec);
  }

  ~ibv_completion_queue() = default;
  ibv_completion_queue(ibv_completion_queue const&) = delete;
  ibv_completion_queue& operator=(ibv_completion_queue const&) = delete;
  ibv_completion_queue(ibv_completion_queue&&) = default;
  ibv_completion_queue& operator=(ibv_completion_queue&&) = default;

  std::size_t poll() {
    std::size_t total = 0;
    int n = 0;
    do {
      std::array<detail::native_wc_t, poll_batch> wcs{};
      n = detail::verbs_ops::poll_cq(cq_.get(), poll_batch, wcs.data());
      for (int i = 0; i < n; ++i) {
        dispatch(wcs[i]);
      }
      total += static_cast<std::size_t>(n > 0 ? n : 0);
    } while (n > 0);
    return total;
  }

  std::size_t poll_one() {
    detail::native_wc_t wc{};
    int const n = detail::verbs_ops::poll_cq(cq_.get(), 1, &wc);
    if (n > 0) {
      dispatch(wc);
      return 1;
    }
    return 0;
  }

  detail::native_cq_t* native_handle() const noexcept { return cq_.get(); }

private:
  void dispatch(detail::native_wc_t const& wc) {
    if (!wc.wr_id) {
      return;
    }
    auto* op = reinterpret_cast<detail::rdma_verbs_op_base*>(wc.wr_id);
    if (wc.status == IBV_WC_SUCCESS) {
      if (wc.opcode != IBV_WC_SEND && wc.opcode != IBV_WC_RDMA_WRITE) {
        op->bytes_transferred_ = wc.byte_len;
      }
    }
    else {
      op->bytes_transferred_ = 0;
      op->ec_ = detail::wc_status_to_ec(wc.status);
    }
    // Non-null owner so the handler upcall fires (nd passed nullptr here).
    op->complete(this);
  }

  ibv_device_ptr device_;
  detail::unique_ibv_cq_ptr cq_;
};

}
