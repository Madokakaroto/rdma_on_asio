#pragma once

#include <optional>

#include "asio/detail/config.hpp"  // ASIO_DECL
#include "asio/ip/address.hpp"
#include "rdma/detail/small_sglist.hpp"
#include "rdma/rdma_commons.hpp"

namespace asio::rdma::detail {

inline bool is_closable_handle(HANDLE handle) noexcept {
  return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

// raii handler
struct handle_deleter {
  void operator()(HANDLE handle) const noexcept {
    if (is_closable_handle(handle)) {
      ::CloseHandle(handle);
    }
  }
};
using unique_handle_t =
    std::unique_ptr<std::remove_pointer_t<HANDLE>, handle_deleter>;

// raii module handler
struct module_deleter {
  void operator()(HMODULE module) const {
    if (module != NULL) {
      ::FreeLibrary(module);
    }
  }
};
using unique_module_t =
    std::unique_ptr<std::remove_pointer_t<HMODULE>, module_deleter>;

// raii buffer
struct scope_buffer {
  void* buffer{nullptr};
  explicit scope_buffer(void* buffer_ptr) : buffer(buffer_ptr) {}
  ~scope_buffer() {
    if (buffer) {
      std::free(buffer);
    }
  }
};

using nd2_adapter_ptr = Microsoft::WRL::ComPtr<IND2Adapter>;
using nd2_provider_ptr = Microsoft::WRL::ComPtr<IND2Provider>;
using nd2_connector_ptr = Microsoft::WRL::ComPtr<IND2Connector>;
using nd2_listener_ptr = Microsoft::WRL::ComPtr<IND2Listener>;
using nd2_queue_pair_ptr = Microsoft::WRL::ComPtr<IND2QueuePair>;
using nd2_completion_queue_ptr = Microsoft::WRL::ComPtr<IND2CompletionQueue>;
using nd2_memory_region_ptr = Microsoft::WRL::ComPtr<IND2MemoryRegion>;
using class_factory_ptr = Microsoft::WRL::ComPtr<IClassFactory>;

using dll_can_unload_now = HRESULT (*)(void);
using dll_get_class_object = HRESULT (*)(REFCLSID rclsid, REFIID rrid,
                                         LPVOID* ppv);

// native type aliases for the { windows, network-direct } platform
using native_context_t = IND2Adapter;
using native_connector_t = IND2Connector;
using native_listener_t = IND2Listener;
using native_qp_t = IND2QueuePair;
using native_cq_t = IND2CompletionQueue;
using native_mr_t = IND2MemoryRegion;
using native_sge_t = ND2_SGE;
using native_wc_t = ND2_RESULT;
using native_context_config_t = ND2_ADAPTER_INFO;

struct native_pd_t {
  native_context_t* context_;
  unique_handle_t sync_handle_;
};

struct nd2_sockaddr_t {
  union {
    struct sockaddr src_addr_;
    struct sockaddr_in src_sin_;
    struct sockaddr_in6 src_sin6_;
    struct sockaddr_storage src_storage_;
  };
  size_t address_size_;
};

struct nd2_cq_init_attr {
  HANDLE overlapped_handle_;
  USHORT processor_group_;
  KAFFINITY processor_affinity_;
};

struct nd2_cq_notify_attr {
  ULONG type_;
  LPOVERLAPPED op_;
};

struct nd2_qp_init_attr {
  void* qp_context_;
  native_cq_t* rcq_;         // receive completion queue
  native_cq_t* icq_;         // initiator completion queue
  ULONG max_send_wr_;         // max send work requests
  ULONG max_recv_wr_;         // max recv work requests
  ULONG max_send_sge_;        // max send num of scatter/gather elements
  ULONG max_recv_sge_;        // max recv num of scatter/gather elements
  ULONG max_inline_data_;     // max payload data size in a packet
};

using native_qp_init_attr = nd2_qp_init_attr;
using native_cq_init_attr = nd2_cq_init_attr;
using native_cq_notify_attr = nd2_cq_notify_attr;

// factory type
struct nd_provider_factory_t {
  WSAPROTOCOL_INFOW proto_;
  std::wstring module_name_;
  unique_module_t module_;
  dll_can_unload_now unload_;
  class_factory_ptr factory_;
};
using nd_provider_factory_ptr = std::shared_ptr<nd_provider_factory_t>;
// adapter type -- one device == one physical adapter: a single OpenAdapter
// (one AdapterId, one PD resource domain) carrying that adapter's v4 and/or v6
// local addresses. v4/v6 are NOT separate devices. See nd_dual_family_plan.md.
struct nd_adapter_t {
  nd2_adapter_ptr adapter_;
  std::unique_ptr<native_pd_t> pd_;
  UINT64 adapter_id_ = 0;  // ResolveAddress id; same HW -> same id
  std::optional<asio::ip::address> v4_address_;
  std::optional<asio::ip::address> v6_address_;
  std::string name_;  // display: first bound address string
  native_context_config_t info_;

  ASIO_DECL asio::ip::address get_v4_address() const;
  ASIO_DECL asio::ip::address get_v6_address() const;
};
using nd_adapter_ptr = std::shared_ptr<nd_adapter_t>;
// provider types -- adapters grouped by AdapterId (one entry per physical NIC,
// each carrying its v4/v6 local addresses).
struct nd_provider_t {
  nd_provider_factory_ptr factory_;
  nd2_provider_ptr provider_;
  std::vector<nd_adapter_ptr> devices_;
};
using nd_provider_ptr = std::shared_ptr<nd_provider_t>;

// shared state for a rdma connection
struct nd_connector_state_t {
  // overlapped handle to receive IO completion
  unique_handle_t overlapped_handle_;
  // the network-direct connector interface
  nd2_connector_ptr connector_;
  // the completion queue interface to poll IO work completion
  nd2_completion_queue_ptr cq_;
  // the queue pair interface to perform verbs IO operations
  nd2_queue_pair_ptr qp_;
  // configuration to create this shared state
  nd_config_t config_;
  // device that creates this state
  nd_adapter_ptr adapter_;
};
using nd_connector_state_ptr = std::shared_ptr<nd_connector_state_t>;

// Move-only transfer token handed from listener to a server-side connector
// (mirrors ibv_connector_handle_t).
struct nd_connector_handle_t {
  nd2_connector_ptr connector_;
  unique_handle_t overlapped_handle_;
  nd_adapter_ptr adapter_;

  nd_connector_handle_t() = default;
  nd_connector_handle_t(nd_connector_handle_t&&) = default;
  nd_connector_handle_t& operator=(nd_connector_handle_t&&) = default;
  nd_connector_handle_t(nd_connector_handle_t const&) = delete;
  nd_connector_handle_t& operator=(nd_connector_handle_t const&) = delete;
};

// Upper bound for copied CM private data (mirrors ibv_impl_types.hpp).
inline constexpr std::size_t max_private_data_size = 256;

// Cap for OUTGOING connect/accept private_data (mirrors ibv: unified at 255).
// Oversize is rejected at initiation (rdma_errc::private_data_too_large).
inline constexpr std::size_t max_outgoing_private_data = 255;

// Connection lifecycle for the nd connector. Per-platform by design: ND's
// IND2Connector::Connect hides address/route resolution inside the provider, so
// nd has NONE of ibv's resolve stages -- this enum is intentionally smaller than
// the ibv connect_state. Only the TERMINAL/discarded determination (`closed`) is
// aligned across backends; intermediate states are not. See
// docs/cancellation_stage1_object.md / cancellation_stage2_control_single_op.md.
enum class connect_state : int {
  idle,        // not connecting (fresh / opened / assigned)
  connecting,  // Connect/Accept issued (one-shot: connector never returns to idle)
  connected,   // established (reserved; set once op completion is wired)
  closed,      // disconnected / failed -- terminal, discarded
};

}

namespace asio::rdma::detail {

// Scatter-gather list with inline storage for the common small-SGE path.
using nd_sglist_t = small_sglist<native_sge_t, 8>;

}
