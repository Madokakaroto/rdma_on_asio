#pragma once

#include <utility>

#include "asio/associator.hpp"
#include "asio/error_code.hpp"

namespace asio::rdma::detail {

// Completion adapter used by move-accept. The queue pair is held directly in
// the final native accept operation's handler storage. On failure an empty QP is
// returned; on success ownership is transferred to the user's handler.
template <typename QueuePair, typename Handler>
struct move_accept_handler {
  QueuePair queue_pair_;
  Handler handler_;

  QueuePair& queue_pair() noexcept { return queue_pair_; }

  void operator()(asio::error_code ec) {
    if (ec) {
      queue_pair_ = QueuePair{};
    }
    std::move(handler_)(ec, std::move(queue_pair_));
  }
};

}  // namespace asio::rdma::detail

namespace asio {

template <template <typename, typename> class Associator, typename QueuePair,
          typename Handler, typename Default>
struct associator<
    Associator,
    asio::rdma::detail::move_accept_handler<QueuePair, Handler>, Default>
    : Associator<Handler, Default> {
  using adapter_type =
      asio::rdma::detail::move_accept_handler<QueuePair, Handler>;

  static typename Associator<Handler, Default>::type get(
      adapter_type const& adapter) noexcept {
    return Associator<Handler, Default>::get(adapter.handler_);
  }

  static auto get(adapter_type const& adapter, Default const& value) noexcept
      -> decltype(Associator<Handler, Default>::get(adapter.handler_, value)) {
    return Associator<Handler, Default>::get(adapter.handler_, value);
  }
};

}  // namespace asio
