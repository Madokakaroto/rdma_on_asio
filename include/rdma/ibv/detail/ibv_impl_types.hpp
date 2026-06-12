#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include "asio/error_code.hpp"
#include "rdma/rdma_commons.hpp"

namespace asio::rdma::detail {

// buffer tags are shared (rdma/rdma_commons.hpp); keep ibv-prefixed aliases.
using ibv_const_buffer_tag = rdma_const_buffer_tag;
using ibv_mutable_buffer_tag = rdma_mutable_buffer_tag;

// portability seam: native_* aliases mirror nd/detail/nd_impl_types.hpp
using native_context_t      = ::ibv_context;
using native_pd_t           = ::ibv_pd;
using native_cq_t           = ::ibv_cq;
using native_qp_t           = ::ibv_qp;
using native_mr_t           = ::ibv_mr;
using native_sge_t          = ::ibv_sge;
using native_wc_t           = ::ibv_wc;
using native_device_attr_t  = ::ibv_device_attr;
using native_qp_init_attr_t = ::ibv_qp_init_attr;
using native_comp_channel_t = ::ibv_comp_channel;

// rdma_cm native aliases
using native_event_channel_t = ::rdma_event_channel;
using native_cm_id_t         = ::rdma_cm_id;
using native_cm_event_t      = ::rdma_cm_event;

// RAII deleter for a protection domain (mirrors deprecated ibv_pd_deleter)
struct ibv_pd_deleter {
  void operator()(native_pd_t* pd) const noexcept {
    if (pd) {
      ::ibv_dealloc_pd(pd);
    }
  }
};
using unique_ibv_pd_ptr = std::unique_ptr<native_pd_t, ibv_pd_deleter>;

struct ibv_cq_deleter {
  void operator()(native_cq_t* cq) const noexcept {
    if (cq) {
      ::ibv_destroy_cq(cq);
    }
  }
};
using unique_ibv_cq_ptr = std::unique_ptr<native_cq_t, ibv_cq_deleter>;

struct ibv_comp_channel_deleter {
  void operator()(native_comp_channel_t* channel) const noexcept {
    if (channel) {
      ::ibv_destroy_comp_channel(channel);
    }
  }
};
using unique_ibv_comp_channel_ptr =
    std::unique_ptr<native_comp_channel_t, ibv_comp_channel_deleter>;

// device abstraction (mirrors nd_adapter_t)
struct ibv_device_t {
  native_context_t* context_ = nullptr;  // raw: owned by librdmacm (valid for process lifetime)
  unique_ibv_pd_ptr pd_;                  // owned per-device; dealloc'd when this struct dies
  native_device_attr_t attr_{};           // queried device capabilities
  std::string name_;                      // ibv_get_device_name(context_->device)
};
using ibv_device_ptr = std::shared_ptr<ibv_device_t>;

// --- rdma_cm RAII deleters & holders (mirror deprecated rdma_core_types.hpp) ---

struct rdma_event_channel_deleter {
  void operator()(native_event_channel_t* channel) const noexcept {
    if (channel) {
      ::rdma_destroy_event_channel(channel);
    }
  }
};
using unique_rdma_event_channel_ptr =
    std::unique_ptr<native_event_channel_t, rdma_event_channel_deleter>;

struct rdma_cm_id_deleter {
  void operator()(native_cm_id_t* cm_id) const noexcept {
    if (cm_id) {
      ::rdma_destroy_id(cm_id);
    }
  }
};
using unique_rdma_cm_id_ptr =
    std::unique_ptr<native_cm_id_t, rdma_cm_id_deleter>;

// CM events are owned by rdma_cm; acking returns them. Use RAII to ack.
struct rdma_cm_event_deleter {
  void operator()(native_cm_event_t* event) const noexcept {
    if (event) {
      ::rdma_ack_cm_event(event);
    }
  }
};
using unique_rdma_cm_event_ptr =
    std::unique_ptr<native_cm_event_t, rdma_cm_event_deleter>;

using cm_channel_holder = unique_rdma_event_channel_ptr;
using cm_id_holder = unique_rdma_cm_id_ptr;

// Connection lifecycle, mirrored by the connect/accept op's per-stage CAS and
// used as the SOLE basis for connector::disconnect()'s teardown decision. The
// transition out of `connecting` (-> connected) is the single atomic arbitration
// point between the op (reactor thread) and disconnect() (any thread): whoever
// acts second performs the one rdma_disconnect for an established connection.
// See docs/cancellation_stage1_object.md (design A).
enum class connect_state : int {
  idle,          // op not armed yet
  addr_resolve,  // [client] resolve_addr issued, awaiting ADDR_RESOLVED
  addr_route,    // [client] resolve_route issued, awaiting ROUTE_RESOLVED
  connecting,    // rdma_connect / rdma_accept issued, awaiting ESTABLISHED
  connected,     // ESTABLISHED -- the only state where rdma_disconnect is legal
  closed,        // torn down / aborted / failed -- terminal, destroy only
};

// Upper bound for copied CM private data (transports cap this well below 256).
inline constexpr std::size_t max_private_data_size = 256;

// Cap for OUTGOING connect/accept private_data. rdma_conn_param.private_data_len
// is a uint8_t, so 255 is the hard wire limit; oversize is rejected at
// initiation (rdma_errc::private_data_too_large) rather than silently truncated.
inline constexpr std::size_t max_outgoing_private_data = 255;

// Scatter-gather list of ibv_sge with small-buffer optimization. SGE counts are
// small in practice; spill to the heap only beyond the inline capacity.
class ibv_sglist_t {
 public:
  static constexpr std::size_t inline_sge_count = 8;

  ibv_sglist_t() = default;
  ~ibv_sglist_t() { reset(); }
  ibv_sglist_t(ibv_sglist_t const&) = delete;
  ibv_sglist_t& operator=(ibv_sglist_t const&) = delete;

  void resize(std::size_t count) {
    reset();
    if (count > inline_sge_count) {
      heap_ = new native_sge_t[count];
      data_ = heap_;
    }
    else {
      data_ = inline_.data();
    }
    size_ = count;
  }

  native_sge_t* data() noexcept { return data_; }
  native_sge_t const* data() const noexcept { return data_; }
  std::size_t size() const noexcept { return size_; }
  native_sge_t& operator[](std::size_t i) noexcept { return data_[i]; }

 private:
  void reset() noexcept {
    delete[] heap_;
    heap_ = nullptr;
    data_ = nullptr;
    size_ = 0;
  }

  std::array<native_sge_t, inline_sge_count> inline_{};
  native_sge_t* heap_ = nullptr;
  native_sge_t* data_ = nullptr;
  std::size_t size_ = 0;
};

// the minimal CM resources of a connector/listener (mirrors rdma_cm_block_t)
struct ibv_cm_block_t {
  cm_channel_holder cm_channel_;  // event channel (own, nonblocking)
  cm_id_holder cm_id_;            // the rdma_cm_id bound to that channel
};

// Move-only transfer token handed from listener to a server-side connector
// (mirrors nd_connector_handle_t). Holds the child cm_id from a CONNECT_REQUEST
// after it has been migrated to its own event channel.
using ibv_connector_handle_t = ibv_cm_block_t;

// Callback the connector invokes (once cm_id has a context) to create the QP on
// that cm_id and back-fill the bound queue_pair. Returns the creation result.
using ibv_create_qp_fn = std::function<asio::error_code(native_cm_id_t*)>;

}
