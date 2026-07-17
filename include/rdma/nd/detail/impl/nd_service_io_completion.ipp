#ifndef RDMA_ND_DETAIL_IMPL_ND_SERVICE_IO_COMPLETION_IPP
#define RDMA_ND_DETAIL_IMPL_ND_SERVICE_IO_COMPLETION_IPP

#include <ranges>

#include "rdma/nd/detail/nd_service_io_completion.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

void nd_io_completion_service::shutdown() {
  cq_.Reset();
  cq_handle_.reset();
}

// Create the shared CQ + overlapped handle on the IOCP scheduler. The device is
// used transiently for its adapter (not stored); cqe is the derived CQ depth.
void nd_io_completion_service::initialize(nd_adapter_ptr const& device,
                                          std::uint32_t cqe,
                                          std::uint32_t poll_batch,
                                          asio::error_code& ec) {
  if (cq_) {
    ec = rdma_errc::already_registered;
    ASIO_ERROR_LOCATION(ec);
    return;
  }
  if (!device || !device->adapter_) {
    ec = rdma_errc::invalid_device;
    ASIO_ERROR_LOCATION(ec);
    return;
  }

  auto handle = create_overlapped_file(device->adapter_.Get(), ec);
  if (ec) {
    ASIO_ERROR_LOCATION(ec);
    return;
  }
  cq_handle_.reset(handle);

  native_cq_init_attr cq_init_attr{
      .overlapped_handle_ = cq_handle_.get(),
      .processor_group_ = 0,
      .processor_affinity_ = 0,
  };
  cq_.Attach(verbs_ops::create_cq(device->adapter_.Get(), cqe, cq_init_attr,
                                  ec));
  if (ec) {
    cq_handle_.reset();
    ASIO_ERROR_LOCATION(ec);
    return;
  }

  scheduler_.register_handle(cq_handle_.get(), ec);
  if (ec) {
    cq_.Reset();
    cq_handle_.reset();
    ASIO_ERROR_LOCATION(ec);
    return;
  }
  wc_buf_.resize(poll_batch ? poll_batch : default_cq_poll_batch);
}

// Start the self-perpetuating CQ poller. Idempotent + thread-safe: the first
// event-mode queue_pair to bind fires it; after that it re-arms itself forever.
void nd_io_completion_service::ensure_poller_started() {
  if (!poller_started_.exchange(true, std::memory_order_acq_rel)) {
    arm_poller();
  }
}

void nd_io_completion_service::nd_poll_wc_op::do_complete(
    void* owner, base_type* base, asio::error_code const& /*result_ec*/,
    std::size_t /*bytes_transferred*/) {
  auto* o = static_cast<nd_poll_wc_op*>(base);
  nd_io_completion_service* svc = o->svc_;
  if (owner) {
    svc->poll_and_dispatch(owner);
    svc->arm_poller();  // self-perpetuate
  }
}

rdma_verbs_op_base* nd_io_completion_service::resolve_wc(
    native_wc_t const& result) {
  if (!result.RequestContext) {
    return nullptr;
  }
  auto* op = reinterpret_cast<rdma_verbs_op_base*>(result.RequestContext);
  op->ec_ = completion_status_to_error(result.Status);
  if (!op->ec_) {
    // send/write byte counts are set on the op at post time.
    if (result.RequestType != ND2_REQUEST_TYPE::Nd2RequestTypeSend &&
        result.RequestType != ND2_REQUEST_TYPE::Nd2RequestTypeWrite) {
      op->bytes_transferred_ = result.BytesTransferred;
    }
  }
  else {
    op->bytes_transferred_ = 0;
  }
  return op;
}

void nd_io_completion_service::poll_and_dispatch(void* owner) {
  asio::detail::op_queue<rdma_verbs_op_base> ops;
  ULONG n = 0;
  constexpr std::size_t max_batches_per_turn = 4;
  std::size_t batches = 0;
  do {
    n = verbs_ops::poll_cq(
        cq_.Get(), wc_buf_.data(), static_cast<size_type>(wc_buf_.size()));
    std::ranges::for_each_n(wc_buf_.begin(), n, [&](auto const& wc) {
      if (auto* op = resolve_wc(wc)) {
        ops.push(op);
      }
    });
    ++batches;
  } while (n == wc_buf_.size() && batches < max_batches_per_turn);
  while (auto* op = ops.front()) {
    ops.pop();
    op->complete(owner);
  }
}

// Arm (or re-arm) the single reusable poller.
void nd_io_completion_service::arm_poller() {
  poller_.reset_overlapped();
  asio::error_code ec;
  native_cq_notify_attr notify_attr{
      .type_ = ND_CQ_NOTIFY_ANY,
      .op_ = &poller_,
  };
  auto const hr = verbs_ops::notify_cq(cq_.Get(), notify_attr, ec);
  scheduler_.work_started();
  if (!ec && hr == ND_PENDING) {
    scheduler_.on_pending(&poller_);
  }
  else {
    scheduler_.on_completion(&poller_, ec, 0L);
  }
}

}  // namespace asio::rdma::detail

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_ND_DETAIL_IMPL_ND_SERVICE_IO_COMPLETION_IPP
