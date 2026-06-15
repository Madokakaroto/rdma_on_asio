#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "asio/as_tuple.hpp"
#include "asio/buffer.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_future.hpp"

#include "rdma/rdma.hpp"
#include "rdma_bench_common.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;
using clock_type = std::chrono::steady_clock;
constexpr auto use_fut = asio::as_tuple(asio::use_future);
using future_result = std::future<std::tuple<asio::error_code, std::size_t>>;

char pattern_byte(std::size_t slot, std::size_t index) {
  return static_cast<char>((slot * 17 + index * 31 + 7) & 0x7f);
}

void fill_pattern(std::vector<char>& storage, std::size_t offset,
                  std::size_t slot, std::size_t length) {
  for (std::size_t i = 0; i < length; ++i) {
    storage[offset + i] = pattern_byte(slot, i);
  }
}

bool verify_pattern(std::vector<char> const& storage, std::size_t offset,
                    std::size_t slot, std::size_t length) {
  for (std::size_t i = 0; i < length; ++i) {
    if (storage[offset + i] != pattern_byte(slot, i)) return false;
  }
  return true;
}

class cq_spinner {
public:
  explicit cq_spinner(rdma::rdma_completion_queue& cq)
      : cq_(cq),
        thread_([this] {
          while (!stop_.load(std::memory_order_relaxed)) {
            asio::error_code ec;
            cq_.poll(ec);
          }
        }) {}

  ~cq_spinner() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
  }

  cq_spinner(cq_spinner const&) = delete;
  cq_spinner& operator=(cq_spinner const&) = delete;

private:
  rdma::rdma_completion_queue& cq_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

rdma_bench::result make_error(rdma_bench::options const& opt,
                              std::string const& command_line,
                              std::string message) {
  auto r = rdma_bench::make_base_result(opt, command_line);
  r.errors = 1;
  r.first_error = std::move(message);
  r.exit_code = 1;
  return r;
}

class event_rw_server : public std::enable_shared_from_this<event_rw_server> {
public:
  using done_handler = std::function<void(rdma_bench::result)>;

  event_rw_server(asio::io_context& io, rdma::rdma_device_ptr device,
                  rdma_bench::options opt, std::string command_line,
                  done_handler on_done)
      : io_(io),
        device_(std::move(device)),
        opt_(std::move(opt)),
        result_(rdma_bench::make_base_result(opt_, std::move(command_line))),
        listener_(io),
        conn_(io),
        qp_(io),
        on_done_(std::move(on_done)) {
    result_.scenario_name =
        opt_.operation == rdma_bench::operation_kind::write
            ? "write_server"
            : "read_server";
  }

  void start() {
    try {
      setup_buffers();
      listener_.open(tcp::v4());
      listener_.bind(opt_.port);
      listener_.listen();
      std::cout << "RDMA_BENCH_READY role=server operation="
                << rdma_bench::operation_name(opt_.operation)
                << " port=" << opt_.port << "\n";
      listener_.async_get_connection(
          conn_, asio::mutable_buffer{},
          [self = shared_from_this()](asio::error_code ec, std::size_t) {
            self->on_connection(ec);
          });
    } catch (std::exception const& e) {
      fail(e.what());
    }
  }

private:
  std::size_t slot_count() const {
    return static_cast<std::size_t>(
        std::max<std::uint64_t>(1, std::min<std::uint64_t>(opt_.queue_depth,
                                                          opt_.iterations)));
  }

  std::size_t offset_for_slot(std::size_t slot) const {
    return slot * opt_.message_size;
  }

  void setup_buffers() {
    auto slots = slot_count();
    storage_.assign(opt_.message_size * slots, 0);
    if (opt_.operation == rdma_bench::operation_kind::read) {
      for (std::size_t slot = 0; slot < slots; ++slot) {
        fill_pattern(storage_, offset_for_slot(slot), slot, opt_.message_size);
      }
    }
    mr_ = std::make_unique<rdma::rdma_memory_region>(
        device_, storage_.data(), storage_.size());
    remote_base_ = mr_->remote_addr(std::size_t{0}, storage_.size());
  }

  void on_connection(asio::error_code ec) {
    if (ec) return fail(ec.message());
    conn_.async_accept(qp_, asio::buffer(&remote_base_, sizeof(remote_base_)),
                       [self = shared_from_this()](asio::error_code ec) {
                         self->on_accept(ec);
                       });
  }

  void on_accept(asio::error_code ec) {
    if (ec) return fail(ec.message());
    conn_.async_wait_disconnect(
        [self = shared_from_this()](asio::error_code) {
          self->finish_success();
        });
  }

  void finish_success() {
    if (done_) return;
    done_ = true;
    if (opt_.operation == rdma_bench::operation_kind::write) {
      bool ok = true;
      auto slots = slot_count();
      for (std::size_t slot = 0; slot < slots; ++slot) {
        ok = ok && verify_pattern(storage_, offset_for_slot(slot), slot,
                                  opt_.message_size);
      }
      result_.validation_passed = ok;
      if (!ok) {
        result_.errors = 1;
        result_.first_error = "remote write validation failed";
        result_.exit_code = 1;
      }
    } else {
      result_.validation_passed = true;
    }
    on_done_(result_);
  }

  void fail(std::string message) {
    if (done_) return;
    done_ = true;
    result_.errors = 1;
    result_.first_error = std::move(message);
    result_.exit_code = 1;
    asio::error_code ignored;
    conn_.disconnect(ignored);
    on_done_(result_);
  }

  asio::io_context& io_;
  rdma::rdma_device_ptr device_;
  rdma_bench::options opt_;
  rdma_bench::result result_;
  rdma::rdma_listener<tcp> listener_;
  rdma::rdma_connector<tcp> conn_;
  rdma::rdma_queue_pair qp_;
  std::vector<char> storage_;
  std::unique_ptr<rdma::rdma_memory_region> mr_;
  rdma::rdma_remote_addr_t remote_base_{};
  bool done_ = false;
  done_handler on_done_;
};

class event_rw_client : public std::enable_shared_from_this<event_rw_client> {
public:
  using done_handler = std::function<void(rdma_bench::result)>;

  event_rw_client(asio::io_context& io, rdma::rdma_device_ptr device,
                  rdma_bench::options opt, std::string command_line,
                  done_handler on_done)
      : io_(io),
        device_(std::move(device)),
        opt_(std::move(opt)),
        result_(rdma_bench::make_base_result(opt_, std::move(command_line))),
        conn_(io),
        qp_(io),
        on_done_(std::move(on_done)) {
    result_.scenario_name =
        opt_.operation == rdma_bench::operation_kind::write
            ? "write_client"
            : "read_client";
  }

  void start() {
    try {
      conn_.open(tcp::v4());
      tcp::endpoint endpoint(asio::ip::make_address(opt_.peer_addr), opt_.port);
      conn_.async_connect(
          qp_, endpoint, asio::const_buffer{},
          asio::buffer(&remote_base_, sizeof(remote_base_)),
          [self = shared_from_this()](asio::error_code ec, std::size_t n) {
            self->on_connect(ec, n);
          });
    } catch (std::exception const& e) {
      fail(e.what());
    }
  }

private:
  std::size_t slot_count() const {
    return static_cast<std::size_t>(
        std::max<std::uint64_t>(1, std::min<std::uint64_t>(opt_.queue_depth,
                                                          opt_.iterations)));
  }

  std::size_t offset_for_slot(std::size_t slot) const {
    return slot * opt_.message_size;
  }

  rdma::rdma_remote_addr_t remote_for_slot(std::size_t slot) const {
    auto remote = remote_base_;
    remote.addr_ += static_cast<std::uint64_t>(offset_for_slot(slot));
    return remote;
  }

  void setup_buffers() {
    auto slots = slot_count();
    storage_.assign(opt_.message_size * slots, 0);
    if (opt_.operation == rdma_bench::operation_kind::write) {
      for (std::size_t slot = 0; slot < slots; ++slot) {
        fill_pattern(storage_, offset_for_slot(slot), slot, opt_.message_size);
      }
    }
    mr_ = std::make_unique<rdma::rdma_memory_region>(
        device_, storage_.data(), storage_.size());
  }

  void on_connect(asio::error_code ec, std::size_t n) {
    if (ec) return fail(ec.message());
    if (n < sizeof(remote_base_)) {
      return fail("server did not return a complete remote address");
    }
    setup_buffers();
    cpu_begin_ = rdma_bench::take_cpu_snapshot();
    begin_ = clock_type::now();
    if (opt_.metric == rdma_bench::metric_kind::latency) {
      samples_.reserve(static_cast<std::size_t>(opt_.iterations));
      post_latency_iteration();
      return;
    }
    auto initial = slot_count();
    for (std::size_t slot = 0; slot < initial; ++slot) {
      post_one(slot);
    }
  }

  void post_one(std::size_t slot) {
    ++posted_;
    result_.posted_count = posted_;
    auto remote = remote_for_slot(slot);
    if (opt_.operation == rdma_bench::operation_kind::write) {
      qp_.async_write(mr_->cslice(offset_for_slot(slot), opt_.message_size),
                      remote,
                      [self = shared_from_this(), slot](asio::error_code ec,
                                                        std::size_t n) {
                        self->on_complete(slot, ec, n);
                      });
    } else {
      qp_.async_read(mr_->slice(offset_for_slot(slot), opt_.message_size),
                     remote,
                     [self = shared_from_this(), slot](asio::error_code ec,
                                                       std::size_t n) {
                       self->on_complete(slot, ec, n);
                     });
    }
  }

  void on_complete(std::size_t slot, asio::error_code ec, std::size_t n) {
    if (done_) return;
    if (ec) return fail(ec.message());
    if (n != opt_.message_size) return fail("short RDMA operation");
    if (opt_.operation == rdma_bench::operation_kind::read &&
        !verify_pattern(storage_, offset_for_slot(slot), slot,
                        opt_.message_size)) {
      return fail("read validation failed");
    }
    ++result_.completed_count;
    if (posted_ < opt_.iterations) {
      post_one(slot);
      return;
    }
    if (result_.completed_count == opt_.iterations) {
      finish_success();
    }
  }

  void post_latency_iteration() {
    auto slot = std::size_t{0};
    sample_begin_ = clock_type::now();
    ++posted_;
    result_.posted_count = posted_;
    auto remote = remote_for_slot(slot);
    if (opt_.operation == rdma_bench::operation_kind::write) {
      qp_.async_write(mr_->cslice(offset_for_slot(slot), opt_.message_size),
                      remote,
                      [self = shared_from_this()](asio::error_code ec,
                                                  std::size_t n) {
                        self->on_latency_complete(ec, n);
                      });
    } else {
      qp_.async_read(mr_->slice(offset_for_slot(slot), opt_.message_size),
                     remote,
                     [self = shared_from_this()](asio::error_code ec,
                                                 std::size_t n) {
                       self->on_latency_complete(ec, n);
                     });
    }
  }

  void on_latency_complete(asio::error_code ec, std::size_t n) {
    if (done_) return;
    auto sample_end = clock_type::now();
    if (ec) return fail(ec.message());
    if (n != opt_.message_size) return fail("short RDMA operation");
    if (opt_.operation == rdma_bench::operation_kind::read &&
        !verify_pattern(storage_, std::size_t{0}, std::size_t{0},
                        opt_.message_size)) {
      return fail("read validation failed");
    }
    samples_.push_back(
        std::chrono::duration<double, std::micro>(sample_end - sample_begin_)
            .count());
    ++result_.completed_count;
    if (result_.completed_count == opt_.iterations) {
      finish_success();
      return;
    }
    post_latency_iteration();
  }

  void finish_success() {
    if (done_) return;
    done_ = true;
    rdma_bench::finish_throughput(result_, begin_, clock_type::now());
    rdma_bench::fill_cpu_metrics(result_, cpu_begin_,
                                 rdma_bench::take_cpu_snapshot());
    if (!samples_.empty()) rdma_bench::fill_latency(result_, samples_);
    result_.validation_passed = true;
    asio::error_code ignored;
    conn_.disconnect(ignored);
    on_done_(result_);
  }

  void fail(std::string message) {
    if (done_) return;
    done_ = true;
    result_.errors = 1;
    result_.first_error = std::move(message);
    result_.exit_code = 1;
    asio::error_code ignored;
    conn_.disconnect(ignored);
    on_done_(result_);
  }

  asio::io_context& io_;
  rdma::rdma_device_ptr device_;
  rdma_bench::options opt_;
  rdma_bench::result result_;
  rdma::rdma_connector<tcp> conn_;
  rdma::rdma_queue_pair qp_;
  std::vector<char> storage_;
  std::unique_ptr<rdma::rdma_memory_region> mr_;
  rdma::rdma_remote_addr_t remote_base_{};
  std::vector<double> samples_;
  std::uint64_t posted_ = 0;
  clock_type::time_point begin_ = clock_type::now();
  clock_type::time_point sample_begin_ = clock_type::now();
  rdma_bench::cpu_snapshot cpu_begin_{};
  bool done_ = false;
  done_handler on_done_;
};

int run_event_read_write(rdma_bench::options opt, std::string command_line) {
  asio::io_context io;
  auto device =
      rdma::rdma_device_manager_t::instance().get_first_available_device(tcp::v4(), {});
  rdma::use_device(io, device);

  std::vector<rdma_bench::result> results;
  std::atomic<int> remaining{0};
  auto done = [&](rdma_bench::result r) {
    results.push_back(std::move(r));
    if (--remaining == 0) io.stop();
  };

  if (opt.single_process) {
    remaining = 2;
    auto server_opt = opt;
    server_opt.topology = "single_host_same_process";
    auto client_opt = opt;
    client_opt.client = true;
    client_opt.peer_addr = opt.local_addr;
    client_opt.topology = "single_host_same_process";
    std::make_shared<event_rw_server>(io, device, server_opt, command_line, done)
        ->start();
    std::make_shared<event_rw_client>(io, device, client_opt, command_line, done)
        ->start();
  } else if (opt.server) {
    remaining = 1;
    std::make_shared<event_rw_server>(io, device, opt, command_line, done)
        ->start();
  } else {
    remaining = 1;
    std::make_shared<event_rw_client>(io, device, opt, command_line, done)
        ->start();
  }

  asio::steady_timer watchdog(io);
  watchdog.expires_after(std::chrono::seconds(opt.timeout_sec));
  watchdog.async_wait([&](asio::error_code ec) {
    if (!ec) {
      auto r = make_error(opt, command_line, "watchdog timeout");
      r.exit_code = 2;
      results.push_back(std::move(r));
      io.stop();
    }
  });

  io.run();
  watchdog.cancel();

  auto error = std::find_if(results.begin(), results.end(),
                            [](auto const& r) { return r.errors != 0; });
  if (error != results.end()) {
    rdma_bench::write_result(*error, opt.json_out);
    return error->exit_code;
  }

  auto selected = std::find_if(results.begin(), results.end(),
                               [](auto const& r) {
                                 return r.scenario_name == "write_client" ||
                                        r.scenario_name == "read_client";
                               });
  if (selected == results.end() && !results.empty()) selected = results.begin();
  if (selected == results.end()) {
    auto r = make_error(opt, command_line, "no result produced");
    rdma_bench::write_result(r, opt.json_out);
    return r.exit_code;
  }
  rdma_bench::write_result(*selected, opt.json_out);
  return selected->exit_code;
}

void signal_ready(std::promise<void>* ready) {
  if (!ready) return;
  try {
    ready->set_value();
  } catch (std::future_error const&) {
  }
}

std::size_t poll_slot_count(rdma_bench::options const& opt) {
  return static_cast<std::size_t>(
      std::max<std::uint64_t>(1, std::min<std::uint64_t>(opt.queue_depth,
                                                        opt.iterations)));
}

std::size_t poll_offset_for_slot(rdma_bench::options const& opt,
                                 std::size_t slot) {
  return slot * opt.message_size;
}

rdma::rdma_remote_addr_t poll_remote_for_slot(rdma::rdma_remote_addr_t base,
                                              rdma_bench::options const& opt,
                                              std::size_t slot) {
  base.addr_ += static_cast<std::uint64_t>(poll_offset_for_slot(opt, slot));
  return base;
}

rdma_bench::result run_poll_server_role(rdma_bench::options opt,
                                        std::string command_line,
                                        std::promise<void>* ready) {
  opt.server = true;
  opt.client = false;
  opt.single_process = false;
  auto result = rdma_bench::make_base_result(opt, command_line);
  result.scenario_name =
      opt.operation == rdma_bench::operation_kind::write
          ? "write_poll_server"
          : "read_poll_server";

  try {
    asio::io_context io;
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});
    rdma::use_device(io, device);

    auto slots = poll_slot_count(opt);
    std::vector<char> storage(opt.message_size * slots, 0);
    if (opt.operation == rdma_bench::operation_kind::read) {
      for (std::size_t slot = 0; slot < slots; ++slot) {
        fill_pattern(storage, poll_offset_for_slot(opt, slot), slot,
                     opt.message_size);
      }
    }
    rdma::rdma_memory_region mr(device, storage.data(), storage.size());
    auto remote_base = mr.remote_addr(std::size_t{0}, storage.size());

    rdma::rdma_listener<tcp> listener(io);
    listener.open(tcp::v4());
    listener.bind(opt.port);
    listener.listen();
    std::cout << "RDMA_BENCH_READY role=server mode=poll operation="
              << rdma_bench::operation_name(opt.operation)
              << " port=" << opt.port << "\n";
    signal_ready(ready);

    rdma::rdma_connector<tcp> conn(io);
    asio::error_code get_ec;
    listener.async_get_connection(
        conn, asio::mutable_buffer{},
        [&](asio::error_code ec, std::size_t) { get_ec = ec; });
    io.run();
    io.restart();
    if (get_ec) return make_error(opt, command_line, get_ec.message());

    rdma::rdma_completion_queue cq(device);
    rdma::rdma_queue_pair qp(cq);
    asio::error_code accept_ec;
    conn.async_accept(qp, asio::buffer(&remote_base, sizeof(remote_base)),
                      [&](asio::error_code ec) { accept_ec = ec; });
    io.run();
    io.restart();
    if (accept_ec) return make_error(opt, command_line, accept_ec.message());

    asio::error_code wait_ec;
    conn.async_wait_disconnect([&](asio::error_code ec) { wait_ec = ec; });
    io.run();
    io.restart();
    (void)wait_ec;

    if (opt.operation == rdma_bench::operation_kind::write) {
      bool ok = true;
      for (std::size_t slot = 0; slot < slots; ++slot) {
        ok = ok && verify_pattern(storage, poll_offset_for_slot(opt, slot),
                                  slot, opt.message_size);
      }
      result.validation_passed = ok;
      if (!ok) {
        result.errors = 1;
        result.first_error = "remote write validation failed";
        result.exit_code = 1;
      }
    } else {
      result.validation_passed = true;
    }
    return result;
  } catch (std::exception const& e) {
    signal_ready(ready);
    return make_error(opt, command_line, e.what());
  }
}

rdma_bench::result run_poll_client_role(rdma_bench::options opt,
                                        std::string command_line) {
  opt.client = true;
  opt.server = false;
  opt.single_process = false;
  auto result = rdma_bench::make_base_result(opt, command_line);
  result.scenario_name =
      opt.operation == rdma_bench::operation_kind::write
          ? "write_poll_client"
          : "read_poll_client";

  try {
    asio::io_context io;
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});
    rdma::use_device(io, device);

    rdma::rdma_completion_queue cq(device);
    rdma::rdma_connector<tcp> conn(io);
    conn.open(tcp::v4());
    rdma::rdma_queue_pair qp(cq);
    rdma::rdma_remote_addr_t remote_base{};
    tcp::endpoint endpoint(asio::ip::make_address(opt.peer_addr), opt.port);
    asio::error_code connect_ec;
    std::size_t reply_len = 0;
    conn.async_connect(qp, endpoint, asio::const_buffer{},
                       asio::buffer(&remote_base, sizeof(remote_base)),
                       [&](asio::error_code ec, std::size_t n) {
                         connect_ec = ec;
                         reply_len = n;
                       });
    io.run();
    io.restart();
    if (connect_ec) return make_error(opt, command_line, connect_ec.message());
    if (reply_len < sizeof(remote_base)) {
      return make_error(opt, command_line,
                        "server did not return a complete remote address");
    }

    auto slots = poll_slot_count(opt);
    std::vector<char> storage(opt.message_size * slots, 0);
    if (opt.operation == rdma_bench::operation_kind::write) {
      for (std::size_t slot = 0; slot < slots; ++slot) {
        fill_pattern(storage, poll_offset_for_slot(opt, slot), slot,
                     opt.message_size);
      }
    }
    rdma::rdma_memory_region mr(device, storage.data(), storage.size());

    cq_spinner spinner(cq);
    auto cpu_begin = rdma_bench::take_cpu_snapshot();
    auto begin = clock_type::now();
    if (opt.metric == rdma_bench::metric_kind::latency) {
      std::vector<double> samples;
      samples.reserve(static_cast<std::size_t>(opt.iterations));
      for (std::uint64_t i = 0; i < opt.iterations; ++i) {
        auto sample_begin = clock_type::now();
        auto remote = poll_remote_for_slot(remote_base, opt, std::size_t{0});
        future_result op;
        if (opt.operation == rdma_bench::operation_kind::write) {
          op = qp.async_write(mr.cslice(std::size_t{0}, opt.message_size),
                              remote, use_fut);
        } else {
          op = qp.async_read(mr.slice(std::size_t{0}, opt.message_size),
                             remote, use_fut);
        }
        auto [ec, n] = op.get();
        auto sample_end = clock_type::now();
        if (ec) return make_error(opt, command_line, ec.message());
        if (n != opt.message_size) {
          return make_error(opt, command_line, "short RDMA operation");
        }
        if (opt.operation == rdma_bench::operation_kind::read &&
            !verify_pattern(storage, std::size_t{0}, std::size_t{0},
                            opt.message_size)) {
          return make_error(opt, command_line, "read validation failed");
        }
        samples.push_back(
            std::chrono::duration<double, std::micro>(sample_end - sample_begin)
                .count());
        ++result.posted_count;
        ++result.completed_count;
      }
      rdma_bench::finish_throughput(result, begin, clock_type::now());
      rdma_bench::fill_latency(result, samples);
    } else {
      std::vector<future_result> ops(slots);
      std::uint64_t posted = 0;
      auto post_one = [&](std::size_t slot) {
        auto remote = poll_remote_for_slot(remote_base, opt, slot);
        if (opt.operation == rdma_bench::operation_kind::write) {
          ops[slot] =
              qp.async_write(mr.cslice(poll_offset_for_slot(opt, slot),
                                       opt.message_size),
                             remote, use_fut);
        } else {
          ops[slot] = qp.async_read(mr.slice(poll_offset_for_slot(opt, slot),
                                             opt.message_size),
                                    remote, use_fut);
        }
        ++posted;
        result.posted_count = posted;
      };

      for (std::size_t slot = 0; slot < slots; ++slot) post_one(slot);
      while (result.completed_count < opt.iterations) {
        auto slot = static_cast<std::size_t>(result.completed_count % slots);
        auto [ec, n] = ops[slot].get();
        if (ec) return make_error(opt, command_line, ec.message());
        if (n != opt.message_size) {
          return make_error(opt, command_line, "short RDMA operation");
        }
        if (opt.operation == rdma_bench::operation_kind::read &&
            !verify_pattern(storage, poll_offset_for_slot(opt, slot), slot,
                            opt.message_size)) {
          return make_error(opt, command_line, "read validation failed");
        }
        ++result.completed_count;
        if (posted < opt.iterations) post_one(slot);
      }
      rdma_bench::finish_throughput(result, begin, clock_type::now());
    }

    auto cpu_end = rdma_bench::take_cpu_snapshot();
    rdma_bench::fill_cpu_metrics(result, cpu_begin, cpu_end);
    result.validation_passed = true;
    asio::error_code ignored;
    conn.disconnect(ignored);
    return result;
  } catch (std::exception const& e) {
    return make_error(opt, command_line, e.what());
  }
}

int run_poll_read_write(rdma_bench::options opt, std::string command_line) {
  if (opt.token_type != "use_future") {
    auto r = rdma_bench::make_skip_result(
        opt, command_line,
        "poll-mode read/write uses as_tuple(use_future) in this benchmark",
        "read_write_poll_token_" + opt.token_type);
    rdma_bench::write_result(r, opt.json_out);
    return 0;
  }

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
      server_result = run_poll_server_role(server_opt, command_line, &ready);
    });
    ready_fut.wait();
    selected = run_poll_client_role(client_opt, command_line);
    if (server.joinable()) server.join();
    if (selected.errors == 0 && server_result.errors != 0) {
      selected = server_result;
    } else if (selected.errors != 0 && server_result.errors != 0) {
      selected.first_error += "; server: " + server_result.first_error;
    }
  } else if (opt.server) {
    selected = run_poll_server_role(opt, command_line, nullptr);
  } else {
    selected = run_poll_client_role(opt, command_line);
  }

  rdma_bench::write_result(selected, opt.json_out);
  return selected.exit_code;
}

int main(int argc, char* argv[]) {
  try {
    auto opt = rdma_bench::parse_options_with_scenario(argc, argv);
    auto cmd = rdma_bench::command_line(argc, argv);
    if (opt.operation != rdma_bench::operation_kind::write &&
        opt.operation != rdma_bench::operation_kind::read) {
      auto r = rdma_bench::make_skip_result(
          opt, cmd, "rdma_read_write_bench only implements read/write",
          "operation_" + std::string(rdma_bench::operation_name(opt.operation)));
      rdma_bench::write_result(r, opt.json_out);
      return 0;
    }
    if (opt.mode == "poll") {
      return run_poll_read_write(std::move(opt), std::move(cmd));
    }
    if (opt.mode != "event") {
      auto r = rdma_bench::make_skip_result(
          opt, cmd, "read/write benchmark supports event or poll mode only",
          "read_write_mode_" + opt.mode);
      rdma_bench::write_result(r, opt.json_out);
      return 0;
    }
    if (opt.token_type != "callback") {
      auto r = rdma_bench::make_skip_result(
          opt, cmd,
          "read/write benchmark currently supports callback token only",
          "read_write_token_" + opt.token_type);
      rdma_bench::write_result(r, opt.json_out);
      return 0;
    }
    return run_event_read_write(std::move(opt), std::move(cmd));
  } catch (std::runtime_error const& e) {
    if (std::string_view(e.what()) == "help") {
      rdma_bench::print_usage(argv[0]);
      return 0;
    }
    std::cerr << "fatal: " << e.what() << "\n";
    rdma_bench::print_usage(argv[0]);
    return 1;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    rdma_bench::print_usage(argv[0]);
    return 1;
  }
}
