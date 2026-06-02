#pragma once

#include "nd/nd_types.hpp"
#include "nd/nd_error.hpp"
#include "nd/nd_device.hpp"
#include "nd/detail/nd_impl_types.hpp"
#include "nd/detail/nd_ops_verbs.hpp"
#include "nd/detail/nd_op_base.hpp"
#include "nd/detail/nd_config_derive.hpp"

namespace asio::rdma {

class nd_completion_queue {
public:
  nd_completion_queue(nd_device_ptr const& device,
                      nd_config_t const& config = {})
      : device_(device) {
    assert(device && device->adapter_);
    asio::error_code ec;

    auto effective = detail::derive_effective_config(config, device->info_);

    handle_.reset(
        detail::create_overlapped_file(device->adapter_.Get(), ec));
    if (ec) {
      asio::detail::throw_error(ec);
    }

    detail::native_cq_init_attr cq_init_attr{
        .overlapped_handle_ = handle_.get(),
        .processor_group_ = 0,
        .processor_affinity_ = 0,
    };
    cq_.Attach(detail::verbs_ops::create_cq(
        device->adapter_.Get(), effective.cqe_, cq_init_attr, ec));
    if (ec) {
      handle_.reset();
      asio::detail::throw_error(ec);
    }
  }

  ~nd_completion_queue() = default;

  nd_completion_queue(nd_completion_queue const&) = delete;
  nd_completion_queue& operator=(nd_completion_queue const&) = delete;
  nd_completion_queue(nd_completion_queue&&) = default;
  nd_completion_queue& operator=(nd_completion_queue&&) = default;

  std::size_t poll() {
    asio::error_code ec;
    auto n = poll(ec);
    if (ec) {
      asio::detail::throw_error(ec);
    }
    return n;
  }

  std::size_t poll(asio::error_code& ec) {
    std::size_t total = 0;
    ULONG retrieved = 0;
    do {
      std::array<detail::native_wc_t, 16> results{};
      retrieved = detail::verbs_ops::poll_cq(cq_.Get(), results);
      for (ULONG i = 0; i < retrieved; ++i) {
        dispatch_completion(this, results[i]);
      }
      total += retrieved;
    } while (retrieved != 0);
    return total;
  }

  std::size_t poll_one() {
    asio::error_code ec;
    auto n = poll_one(ec);
    if (ec) {
      asio::detail::throw_error(ec);
    }
    return n;
  }

  std::size_t poll_one(asio::error_code& ec) {
    detail::native_wc_t result{};
    auto const retrieved = detail::verbs_ops::poll_cq(cq_.Get(), result);
    if (retrieved > 0) {
      dispatch_completion(this, result);
      return 1;
    }
    return 0;
  }

  detail::native_cq_t* native_handle() const noexcept { return cq_.Get(); }

private:
  static void dispatch_completion(void* owner, detail::native_wc_t const& wc) {
    if (!wc.RequestContext) return;
    auto* op = reinterpret_cast<detail::rdma_verbs_op_base*>(wc.RequestContext);
    op->ec_ = static_cast<nd_errc>(wc.Status);
    if (!op->ec_) {
      if (wc.RequestType != ND2_REQUEST_TYPE::Nd2RequestTypeSend &&
          wc.RequestType != ND2_REQUEST_TYPE::Nd2RequestTypeWrite) {
        op->bytes_transferred_ = wc.BytesTransferred;
      }
    } else {
      op->bytes_transferred_ = 0;
    }
    // Non-null owner so the handler upcall fires (mirrors ibv_completion_queue).
    op->complete(owner);
  }

  nd_device_ptr device_;
  detail::nd2_completion_queue_ptr cq_;
  detail::unique_handle_t handle_;
};

}
