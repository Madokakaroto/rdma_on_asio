#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#include "rdma_test_address.hpp"

namespace fs = std::filesystem;
using clock_type = std::chrono::steady_clock;

constexpr int k_skip_exit_code = 77;

struct skip_test : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct options {
  std::string name = "rdma_ctest";
  fs::path work_dir = fs::path("rdma-ctest");
  std::chrono::seconds timeout{30};
  std::string server_ready_regex = "RDMA_CTEST_READY|RDMA_BENCH_READY";
  std::chrono::milliseconds server_ready_delay{0};
  std::vector<std::string> server_command;
  std::vector<std::string> client_command;
};

std::string take_value(int& index, int argc, char* argv[],
                       std::string_view name) {
  if (index + 1 >= argc) {
    throw std::invalid_argument("missing value for " + std::string(name));
  }
  return argv[++index];
}

std::chrono::seconds parse_seconds(std::string const& value,
                                   std::string_view name) {
  auto parsed = std::stoul(value);
  if (parsed == 0) {
    throw std::invalid_argument(std::string(name) + " must be greater than zero");
  }
  return std::chrono::seconds(parsed);
}

std::chrono::milliseconds parse_milliseconds(std::string const& value,
                                             std::string_view name) {
  return std::chrono::milliseconds(std::stoul(value));
}

options parse_options(int argc, char* argv[]) {
  options opt;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--name") {
      opt.name = take_value(i, argc, argv, arg);
    } else if (arg == "--work-dir") {
      opt.work_dir = take_value(i, argc, argv, arg);
    } else if (arg == "--timeout-sec") {
      opt.timeout = parse_seconds(take_value(i, argc, argv, arg), arg);
    } else if (arg == "--server-ready-regex") {
      opt.server_ready_regex = take_value(i, argc, argv, arg);
    } else if (arg == "--server-ready-delay-ms") {
      opt.server_ready_delay =
          parse_milliseconds(take_value(i, argc, argv, arg), arg);
    } else if (arg == "--server") {
      opt.server_command.push_back(take_value(i, argc, argv, arg));
    } else if (arg == "--server-arg") {
      opt.server_command.push_back(take_value(i, argc, argv, arg));
    } else if (arg == "--client") {
      opt.client_command.push_back(take_value(i, argc, argv, arg));
    } else if (arg == "--client-arg") {
      opt.client_command.push_back(take_value(i, argc, argv, arg));
    } else if (arg == "--help" || arg == "-h") {
      throw std::runtime_error("help");
    } else {
      throw std::invalid_argument("unknown option: " + std::string(arg));
    }
  }

  if (opt.server_command.empty()) {
    throw std::invalid_argument("--server is required");
  }
  if (opt.client_command.empty()) {
    throw std::invalid_argument("--client is required");
  }
  return opt;
}

std::string query_auto_address(std::string_view token) {
  try {
    if (token == "auto") {
      return rdma_test::local_device_address_string(asio::rdma::tcp::v4());
    }
    if (token == "auto-v4") {
      return rdma_test::local_device_address_string(asio::rdma::tcp::v4());
    }
    if (token == "auto-v6") {
      return rdma_test::local_device_address_string(asio::rdma::tcp::v6());
    }
  } catch (std::exception const& e) {
    throw skip_test(std::string("RDMA_CTEST_SKIP: ") +
                    "cannot resolve " + std::string(token) + ": " + e.what());
  }
  return std::string(token);
}

std::vector<std::string> resolve_auto_arguments(
    std::vector<std::string> const& command) {
  std::vector<std::string> resolved;
  resolved.reserve(command.size());
  for (auto const& arg : command) {
    if (arg == "auto" || arg == "auto-v4" || arg == "auto-v6") {
      resolved.push_back(query_auto_address(arg));
    } else {
      resolved.push_back(arg);
    }
  }
  return resolved;
}

std::string shell_quote_for_display(std::string const& value) {
  if (value.find_first_of(" \t\"") == std::string::npos) return value;
  std::string out = "\"";
  for (char ch : value) {
    if (ch == '"') out += '\\';
    out += ch;
  }
  out += '"';
  return out;
}

std::string command_for_display(std::vector<std::string> const& command) {
  std::string out;
  for (auto const& arg : command) {
    if (!out.empty()) out += ' ';
    out += shell_quote_for_display(arg);
  }
  return out;
}

class child_process {
public:
  child_process(std::string role, std::vector<std::string> command,
                fs::path log_path)
      : role_(std::move(role)),
        command_(std::move(command)),
        log_path_(std::move(log_path)) {}

  ~child_process() {
    if (running()) terminate();
    join_reader();
    close_handles();
  }

  child_process(child_process const&) = delete;
  child_process& operator=(child_process const&) = delete;

  void start() {
    if (command_.empty()) {
      throw std::invalid_argument("empty command for " + role_);
    }
    fs::create_directories(log_path_.parent_path());
    log_.open(log_path_, std::ios::binary);
    if (!log_) {
      throw std::runtime_error("failed to open log: " + log_path_.string());
    }

#if defined(_WIN32)
    start_windows();
#else
    start_posix();
#endif
  }

  bool wait_until_output_matches(std::regex const& re,
                                 clock_type::time_point deadline) {
    std::unique_lock lock(output_mutex_);
    while (clock_type::now() < deadline) {
      if (std::regex_search(output_, re)) return true;
      lock.unlock();
      if (!running()) {
        lock.lock();
        return std::regex_search(output_, re);
      }
      lock.lock();
      output_cv_.wait_until(lock, clock_type::now() +
                                      std::chrono::milliseconds(50));
    }
    return std::regex_search(output_, re);
  }

  bool wait_until_exit(clock_type::time_point deadline) {
    while (clock_type::now() < deadline) {
      if (!running()) {
        join_reader();
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return !running();
  }

  bool running() {
#if defined(_WIN32)
    if (!process_) return false;
    DWORD code = 0;
    if (!::GetExitCodeProcess(process_, &code)) return false;
    if (code == STILL_ACTIVE) return true;
    exit_code_ = static_cast<int>(code);
    return false;
#else
    if (pid_ <= 0 || exited_) return false;
    int status = 0;
    auto rc = ::waitpid(pid_, &status, WNOHANG);
    if (rc == 0) return true;
    if (rc == pid_) {
      exited_ = true;
      exit_code_ = status_to_exit_code(status);
      return false;
    }
    exited_ = true;
    exit_code_ = 1;
    return false;
#endif
  }

  int exit_code() {
    (void)running();
    return exit_code_;
  }

  void terminate() {
#if defined(_WIN32)
    if (process_) {
      DWORD code = 0;
      if (::GetExitCodeProcess(process_, &code) && code == STILL_ACTIVE) {
        ::TerminateProcess(process_, 1);
        ::WaitForSingleObject(process_, 5000);
      }
    }
#else
    if (pid_ > 0 && running()) {
      ::kill(pid_, SIGTERM);
      auto deadline = clock_type::now() + std::chrono::seconds(5);
      while (clock_type::now() < deadline && running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      if (running()) {
        ::kill(pid_, SIGKILL);
        while (running()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
      }
    }
#endif
  }

  fs::path const& log_path() const noexcept { return log_path_; }
  std::string const& role() const noexcept { return role_; }
  std::vector<std::string> const& command() const noexcept { return command_; }

private:
#if defined(_WIN32)
  static std::string quote_windows_arg(std::string const& arg) {
    if (arg.empty()) return "\"\"";
    bool need_quotes = arg.find_first_of(" \t\"") != std::string::npos;
    if (!need_quotes) return arg;

    std::string out = "\"";
    unsigned backslashes = 0;
    for (char ch : arg) {
      if (ch == '\\') {
        ++backslashes;
      } else if (ch == '"') {
        out.append(backslashes * 2 + 1, '\\');
        out += ch;
        backslashes = 0;
      } else {
        out.append(backslashes, '\\');
        backslashes = 0;
        out += ch;
      }
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
  }

  static std::string make_windows_command_line(
      std::vector<std::string> const& command) {
    std::string out;
    for (auto const& arg : command) {
      if (!out.empty()) out += ' ';
      out += quote_windows_arg(arg);
    }
    return out;
  }

  void start_windows() {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!::CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
      throw std::runtime_error("CreatePipe failed for " + role_);
    }
    read_pipe_ = read_pipe;
    if (!::SetHandleInformation(read_pipe_, HANDLE_FLAG_INHERIT, 0)) {
      ::CloseHandle(write_pipe);
      throw std::runtime_error("SetHandleInformation failed for " + role_);
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;

    PROCESS_INFORMATION pi{};
    auto command_line = make_windows_command_line(command_);
    BOOL ok = ::CreateProcessA(
        nullptr, command_line.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(write_pipe);
    if (!ok) {
      close_handles();
      throw std::runtime_error("CreateProcess failed for " + role_ + ": " +
                               command_for_display(command_));
    }

    process_ = pi.hProcess;
    thread_ = pi.hThread;
    reader_ = std::thread([this] { read_output_windows(); });
  }

  void read_output_windows() {
    char buffer[4096];
    DWORD n = 0;
    while (::ReadFile(read_pipe_, buffer, sizeof(buffer), &n, nullptr) && n > 0) {
      append_output(buffer, n);
    }
  }
#else
  static int status_to_exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
  }

  void start_posix() {
    int fds[2];
    if (::pipe(fds) != 0) {
      throw std::runtime_error("pipe failed for " + role_);
    }
    read_fd_ = fds[0];
    int write_fd = fds[1];

    pid_ = ::fork();
    if (pid_ < 0) {
      ::close(read_fd_);
      ::close(write_fd);
      read_fd_ = -1;
      throw std::runtime_error("fork failed for " + role_);
    }

    if (pid_ == 0) {
      ::dup2(write_fd, STDOUT_FILENO);
      ::dup2(write_fd, STDERR_FILENO);
      ::close(read_fd_);
      ::close(write_fd);

      std::vector<char*> argv;
      argv.reserve(command_.size() + 1);
      for (auto& arg : command_) argv.push_back(arg.data());
      argv.push_back(nullptr);
      ::execv(argv[0], argv.data());
      _exit(127);
    }

    ::close(write_fd);
    reader_ = std::thread([this] { read_output_posix(); });
  }

  void read_output_posix() {
    char buffer[4096];
    for (;;) {
      auto n = ::read(read_fd_, buffer, sizeof(buffer));
      if (n <= 0) break;
      append_output(buffer, static_cast<std::size_t>(n));
    }
  }
#endif

  void append_output(char const* data, std::size_t size) {
    {
      std::lock_guard lock(output_mutex_);
      output_.append(data, size);
      if (log_) {
        log_.write(data, static_cast<std::streamsize>(size));
        log_.flush();
      }
    }
    output_cv_.notify_all();
  }

  void join_reader() {
    if (reader_.joinable()) reader_.join();
  }

  void close_handles() {
#if defined(_WIN32)
    if (read_pipe_) {
      ::CloseHandle(read_pipe_);
      read_pipe_ = nullptr;
    }
    if (thread_) {
      ::CloseHandle(thread_);
      thread_ = nullptr;
    }
    if (process_) {
      ::CloseHandle(process_);
      process_ = nullptr;
    }
#else
    if (read_fd_ >= 0) {
      ::close(read_fd_);
      read_fd_ = -1;
    }
#endif
  }

  std::string role_;
  std::vector<std::string> command_;
  fs::path log_path_;
  std::ofstream log_;
  std::thread reader_;
  std::mutex output_mutex_;
  std::condition_variable output_cv_;
  std::string output_;
  int exit_code_ = 0;

#if defined(_WIN32)
  HANDLE process_ = nullptr;
  HANDLE thread_ = nullptr;
  HANDLE read_pipe_ = nullptr;
#else
  pid_t pid_ = -1;
  int read_fd_ = -1;
  bool exited_ = false;
#endif
};

void print_usage(char const* argv0) {
  std::cerr
      << "Usage:\n"
      << "  " << argv0 << " --name NAME --server EXE [--server-arg ARG...]\n"
      << "      --client EXE [--client-arg ARG...] [--timeout-sec N]\n";
}

int run(options opt) {
  auto server_command = resolve_auto_arguments(opt.server_command);
  auto client_command = resolve_auto_arguments(opt.client_command);

  fs::create_directories(opt.work_dir);
  auto server_log = opt.work_dir / (opt.name + ".server.log");
  auto client_log = opt.work_dir / (opt.name + ".client.log");

  std::cout << "[rdma_ctest_runner] server: "
            << command_for_display(server_command) << "\n";
  std::cout << "[rdma_ctest_runner] client: "
            << command_for_display(client_command) << "\n";
  std::cout << "[rdma_ctest_runner] server log: " << server_log.string()
            << "\n";
  std::cout << "[rdma_ctest_runner] client log: " << client_log.string()
            << "\n";

  auto deadline = clock_type::now() + opt.timeout;
  child_process server("server", std::move(server_command), server_log);
  server.start();

  if (opt.server_ready_delay.count() > 0) {
    std::this_thread::sleep_for(opt.server_ready_delay);
  } else if (!opt.server_ready_regex.empty()) {
    std::regex ready(opt.server_ready_regex);
    if (!server.wait_until_output_matches(ready, deadline)) {
      server.terminate();
      std::cerr << "server did not become ready before timeout\n";
      return 1;
    }
  }

  child_process client("client", std::move(client_command), client_log);
  client.start();

  if (!client.wait_until_exit(deadline)) {
    client.terminate();
    server.terminate();
    std::cerr << "client timed out\n";
    return 1;
  }

  auto server_grace = clock_type::now() + std::chrono::seconds(5);
  if (server_grace > deadline) server_grace = deadline;
  if (!server.wait_until_exit(server_grace)) {
    server.terminate();
    std::cerr << "server did not exit after client finished\n";
    return 1;
  }

  if (client.exit_code() != 0) {
    std::cerr << "client exited with " << client.exit_code() << "\n";
    return client.exit_code();
  }
  if (server.exit_code() != 0) {
    std::cerr << "server exited with " << server.exit_code() << "\n";
    return server.exit_code();
  }

  std::cout << "[rdma_ctest_runner] PASS " << opt.name << "\n";
  return 0;
}

int main(int argc, char* argv[]) {
  try {
    auto opt = parse_options(argc, argv);
    return run(std::move(opt));
  } catch (std::runtime_error const& e) {
    if (std::string_view(e.what()) == "help") {
      print_usage(argv[0]);
      return 0;
    }
    auto const* skip = dynamic_cast<skip_test const*>(&e);
    if (skip) {
      std::cout << skip->what() << "\n";
      return k_skip_exit_code;
    }
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
