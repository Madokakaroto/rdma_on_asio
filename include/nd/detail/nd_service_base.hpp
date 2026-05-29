#pragma once

#include "asio/detail/config.hpp"
#include "asio/detail/object_pool.hpp"
#include "asio/detail/io_object_impl.hpp"
#include "asio/execution_context.hpp"
#include "asio/detail/win_iocp_io_context.hpp"
#include "nd/nd_device.hpp"

namespace asio::rdma::detail {

// base class of network direct service
class nd_service_base {
public:
 struct base_implementation_type {
  base_implementation_type* next_;
  base_implementation_type* prev_;
};

protected:
  asio::detail::win_iocp_io_context& scheduler_;
  asio::detail::mutex mutex_;
  base_implementation_type* impl_list_;

protected:
  static inline asio::detail::win_iocp_io_context& use_asio_scheduler(
      asio::execution_context& context) {
    return asio::use_service<asio::detail::win_iocp_io_context>(context);
  }

  explicit nd_service_base(asio::execution_context& context)
      : scheduler_(use_asio_scheduler(context))
      , mutex_()
      , impl_list_(nullptr) {
  }

  void base_construct(base_implementation_type& impl) {
    asio::detail::mutex::scoped_lock lock(mutex_);
    do_insert(impl);
  }

  void base_move_construct(base_implementation_type& impl,
                           base_implementation_type& other_impl) {
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

  void insert(base_implementation_type& impl) {
    asio::detail::mutex::scoped_lock lock(mutex_);
    do_insert(impl);
  }

  void remove(base_implementation_type& impl) {
    asio::detail::mutex::scoped_lock lock(mutex_);
    do_remove(impl);
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