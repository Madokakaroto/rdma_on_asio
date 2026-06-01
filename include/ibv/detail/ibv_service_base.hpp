#pragma once

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

  explicit ibv_service_base(asio::execution_context& context)
      : reactor_(asio::use_service<asio::detail::reactor>(context))
      , scheduler_(asio::use_service<asio::detail::scheduler>(context))
      , mutex_()
      , impl_list_(nullptr)
      , success_ec_() {
    // Install the reactor as the scheduler's task (epoll loop). asio's own
    // reactive service bases do this; without it the scheduler parks on its
    // condition variable and never calls epoll_wait.
    reactor_.init_task();
  }

  void base_construct(base_implementation_type& impl) {
    asio::detail::mutex::scoped_lock lock(mutex_);
    do_insert(impl);
  }

  void base_move_construct(base_implementation_type& impl,
                           base_implementation_type& /*other_impl*/) {
    asio::detail::mutex::scoped_lock lock(mutex_);
    do_insert(impl);
  }

  void base_destroy(base_implementation_type& impl) {
    asio::detail::mutex::scoped_lock lock(mutex_);
    do_remove(impl);
  }

  template <typename ImplType, typename Destroyer>
  void base_shutdown(Destroyer const& destroyer) {
    asio::detail::mutex::scoped_lock lock(mutex_);
    base_implementation_type* impl = impl_list_;
    while (impl) {
      destroyer(static_cast<ImplType&>(*impl));
      impl = impl->next_;
    }
  }

  void do_insert(base_implementation_type& impl) {
    impl.next_ = impl_list_;
    impl.prev_ = nullptr;
    if (impl_list_) {
      impl_list_->prev_ = &impl;
    }
    impl_list_ = &impl;
  }

  void do_remove(base_implementation_type& impl) {
    if (impl_list_ == &impl) {
      impl_list_ = impl.next_;
    }
    if (impl.prev_) {
      impl.prev_->next_ = impl.next_;
    }
    if (impl.next_) {
      impl.next_->prev_ = impl.prev_;
    }
    impl.next_ = nullptr;
    impl.prev_ = nullptr;
  }
};

}
