#ifndef RDMA_IBV_IMPL_IBV_SERVICE_BASE_IPP
#define RDMA_IBV_IMPL_IBV_SERVICE_BASE_IPP

#include "rdma/ibv/detail/ibv_service_base.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

ibv_service_base::ibv_service_base(asio::execution_context& context)
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

void ibv_service_base::base_construct(base_implementation_type& impl) {
  asio::detail::mutex::scoped_lock lock(mutex_);
  do_insert(impl);
}

void ibv_service_base::base_move_construct(base_implementation_type& impl,
                                           base_implementation_type& /*other_impl*/) {
  asio::detail::mutex::scoped_lock lock(mutex_);
  do_insert(impl);
}

void ibv_service_base::base_destroy(base_implementation_type& impl) {
  asio::detail::mutex::scoped_lock lock(mutex_);
  do_remove(impl);
}

void ibv_service_base::do_insert(base_implementation_type& impl) {
  impl.next_ = impl_list_;
  impl.prev_ = nullptr;
  if (impl_list_) {
    impl_list_->prev_ = &impl;
  }
  impl_list_ = &impl;
}

void ibv_service_base::do_remove(base_implementation_type& impl) {
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

}  // namespace asio::rdma::detail

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_IBV_IMPL_IBV_SERVICE_BASE_IPP
