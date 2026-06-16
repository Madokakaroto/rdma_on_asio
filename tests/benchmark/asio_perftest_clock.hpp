#pragma once

// perftest-aligned cycle-counter timing (Stage 10 core, pulled ahead of 9b).
//
// perftest measures with the CPU cycle counter (RDTSC on x86) rather than a
// wall clock, and converts cycles->microseconds with a measured cycles/usec rate
// (a 200-point linear regression -- robust under frequency scaling and in VMs,
// where /proc/cpuinfo's current MHz is the wrong number for the invariant TSC).
// This header mirrors perftest's get_clock.c (get_cycles / sample_get_cpu_mhz).

#include <cmath>
#include <cstdint>

#if defined(_WIN32)
#  include <intrin.h>
#  include <windows.h>
#else
#  include <sys/time.h>
#endif

namespace rdma_bench {

using cycles_t = unsigned long long;

#if defined(__x86_64__) || defined(__i386__)
inline cycles_t get_cycles() {
  unsigned low, high;
  asm volatile("rdtsc" : "=a"(low), "=d"(high));
  return (static_cast<cycles_t>(high) << 32) | low;
}
#elif defined(_WIN32)
inline cycles_t get_cycles() { return __rdtsc(); }
#elif defined(__aarch64__)
inline cycles_t get_cycles() {
  cycles_t v;
  asm volatile("isb" ::: "memory");
  asm volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
}
#else
#  error "asio_perftest cycle counter not implemented for this architecture"
#endif

// Microseconds since an arbitrary epoch (for the regression sampling loop).
inline double now_usec() {
#if defined(_WIN32)
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (static_cast<double>(c.QuadPart) * 1e6) / static_cast<double>(f.QuadPart);
#else
  timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<double>(tv.tv_sec) * 1e6 + static_cast<double>(tv.tv_usec);
#endif
}

// Measure cycles-per-microsecond by linear regression of (elapsed_usec,
// elapsed_cycles) over increasing busy-wait windows (perftest sample_get_cpu_mhz).
inline double measure_cpu_mhz() {
  constexpr int kMeasurements = 200;
  constexpr int kUsecStep = 10;
  constexpr int kUsecStart = 100;
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (int i = 0; i < kMeasurements; ++i) {
    cycles_t start = get_cycles();
    double t1 = now_usec();
    double t2;
    double target = kUsecStart + i * kUsecStep;
    do {
      t2 = now_usec();
    } while (t2 - t1 < target);
    double x = t2 - t1;                                   // elapsed usec
    double y = static_cast<double>(get_cycles() - start);  // elapsed cycles
    sx += x; sy += y; sxx += x * x; sxy += x * y;
  }
  double n = kMeasurements;
  double b = (n * sxy - sx * sy) / (n * sxx - sx * sx);  // cycles per usec
  return b;
}

// cycles per microsecond (== CPU MHz), measured once and cached.
inline double cpu_mhz() {
  static double const value = measure_cpu_mhz();
  return value;
}

inline double cycles_to_usec(cycles_t c) {
  return static_cast<double>(c) / cpu_mhz();
}

}  // namespace rdma_bench
