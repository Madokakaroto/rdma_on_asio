#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace asio::rdma::detail {

inline constexpr bool mr_contains_offset(std::size_t region_length,
                                         std::size_t offset,
                                         std::size_t length) noexcept {
  return offset <= region_length && length <= region_length - offset;
}

// The supported Windows/Linux targets use a flat virtual address space where
// object pointers round-trip through uintptr_t and integer ordering reflects
// virtual-address ordering. This deliberately avoids subtracting unrelated C++
// pointers, which would be undefined behaviour.
inline std::optional<std::size_t> mr_offset_of(void const* region_base,
                                               std::size_t region_length,
                                               void const* address,
                                               std::size_t length) noexcept {
  if (region_base == nullptr || address == nullptr) {
    return std::nullopt;
  }

  auto const base = reinterpret_cast<std::uintptr_t>(region_base);
  auto const point = reinterpret_cast<std::uintptr_t>(address);
  auto const max = (std::numeric_limits<std::uintptr_t>::max)();
  if (region_length > max - base || point < base) {
    return std::nullopt;
  }

  auto const integer_offset = point - base;
  if (integer_offset > (std::numeric_limits<std::size_t>::max)()) {
    return std::nullopt;
  }
  auto const offset = static_cast<std::size_t>(integer_offset);
  if (!mr_contains_offset(region_length, offset, length)) {
    return std::nullopt;
  }
  return offset;
}

}  // namespace asio::rdma::detail
