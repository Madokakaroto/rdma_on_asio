#pragma once

#include "asio/detail/bind_handler.hpp"
#include "asio/detail/fenced_block.hpp"
#include "asio/detail/handler_alloc_helpers.hpp"
#include "asio/detail/handler_work.hpp"
#include "asio/detail/memory.hpp"
#include "nd/detail/nd_op_base.hpp"
#include "nd/detail/nd_connector_service.hpp"
#include <span>
#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail {

template <typename Handler, typename IoExecutor>
class nd_get_connection_request_op final : public nd_op_base {
public:
  ASIO_DEFINE_HANDLER_PTR(nd_get_connection_request_op);

  nd_get_connection_request_op(IND2Listener* listener,
                               nd_connector_handle_t&& connector_handle,
                               Handler& handler, const IoExecutor& io_ex)
      : nd_op_base(listener, &nd_op_base::default_process,
                   &nd_get_connection_request_op::do_complete)
      , connector_handle_(std::move(connector_handle))
      , handler_(ASIO_MOVE_CAST(Handler)(handler))
      , work_(handler_, io_ex) {
  }

  nd_connector_handle_t& get_connector_handle() {
    return connector_handle_;
  }

private:
  nd_connector_handle_t connector_handle_;
  Handler handler_;
  asio::detail::handler_work<Handler, IoExecutor> work_;

  static void do_complete(void* owner, asio::detail::operation* base,
                          const asio::error_code& result_ec,
                          std::size_t /*bytes_transferred*/) {
    asio::error_code ec = result_ec;
    auto* o = static_cast<nd_get_connection_request_op*>(base);

    // Retrieve private data from the connector
    std::span<const std::byte> private_data;
    ULONG pd_size = 0;
    if (!ec && o->connector_handle_.connector_) {
      auto* connector = o->connector_handle_.connector_.Get();
      void const* pd_ptr = nullptr;
      auto const hr = connector->GetPrivateData(&pd_ptr, &pd_size);
      if (SUCCEEDED(hr) && pd_ptr && pd_size > 0) {
        private_data = std::span<const std::byte>(
            static_cast<const std::byte*>(pd_ptr), pd_size);
      }
    }

    nd_connector_handle_t connector = std::move(o->connector_handle_);

    ptr p = {asio::detail::addressof(o->handler_), o, o};

    ASIO_HANDLER_COMPLETION((*o));

    asio::detail::handler_work<Handler, IoExecutor> w(
        ASIO_MOVE_CAST2(asio::detail::handler_work<Handler, IoExecutor>)(
            o->work_));

    ASIO_ERROR_LOCATION(ec);

    Handler handler(ASIO_MOVE_CAST(Handler)(o->handler_));
    p.h = asio::detail::addressof(handler);
    p.reset();

    if (owner) {
      asio::detail::fenced_block b(asio::detail::fenced_block::half);
      ASIO_HANDLER_INVOCATION_BEGIN((ec));
      handler(ec, std::move(connector), private_data);
      ASIO_HANDLER_INVOCATION_END;
    }
  }
};

}

#include "asio/detail/pop_options.hpp"
