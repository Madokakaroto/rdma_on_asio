#pragma once

#include "asio/detail/config.hpp"
#include "asio/detail/object_pool.hpp"
#include "asio/detail/io_object_impl.hpp"
#include "asio/execution_context.hpp"
#include "asio/detail/win_iocp_io_context.hpp"
#include "rdma/nd/nd_device.hpp"

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
  ASIO_DECL static asio::detail::win_iocp_io_context& use_asio_scheduler(
      asio::execution_context& context);

  ASIO_DECL explicit nd_service_base(asio::execution_context& context);

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

  ASIO_DECL void insert(base_implementation_type& impl);

  ASIO_DECL void remove(base_implementation_type& impl);

  ASIO_DECL void do_insert(base_implementation_type& impl);

  ASIO_DECL void do_remove(base_implementation_type& impl);
};

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/detail/impl/nd_service_base.ipp"
#endif
