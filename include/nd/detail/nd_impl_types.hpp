#pragma once

namespace asio::rdma::detail {

struct nd_const_buffer_tag{};
struct nd_mutable_buffer_tag{};

// raii handler
struct handle_deleter {
  void operator()(HANDLE handle) const {
    if (handle != INVALID_HANDLE_VALUE || handle != NULL) {
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
// adapter type
struct nd_adapter_t {
  nd2_adapter_ptr adapter_;
  std::unique_ptr<native_pd_t> pd_;
  std::string name_;
  native_context_config_t info_;
};
using nd_adapter_ptr = std::shared_ptr<nd_adapter_t>;
// provider types
struct nd_provider_t {
  nd_provider_factory_ptr factory_;
  nd2_provider_ptr provider_;
  std::vector<nd_adapter_ptr> v4_adapters_;
  std::vector<nd_adapter_ptr> v6_adapters_;
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


}

namespace asio::rdma::detail {

// scatter/gather list type
// TODO ... just use the inplace_vector in C++26
class nd_sglist_t {
 public:
  static constexpr size_t inline_sge_count = 4;
  using inline_storage_type = std::array<native_sge_t, inline_sge_count>;
  using value_type = inline_storage_type::value_type;
  using pointer = inline_storage_type::pointer;
  using size_type = inline_storage_type::size_type;
  using reference = inline_storage_type::reference;
  using const_reference = inline_storage_type::const_reference;
  using iterator = inline_storage_type::iterator;
  using const_iterator = inline_storage_type::const_iterator;

 private:
  inline_storage_type storage_;
  pointer sglist_;
  size_type sge_count_;

 public:
  explicit nd_sglist_t(size_type sge_count)
      : storage_(),
        sglist_(initiate_sglist(sge_count)),
        sge_count_(sge_count) {}

  nd_sglist_t() : nd_sglist_t(0) {}
  ~nd_sglist_t() { destroy(); }

  nd_sglist_t(nd_sglist_t const& other)
      : storage_(other.storage_),
        sglist_(initiate_sglist(other.sge_count_)),
        sge_count_(other.sge_count_) {
    if (sge_count_ > inline_sge_count) {
      std::memcpy(sglist_, other.sglist_, sizeof(native_sge_t) * sge_count_);
    }
  }

  nd_sglist_t& operator=(nd_sglist_t const& other) {
    nd_sglist_t temp{other};
    *this = std::move(temp);
    return *this;
  };

  nd_sglist_t(nd_sglist_t&& other) noexcept
      : storage_(std::move(other.storage_)),
        sglist_(other.sglist_),
        sge_count_(other.sge_count_) {
    other.sglist_ = nullptr;
    other.sge_count_ = 0;
  }

  nd_sglist_t& operator=(nd_sglist_t&& other) {
    destroy();

    storage_ = std::move(other.storage_);
    sge_count_ = other.sge_count_;
    if (sge_count_ > inline_sge_count) {
      sglist_ = other.sglist_;
    }
    else {
      sglist_ = storage_.data();
    }

    other.sglist_ = nullptr;
    other.sge_count_ = 0;
    return *this;
  };

 public:
  reference operator[](size_type index) {
    assert(index < sge_count_);
    return sglist_[index];
  }

  const_reference operator[](size_type index) const {
    assert(index < sge_count_);
    return sglist_[index];
  }

  iterator begin() noexcept { return storage_.begin(); }
  iterator end() noexcept { return begin() + sge_count_; }
  const_iterator cbegin() const noexcept { return storage_.cbegin(); }
  const_iterator cend() const noexcept { return cbegin() + sge_count_; }
  pointer data() const noexcept { return sglist_; }
  size_type size() const noexcept { return sge_count_; }
  size_type total_buffer_size() const noexcept {
    return std::reduce(
        cbegin(), cend(), size_type{0},
        [](size_type acc, const_reference operand) {
          return acc + operand.BufferLength;
        });
  }

  void resize(size_t size) {
    if (size > storage_.size() && size > sge_count_) {
      sglist_ = reinterpret_cast<native_sge_t*>(
          std::realloc(sglist_, size * sizeof(native_sge_t)));
      assert(sglist_);
    }
    sge_count_ = size;
  }

 private:
  native_sge_t* initiate_sglist(size_type sge_count) {
    if (sge_count <= inline_sge_count) {
      return storage_.data();
    }
    else {
      auto* sglist = reinterpret_cast<native_sge_t*>(
          std::malloc(sge_count * sizeof(native_sge_t)));
      std::memset(sglist, 0, sge_count * sizeof(native_sge_t));
      return sglist;
    }
  }

  void destroy() {
    if (sge_count_ > inline_sge_count) {
      if (sglist_) {
        std::free(sglist_);
      }
    }
  }
};



}