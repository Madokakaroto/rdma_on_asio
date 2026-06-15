#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wrl/client.h>

#include "ndsupport.h"
#include "rdma_bench_common.hpp"

using Microsoft::WRL::ComPtr;
using clock_type = std::chrono::steady_clock;

struct nd_error : std::runtime_error {
  HRESULT hr;
  nd_error(std::string what, HRESULT value)
      : std::runtime_error(std::move(what)), hr(value) {}
};

std::string hr_hex(HRESULT hr) {
  std::ostringstream os;
  os << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
  return os.str();
}

void check_hr(HRESULT hr, char const* what) {
  if (FAILED(hr)) throw nd_error(what, hr);
}

struct winsock_runtime {
  winsock_runtime() {
    WSADATA data{};
    int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) throw std::runtime_error("WSAStartup failed");
  }
  ~winsock_runtime() { ::WSACleanup(); }
};

struct nd_runtime {
  nd_runtime() { check_hr(::NdStartup(), "NdStartup failed"); }
  ~nd_runtime() { ::NdCleanup(); }
};

struct unique_handle {
  HANDLE handle = nullptr;
  unique_handle() = default;
  explicit unique_handle(HANDLE h) : handle(h) {}
  ~unique_handle() {
    if (handle) ::CloseHandle(handle);
  }
  unique_handle(unique_handle const&) = delete;
  unique_handle& operator=(unique_handle const&) = delete;
  unique_handle(unique_handle&& other) noexcept : handle(other.handle) {
    other.handle = nullptr;
  }
  unique_handle& operator=(unique_handle&& other) noexcept {
    if (this != &other) {
      if (handle) ::CloseHandle(handle);
      handle = other.handle;
      other.handle = nullptr;
    }
    return *this;
  }
  operator HANDLE() const noexcept { return handle; }
};

OVERLAPPED make_overlapped() {
  OVERLAPPED ov{};
  ov.hEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!ov.hEvent) throw std::runtime_error("CreateEvent failed");
  return ov;
}

void close_overlapped(OVERLAPPED& ov) {
  if (ov.hEvent) {
    ::CloseHandle(ov.hEvent);
    ov.hEvent = nullptr;
  }
}

struct overlapped_holder {
  OVERLAPPED ov;
  overlapped_holder() : ov(make_overlapped()) {}
  ~overlapped_holder() { close_overlapped(ov); }
  OVERLAPPED* get() noexcept { return &ov; }
};

sockaddr_in make_ipv4_endpoint(std::string const& address, std::uint16_t port) {
  sockaddr_in out{};
  out.sin_family = AF_INET;
  out.sin_port = htons(port);
  if (::InetPtonA(AF_INET, address.c_str(), &out.sin_addr) != 1) {
    throw std::runtime_error("expected an IPv4 address: " + address);
  }
  return out;
}

void wait_overlapped(IND2Overlapped* object, OVERLAPPED& ov,
                     HRESULT initial, char const* what) {
  if (initial == ND_PENDING) {
    initial = object->GetOverlappedResult(&ov, TRUE);
  }
  check_hr(initial, what);
}

struct op_context {
  bool done = false;
  HRESULT status = ND_SUCCESS;
  ULONG bytes = 0;

  void reset_for_post() noexcept {
    done = false;
    status = ND_SUCCESS;
    bytes = 0;
  }
};

struct remote_info {
  std::uint64_t addr = 0;
  std::uint32_t token = 0;
  std::uint32_t reserved = 0;
};

std::string native_scenario_name(rdma_bench::options const& opt,
                                 std::string_view role) {
  std::string name = "native_nd_";
  name += rdma_bench::operation_name(opt.operation);
  name += "_";
  name += rdma_bench::metric_name(opt.metric);
  name += "_";
  name += role;
  return name;
}

ULONG checked_message_size(rdma_bench::options const& opt) {
  if (opt.message_size >
      static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())) {
    throw std::invalid_argument("--message-size exceeds NetworkDirect ULONG");
  }
  return static_cast<ULONG>(opt.message_size);
}

std::size_t native_slot_count(rdma_bench::options const& opt,
                              std::uint64_t limit = (std::numeric_limits<std::uint64_t>::max)()) {
  auto capped = (std::min)(static_cast<std::uint64_t>(opt.queue_depth), limit);
  capped = (std::min)(capped, opt.iterations);
  return static_cast<std::size_t>((std::max<std::uint64_t>)(1, capped));
}

std::size_t native_offset_for_slot(rdma_bench::options const& opt,
                                   std::size_t slot) {
  return slot * opt.message_size;
}

remote_info remote_for_slot(remote_info base, rdma_bench::options const& opt,
                            std::size_t slot) {
  base.addr += static_cast<std::uint64_t>(native_offset_for_slot(opt, slot));
  return base;
}

void signal_ready(std::promise<void>* ready) {
  if (!ready) return;
  try {
    ready->set_value();
  } catch (std::future_error const&) {
  }
}

class nd_session {
public:
  explicit nd_session(sockaddr_in const& local) {
    check_hr(::NdOpenAdapter(IID_IND2Adapter,
                             reinterpret_cast<sockaddr const*>(&local),
                             sizeof(local),
                             reinterpret_cast<void**>(adapter_.GetAddressOf())),
             "NdOpenAdapter failed");
    HANDLE file = nullptr;
    check_hr(adapter_->CreateOverlappedFile(&file),
             "IND2Adapter::CreateOverlappedFile failed");
    file_ = unique_handle(file);

    info_.InfoVersion = ND_VERSION_2;
    ULONG info_size = sizeof(info_);
    check_hr(adapter_->Query(&info_, &info_size), "IND2Adapter::Query failed");
  }

  void create_cq_and_qp(std::uint32_t queue_depth) {
    ULONG depth = std::max<ULONG>(queue_depth + 8, 16);
    depth = std::min(depth, info_.MaxCompletionQueueDepth);
    ULONG recv_depth = std::min<ULONG>(queue_depth + 4,
                                       info_.MaxReceiveQueueDepth);
    ULONG send_depth = std::min<ULONG>(queue_depth + 4,
                                       info_.MaxInitiatorQueueDepth);
    if (depth == 0 || recv_depth == 0 || send_depth == 0) {
      throw std::runtime_error("ND adapter reports zero queue depth");
    }
    cq_depth_ = depth;
    recv_depth_ = recv_depth;
    send_depth_ = send_depth;
    check_hr(adapter_->CreateCompletionQueue(
                 IID_IND2CompletionQueue, file_, depth, 0, 0,
                 reinterpret_cast<void**>(cq_.GetAddressOf())),
             "IND2Adapter::CreateCompletionQueue failed");
    check_hr(adapter_->CreateQueuePair(
                 IID_IND2QueuePair, cq_.Get(), cq_.Get(), nullptr,
                 recv_depth, send_depth, 1, 1, 0,
                 reinterpret_cast<void**>(qp_.GetAddressOf())),
             "IND2Adapter::CreateQueuePair failed");
  }

  void create_connector() {
    check_hr(adapter_->CreateConnector(
                 IID_IND2Connector, file_,
                 reinterpret_cast<void**>(connector_.GetAddressOf())),
             "IND2Adapter::CreateConnector failed");
  }

  void create_listener() {
    check_hr(adapter_->CreateListener(
                 IID_IND2Listener, file_,
                 reinterpret_cast<void**>(listener_.GetAddressOf())),
             "IND2Adapter::CreateListener failed");
  }

  void create_and_register_mr(void* data, std::size_t size, ULONG flags) {
    check_hr(adapter_->CreateMemoryRegion(
                 IID_IND2MemoryRegion, file_,
                 reinterpret_cast<void**>(mr_.GetAddressOf())),
             "IND2Adapter::CreateMemoryRegion failed");
    overlapped_holder ov;
    HRESULT hr = mr_->Register(data, size, flags, ov.get());
    wait_overlapped(mr_.Get(), ov.ov, hr, "IND2MemoryRegion::Register failed");
  }

  ND2_SGE sge(void* data, ULONG size) const {
    ND2_SGE out{};
    out.Buffer = data;
    out.BufferLength = size;
    out.MemoryRegionToken = mr_->GetLocalToken();
    return out;
  }

  void post_recv(op_context& ctx, ND2_SGE const& sge) {
    ctx.reset_for_post();
    check_hr(qp_->Receive(&ctx, &sge, 1), "IND2QueuePair::Receive failed");
  }

  void post_send(op_context& ctx, ND2_SGE const& sge) {
    ctx.reset_for_post();
    check_hr(qp_->Send(&ctx, &sge, 1, 0), "IND2QueuePair::Send failed");
  }

  void post_write(op_context& ctx, ND2_SGE const& sge, remote_info remote) {
    ctx.reset_for_post();
    check_hr(qp_->Write(&ctx, &sge, 1, remote.addr, remote.token, 0),
             "IND2QueuePair::Write failed");
  }

  void post_read(op_context& ctx, ND2_SGE const& sge, remote_info remote) {
    ctx.reset_for_post();
    check_hr(qp_->Read(&ctx, &sge, 1, remote.addr, remote.token, 0),
             "IND2QueuePair::Read failed");
  }

  void wait_for(op_context& expected) {
    wait_for_on(cq_.Get(), expected);
  }

  void wait_for_on(IND2CompletionQueue* cq, op_context& expected) {
    while (!expected.done) {
      ND2_RESULT result{};
      ULONG n = cq->GetResults(&result, 1);
      if (n == 0) {
        ::SwitchToThread();
        continue;
      }
      auto* ctx = static_cast<op_context*>(result.RequestContext);
      if (!ctx) throw std::runtime_error("ND completion without context");
      ctx->status = result.Status;
      ctx->bytes = result.BytesTransferred;
      ctx->done = true;
      check_hr(ctx->status, "ND work request failed");
    }
  }

  ComPtr<IND2Adapter> adapter_;
  unique_handle file_;
  ND2_ADAPTER_INFO info_{};
  ComPtr<IND2CompletionQueue> cq_;
  ComPtr<IND2QueuePair> qp_;
  ComPtr<IND2Connector> connector_;
  ComPtr<IND2Listener> listener_;
  ComPtr<IND2MemoryRegion> mr_;
  ULONG cq_depth_ = 0;
  ULONG recv_depth_ = 0;
  ULONG send_depth_ = 0;
};

char fill_byte(std::size_t index) {
  return static_cast<char>((index * 29 + 11) & 0x7f);
}

void fill_payload(std::vector<char>& storage, std::size_t offset,
                  std::size_t length) {
  for (std::size_t i = 0; i < length; ++i) {
    storage[offset + i] = fill_byte(i);
  }
}

rdma_bench::result make_native_error(rdma_bench::options const& opt,
                                     std::string const& cmd,
                                     std::string message) {
  auto r = rdma_bench::make_base_result(opt, cmd);
  r.backend = "nd";
  r.baseline = "native_nd";
  r.errors = 1;
  r.first_error = std::move(message);
  r.exit_code = 1;
  return r;
}

std::string exception_message(std::exception const& e) {
  auto const* nd = dynamic_cast<nd_error const*>(&e);
  if (!nd) return e.what();
  return std::string(e.what()) + " (" + hr_hex(nd->hr) + ")";
}

rdma_bench::result run_native_read_write_server(rdma_bench::options opt,
                                                std::string cmd,
                                                std::promise<void>* ready) {
  opt.backend = "nd";
  opt.baseline = "native_nd";
  auto result = rdma_bench::make_base_result(opt, cmd);
  result.scenario_name = native_scenario_name(opt, "server");

  try {
    sockaddr_in local = make_ipv4_endpoint(opt.local_addr, opt.port);
    nd_session s(local);
    s.create_cq_and_qp(opt.queue_depth);

    auto const slots = native_slot_count(opt);
    auto const data_offset = sizeof(remote_info);
    std::vector<char> storage(data_offset + opt.message_size * slots, 0);
    char* data = storage.data() + data_offset;
    if (opt.operation == rdma_bench::operation_kind::read) {
      for (std::size_t slot = 0; slot < slots; ++slot) {
        fill_payload(storage, data_offset + native_offset_for_slot(opt, slot),
                     opt.message_size);
      }
    }

    ULONG mr_flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE;
    if (opt.operation == rdma_bench::operation_kind::read) {
      mr_flags |= ND_MR_FLAG_ALLOW_REMOTE_READ;
    } else {
      mr_flags |= ND_MR_FLAG_ALLOW_REMOTE_WRITE;
    }
    s.create_and_register_mr(storage.data(), storage.size(), mr_flags);

    s.create_connector();
    s.create_listener();
    check_hr(s.listener_->Bind(reinterpret_cast<sockaddr*>(&local),
                               sizeof(local)),
             "IND2Listener::Bind failed");
    check_hr(s.listener_->Listen(1), "IND2Listener::Listen failed");
    std::cout << "RDMA_BENCH_READY role=server baseline=native_nd operation="
              << rdma_bench::operation_name(opt.operation)
              << " port=" << opt.port << "\n";
    signal_ready(ready);

    overlapped_holder ov_get;
    HRESULT hr = s.listener_->GetConnectionRequest(s.connector_.Get(),
                                                   ov_get.get());
    wait_overlapped(s.listener_.Get(), ov_get.ov, hr,
                    "IND2Listener::GetConnectionRequest failed");

    remote_info advertised{
        reinterpret_cast<std::uint64_t>(data),
        s.mr_->GetRemoteToken(),
        0};
    std::memcpy(storage.data(), &advertised, sizeof(advertised));

    ULONG inbound_read_limit = 0;
    if (opt.operation == rdma_bench::operation_kind::read) {
      inbound_read_limit =
          (std::min<ULONG>)(s.info_.MaxInboundReadLimit,
                            (std::max<ULONG>)(1, opt.queue_depth));
      if (inbound_read_limit == 0) {
        throw std::runtime_error("ND adapter reports zero inbound read limit");
      }
    }

    overlapped_holder ov_accept;
    hr = s.connector_->Accept(s.qp_.Get(), inbound_read_limit, 0, nullptr, 0,
                              ov_accept.get());
    wait_overlapped(s.connector_.Get(), ov_accept.ov, hr,
                    "IND2Connector::Accept failed");

    op_context info_send;
    auto info_sge = s.sge(storage.data(), static_cast<ULONG>(sizeof(remote_info)));
    s.post_send(info_send, info_sge);
    s.wait_for(info_send);

    overlapped_holder ov_disconnect;
    hr = s.connector_->NotifyDisconnect(ov_disconnect.get());
    wait_overlapped(s.connector_.Get(), ov_disconnect.ov, hr,
                    "IND2Connector::NotifyDisconnect failed");

    if (opt.operation == rdma_bench::operation_kind::write) {
      bool ok = true;
      auto slots_to_check =
          opt.metric == rdma_bench::metric_kind::latency ? std::size_t{1}
                                                         : slots;
      for (std::size_t slot = 0; slot < slots_to_check; ++slot) {
        for (std::size_t i = 0; i < opt.message_size; ++i) {
          if (data[native_offset_for_slot(opt, slot) + i] != fill_byte(i)) {
            ok = false;
            break;
          }
        }
        if (!ok) break;
      }
      result.validation_passed = ok;
      if (!ok) {
        result.errors = 1;
        result.first_error = "native ND remote write validation failed";
        result.exit_code = 1;
      }
    } else {
      result.validation_passed = true;
    }
    return result;
  } catch (std::exception const& e) {
    signal_ready(ready);
    return make_native_error(opt, cmd, exception_message(e));
  }
}

rdma_bench::result run_native_read_write_client(rdma_bench::options opt,
                                                std::string cmd) {
  opt.backend = "nd";
  opt.baseline = "native_nd";
  auto result = rdma_bench::make_base_result(opt, cmd);
  result.scenario_name = native_scenario_name(opt, "client");

  try {
    sockaddr_in local = make_ipv4_endpoint(opt.local_addr, 0);
    sockaddr_in peer = make_ipv4_endpoint(opt.peer_addr, opt.port);
    nd_session s(local);
    s.create_cq_and_qp(opt.queue_depth);

    ULONG outbound_read_limit = 0;
    std::uint64_t slot_limit = s.send_depth_;
    if (opt.operation == rdma_bench::operation_kind::read) {
      outbound_read_limit =
          (std::min<ULONG>)(s.info_.MaxOutboundReadLimit,
                            (std::max<ULONG>)(1, opt.queue_depth));
      if (outbound_read_limit == 0) {
        throw std::runtime_error("ND adapter reports zero outbound read limit");
      }
      slot_limit = (std::min<std::uint64_t>)(slot_limit, outbound_read_limit);
    }

    auto const slots = native_slot_count(opt, slot_limit);
    auto const data_offset = sizeof(remote_info);
    std::vector<char> storage(data_offset + opt.message_size * slots, 0);
    char* data = storage.data() + data_offset;
    if (opt.operation == rdma_bench::operation_kind::write) {
      for (std::size_t slot = 0; slot < slots; ++slot) {
        fill_payload(storage, data_offset + native_offset_for_slot(opt, slot),
                     opt.message_size);
      }
    }

    ULONG mr_flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE;
    if (opt.operation == rdma_bench::operation_kind::read) {
      mr_flags |= ND_MR_FLAG_RDMA_READ_SINK;
    }
    s.create_and_register_mr(storage.data(), storage.size(), mr_flags);

    s.create_connector();
    check_hr(s.connector_->Bind(reinterpret_cast<sockaddr*>(&local),
                                sizeof(local)),
             "IND2Connector::Bind failed");

    op_context info_recv;
    auto info_sge = s.sge(storage.data(), static_cast<ULONG>(sizeof(remote_info)));
    s.post_recv(info_recv, info_sge);

    overlapped_holder ov_connect;
    HRESULT hr = s.connector_->Connect(
        s.qp_.Get(), reinterpret_cast<sockaddr*>(&peer), sizeof(peer),
        0, outbound_read_limit, nullptr, 0, ov_connect.get());
    wait_overlapped(s.connector_.Get(), ov_connect.ov, hr,
                    "IND2Connector::Connect failed");

    overlapped_holder ov_complete;
    hr = s.connector_->CompleteConnect(ov_complete.get());
    wait_overlapped(s.connector_.Get(), ov_complete.ov, hr,
                    "IND2Connector::CompleteConnect failed");

    s.wait_for(info_recv);
    if (info_recv.bytes != sizeof(remote_info)) {
      throw std::runtime_error("short native ND remote-info receive");
    }
    remote_info remote{};
    std::memcpy(&remote, storage.data(), sizeof(remote));

    auto const message_size = checked_message_size(opt);
    auto post_one = [&](op_context& ctx, std::size_t slot) {
      auto sge = s.sge(data + native_offset_for_slot(opt, slot), message_size);
      auto slot_remote = remote_for_slot(remote, opt, slot);
      if (opt.operation == rdma_bench::operation_kind::write) {
        s.post_write(ctx, sge, slot_remote);
      } else {
        s.post_read(ctx, sge, slot_remote);
      }
    };
    auto verify_read = [&](std::size_t slot) {
      if (opt.operation != rdma_bench::operation_kind::read) return;
      auto offset = native_offset_for_slot(opt, slot);
      for (std::size_t i = 0; i < opt.message_size; ++i) {
        if (data[offset + i] != fill_byte(i)) {
          throw std::runtime_error("native ND read validation failed");
        }
      }
    };

    auto cpu_begin = rdma_bench::take_cpu_snapshot();
    auto begin = clock_type::now();
    if (opt.metric == rdma_bench::metric_kind::bandwidth) {
      std::vector<op_context> op_ctx(slots);
      std::uint64_t posted = 0;
      auto submit = [&](std::size_t slot) {
        post_one(op_ctx[slot], slot);
        ++posted;
        result.posted_count = posted;
      };

      for (std::size_t slot = 0; slot < slots; ++slot) submit(slot);
      while (result.completed_count < opt.iterations) {
        auto slot = static_cast<std::size_t>(result.completed_count % slots);
        s.wait_for(op_ctx[slot]);
        verify_read(slot);
        ++result.completed_count;
        if (posted < opt.iterations) submit(slot);
      }
      rdma_bench::finish_throughput(result, begin, clock_type::now());
    } else {
      std::vector<double> samples;
      samples.reserve(static_cast<std::size_t>(opt.iterations));
      for (std::uint64_t i = 0; i < opt.iterations; ++i) {
        auto sample_begin = clock_type::now();
        op_context ctx;
        post_one(ctx, std::size_t{0});
        ++result.posted_count;
        s.wait_for(ctx);
        auto sample_end = clock_type::now();
        verify_read(std::size_t{0});
        samples.push_back(
            std::chrono::duration<double, std::micro>(sample_end - sample_begin)
                .count());
        ++result.completed_count;
      }
      rdma_bench::finish_throughput(result, begin, clock_type::now());
      rdma_bench::fill_latency(result, samples);
    }

    rdma_bench::fill_cpu_metrics(result, cpu_begin,
                                 rdma_bench::take_cpu_snapshot());

    overlapped_holder ov_disconnect;
    hr = s.connector_->Disconnect(ov_disconnect.get());
    wait_overlapped(s.connector_.Get(), ov_disconnect.ov, hr,
                    "IND2Connector::Disconnect failed");
    result.validation_passed = true;
    return result;
  } catch (std::exception const& e) {
    return make_native_error(opt, cmd, exception_message(e));
  }
}

rdma_bench::result run_native_server(rdma_bench::options opt,
                                     std::string cmd,
                                     std::promise<void>* ready) {
  opt.backend = "nd";
  opt.baseline = "native_nd";
  if (opt.operation != rdma_bench::operation_kind::send_recv) {
    return run_native_read_write_server(std::move(opt), std::move(cmd), ready);
  }
  auto result = rdma_bench::make_base_result(opt, cmd);
  result.scenario_name = native_scenario_name(opt, "server");

  try {
    sockaddr_in local = make_ipv4_endpoint(opt.local_addr, opt.port);
    nd_session s(local);
    s.create_cq_and_qp(opt.queue_depth);

    std::size_t slots =
        static_cast<std::size_t>(std::max<std::uint64_t>(
            1, std::min<std::uint64_t>(opt.queue_depth, opt.iterations)));
    std::vector<char> storage(1 + opt.message_size * std::max<std::size_t>(2, slots), 0);
    s.create_and_register_mr(storage.data(), storage.size(),
                             ND_MR_FLAG_ALLOW_LOCAL_WRITE);

    s.create_connector();
    s.create_listener();
    check_hr(s.listener_->Bind(reinterpret_cast<sockaddr*>(&local),
                               sizeof(local)),
             "IND2Listener::Bind failed");
    check_hr(s.listener_->Listen(1), "IND2Listener::Listen failed");
    std::cout << "RDMA_BENCH_READY role=server baseline=native_nd port="
              << opt.port << "\n";
    signal_ready(ready);

    overlapped_holder ov_get;
    HRESULT hr = s.listener_->GetConnectionRequest(s.connector_.Get(),
                                                   ov_get.get());
    wait_overlapped(s.listener_.Get(), ov_get.ov, hr,
                    "IND2Listener::GetConnectionRequest failed");

    if (opt.metric == rdma_bench::metric_kind::bandwidth) {
      std::vector<op_context> recv_ctx(slots);
      for (std::size_t slot = 0; slot < slots; ++slot) {
        auto offset = 1 + slot * opt.message_size;
        auto recv_sge = s.sge(storage.data() + offset,
                              static_cast<ULONG>(opt.message_size));
        s.post_recv(recv_ctx[slot], recv_sge);
        ++result.posted_count;
      }

      overlapped_holder ov_accept;
      hr = s.connector_->Accept(s.qp_.Get(), 0, 0, nullptr, 0,
                                ov_accept.get());
      wait_overlapped(s.connector_.Get(), ov_accept.ov, hr,
                      "IND2Connector::Accept failed");

      storage[0] = 'R';
      op_context ready_send;
      auto ready_sge = s.sge(storage.data(), 1);
      s.post_send(ready_send, ready_sge);
      s.wait_for(ready_send);

      auto begin = clock_type::now();
      while (result.completed_count < opt.iterations) {
        auto slot = static_cast<std::size_t>(result.completed_count % slots);
        s.wait_for(recv_ctx[slot]);
        if (recv_ctx[slot].bytes != opt.message_size) {
          throw std::runtime_error("short native ND receive");
        }
        ++result.completed_count;
        if (result.posted_count < opt.iterations) {
          auto offset = 1 + slot * opt.message_size;
          auto recv_sge = s.sge(storage.data() + offset,
                                static_cast<ULONG>(opt.message_size));
          s.post_recv(recv_ctx[slot], recv_sge);
          ++result.posted_count;
        }
      }
      rdma_bench::finish_throughput(result, begin, clock_type::now());
    } else {
      op_context recv_ctx[2];
      auto recv_sge0 = s.sge(storage.data() + 1,
                             static_cast<ULONG>(opt.message_size));
      s.post_recv(recv_ctx[0], recv_sge0);
      ++result.posted_count;

      overlapped_holder ov_accept;
      hr = s.connector_->Accept(s.qp_.Get(), 0, 0, nullptr, 0,
                                ov_accept.get());
      wait_overlapped(s.connector_.Get(), ov_accept.ov, hr,
                      "IND2Connector::Accept failed");

      storage[0] = 'R';
      op_context ready_send;
      auto ready_sge = s.sge(storage.data(), 1);
      s.post_send(ready_send, ready_sge);
      s.wait_for(ready_send);

      for (std::uint64_t i = 0; i < opt.iterations; ++i) {
        auto slot = static_cast<std::size_t>(i % 2);
        s.wait_for(recv_ctx[slot]);
        if (recv_ctx[slot].bytes != opt.message_size) {
          throw std::runtime_error("short native ND latency receive");
        }
        if (i + 1 < opt.iterations) {
          auto next_slot = std::size_t{1} - slot;
          auto next_sge = s.sge(storage.data() + 1 + next_slot * opt.message_size,
                                static_cast<ULONG>(opt.message_size));
          s.post_recv(recv_ctx[next_slot], next_sge);
          ++result.posted_count;
        }
        op_context send_ctx;
        auto send_sge = s.sge(storage.data() + 1 + slot * opt.message_size,
                              static_cast<ULONG>(opt.message_size));
        s.post_send(send_ctx, send_sge);
        s.wait_for(send_ctx);
        ++result.completed_count;
      }
    }

    overlapped_holder ov_disconnect;
    s.connector_->Disconnect(ov_disconnect.get());
    result.validation_passed = true;
    return result;
  } catch (std::exception const& e) {
    signal_ready(ready);
    return make_native_error(opt, cmd, exception_message(e));
  }
}

rdma_bench::result run_native_client(rdma_bench::options opt,
                                     std::string cmd) {
  opt.backend = "nd";
  opt.baseline = "native_nd";
  if (opt.operation != rdma_bench::operation_kind::send_recv) {
    return run_native_read_write_client(std::move(opt), std::move(cmd));
  }
  auto result = rdma_bench::make_base_result(opt, cmd);
  result.scenario_name = native_scenario_name(opt, "client");

  try {
    sockaddr_in local = make_ipv4_endpoint(opt.local_addr, 0);
    sockaddr_in peer = make_ipv4_endpoint(opt.peer_addr, opt.port);
    nd_session s(local);
    s.create_cq_and_qp(opt.queue_depth);

    std::size_t slots =
        static_cast<std::size_t>(std::max<std::uint64_t>(
            1, std::min<std::uint64_t>(opt.queue_depth, opt.iterations)));
    std::vector<char> storage(1 + opt.message_size * std::max<std::size_t>(2, slots), 0);
    for (std::size_t slot = 0; slot < std::max<std::size_t>(2, slots); ++slot) {
      fill_payload(storage, 1 + slot * opt.message_size, opt.message_size);
    }
    s.create_and_register_mr(storage.data(), storage.size(),
                             ND_MR_FLAG_ALLOW_LOCAL_WRITE);

    s.create_connector();
    check_hr(s.connector_->Bind(reinterpret_cast<sockaddr*>(&local),
                                sizeof(local)),
             "IND2Connector::Bind failed");

    op_context ready_recv;
    auto ready_sge = s.sge(storage.data(), 1);
    s.post_recv(ready_recv, ready_sge);

    overlapped_holder ov_connect;
    HRESULT hr = s.connector_->Connect(
        s.qp_.Get(), reinterpret_cast<sockaddr*>(&peer), sizeof(peer),
        0, 0, nullptr, 0, ov_connect.get());
    wait_overlapped(s.connector_.Get(), ov_connect.ov, hr,
                    "IND2Connector::Connect failed");

    overlapped_holder ov_complete;
    hr = s.connector_->CompleteConnect(ov_complete.get());
    wait_overlapped(s.connector_.Get(), ov_complete.ov, hr,
                    "IND2Connector::CompleteConnect failed");

    s.wait_for(ready_recv);
    if (storage[0] != 'R') throw std::runtime_error("missing native ND ready byte");

    auto cpu_begin = rdma_bench::take_cpu_snapshot();
    auto begin = clock_type::now();
    if (opt.metric == rdma_bench::metric_kind::bandwidth) {
      std::vector<op_context> send_ctx(slots);
      std::uint64_t posted = 0;
      auto post_one = [&](std::size_t slot) {
        auto sge = s.sge(storage.data() + 1 + slot * opt.message_size,
                         static_cast<ULONG>(opt.message_size));
        s.post_send(send_ctx[slot], sge);
        ++posted;
        result.posted_count = posted;
      };
      for (std::size_t slot = 0; slot < slots; ++slot) post_one(slot);
      while (result.completed_count < opt.iterations) {
        auto slot = static_cast<std::size_t>(result.completed_count % slots);
        s.wait_for(send_ctx[slot]);
        ++result.completed_count;
        if (posted < opt.iterations) post_one(slot);
      }
      rdma_bench::finish_throughput(result, begin, clock_type::now());
    } else {
      std::vector<double> samples;
      samples.reserve(static_cast<std::size_t>(opt.iterations));
      for (std::uint64_t i = 0; i < opt.iterations; ++i) {
        auto sample_begin = clock_type::now();
        op_context recv_ctx;
        auto recv_sge = s.sge(storage.data() + 1 + opt.message_size,
                              static_cast<ULONG>(opt.message_size));
        s.post_recv(recv_ctx, recv_sge);
        op_context send_ctx;
        auto send_sge = s.sge(storage.data() + 1,
                              static_cast<ULONG>(opt.message_size));
        s.post_send(send_ctx, send_sge);
        s.wait_for(send_ctx);
        s.wait_for(recv_ctx);
        auto sample_end = clock_type::now();
        samples.push_back(
            std::chrono::duration<double, std::micro>(sample_end - sample_begin)
                .count() / 2.0);
        ++result.posted_count;
        ++result.completed_count;
      }
      rdma_bench::finish_throughput(result, begin, clock_type::now());
      rdma_bench::fill_latency(result, samples);
    }
    rdma_bench::fill_cpu_metrics(result, cpu_begin,
                                 rdma_bench::take_cpu_snapshot());

    overlapped_holder ov_disconnect;
    s.connector_->Disconnect(ov_disconnect.get());
    result.validation_passed = true;
    return result;
  } catch (std::exception const& e) {
    return make_native_error(opt, cmd, exception_message(e));
  }
}

int run_native_baseline(rdma_bench::options opt, std::string cmd) {
  winsock_runtime winsock;
  nd_runtime nd;

  rdma_bench::result selected;
  if (opt.single_process) {
    std::promise<void> ready;
    auto ready_fut = ready.get_future();
    auto server_opt = opt;
    server_opt.topology = "single_host_same_process";
    auto client_opt = opt;
    client_opt.peer_addr = opt.local_addr;
    client_opt.topology = "single_host_same_process";
    rdma_bench::result server_result;
    std::thread server([&] {
      server_result = run_native_server(server_opt, cmd, &ready);
    });
    ready_fut.wait();
    selected = run_native_client(client_opt, cmd);
    if (server.joinable()) server.join();
    if (selected.errors == 0 && server_result.errors != 0) {
      selected = server_result;
    } else if (selected.errors != 0 && server_result.errors != 0) {
      selected.first_error += "; server: " + server_result.first_error;
    }
  } else if (opt.server) {
    selected = run_native_server(opt, cmd, nullptr);
  } else {
    selected = run_native_client(opt, cmd);
  }
  rdma_bench::write_result(selected, opt.json_out);
  return selected.exit_code;
}

int main(int argc, char* argv[]) {
  try {
    auto opt = rdma_bench::parse_options_with_scenario(argc, argv);
    auto cmd = rdma_bench::command_line(argc, argv);
    opt.backend = "nd";
    opt.baseline = "native_nd";

    if (opt.mode != "poll") {
      auto r = rdma_bench::make_skip_result(
          opt, cmd,
          "native ND direct baseline only supports poll mode",
          "native_nd_mode_" + opt.mode);
      r.backend = "nd";
      r.baseline = "native_nd";
      r.scenario_name = "native_nd_baseline";
      rdma_bench::write_result(r, opt.json_out);
      return 0;
    }
    if (opt.local_addr.empty()) {
      throw std::invalid_argument("--local-addr is required");
    }
    return run_native_baseline(std::move(opt), std::move(cmd));
  } catch (std::runtime_error const& e) {
    if (std::string_view(e.what()) == "help") {
      rdma_bench::print_usage(argv[0]);
      return 0;
    }
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
