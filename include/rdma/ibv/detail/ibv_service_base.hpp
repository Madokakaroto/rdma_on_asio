#pragma once

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "asio/detail/mutex.hpp"
#include "asio/detail/reactor.hpp"
#include "asio/detail/scheduler.hpp"
#include "asio/execution_context.hpp"

namespace asio::rdma::detail {

// Base for the ibv connector/listener services (mirrors nd_service_base, but
// uses asio's epoll reactor instead of IOCP). Holds the reactor + scheduler
// references and an intrusive list of live implementations for shutdown.
class ibv_service_base {
public:
  struct base_implementation_type {
    base_implementation_type* next_;
    base_implementation_type* prev_;
  };

protected:
  asio::detail::reactor& reactor_;
  asio::detail::scheduler& scheduler_;
  asio::detail::mutex mutex_;
  base_implementation_type* impl_list_;
  // Baseline success ec passed to reactor_op ctors.
  asio::error_code success_ec_;

  ASIO_DECL explicit ibv_service_base(asio::execution_context& context);

  ASIO_DECL void base_construct(base_implementation_type& impl);

  ASIO_DECL void base_move_construct(base_implementation_type& impl,
                                     base_implementation_type& other_impl);

  ASIO_DECL void base_destroy(base_implementation_type& impl);

  template <typename ImplType, typename Destroyer>
  void base_shutdown(Destroyer const& destroyer) {
    asio::detail::mutex::scoped_lock lock(mutex_);
    base_implementation_type* impl = impl_list_;
    while (impl) {
      destroyer(static_cast<ImplType&>(*impl));
      impl = impl->next_;
    }
  }

  ASIO_DECL void do_insert(base_implementation_type& impl);

  ASIO_DECL void do_remove(base_implementation_type& impl);
};

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/ibv/detail/impl/ibv_service_base.ipp"
#endif
