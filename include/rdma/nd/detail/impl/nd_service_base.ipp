#ifndef RDMA_ND_DETAIL_IMPL_ND_SERVICE_BASE_IPP
#define RDMA_ND_DETAIL_IMPL_ND_SERVICE_BASE_IPP

#include "rdma/nd/detail/nd_service_base.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

asio::detail::win_iocp_io_context& nd_service_base::use_asio_scheduler(
    asio::execution_context& context) {
  return asio::use_service<asio::detail::win_iocp_io_context>(context);
}

nd_service_base::nd_service_base(asio::execution_context& context)
    : scheduler_(use_asio_scheduler(context))
    , mutex_()
    , impl_list_(nullptr) {
}

void nd_service_base::base_construct(base_implementation_type& impl) {
  asio::detail::mutex::scoped_lock lock(mutex_);
  do_insert(impl);
}

void nd_service_base::base_move_construct(base_implementation_type& impl,
                                          base_implementation_type& other_impl) {
  asio::detail::mutex::scoped_lock lock(mutex_);
  do_insert(impl);
}

void nd_service_base::base_destroy(base_implementation_type& impl) {
  asio::detail::mutex::scoped_lock lock(mutex_);
  do_remove(impl);
}

void nd_service_base::insert(base_implementation_type& impl) {
  asio::detail::mutex::scoped_lock lock(mutex_);
  do_insert(impl);
}

void nd_service_base::remove(base_implementation_type& impl) {
  asio::detail::mutex::scoped_lock lock(mutex_);
  do_remove(impl);
}

void nd_service_base::do_insert(base_implementation_type& impl) {
  impl.next_ = impl_list_;
  impl.prev_ = nullptr;
  if (impl_list_) {
    impl_list_->prev_ = &impl;
  }
  impl_list_ = &impl;
}

void nd_service_base::do_remove(base_implementation_type& impl) {
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

#endif  // RDMA_ND_DETAIL_IMPL_ND_SERVICE_BASE_IPP
