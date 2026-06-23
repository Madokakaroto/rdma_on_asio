#pragma once

#if defined(ASIO_SEPARATE_COMPILATION) && \
    !defined(ASIO_RDMA_NETWORKDIRECT_SOURCE_FILE)
#error Do not include rdma/nd/impl/networkdirect.hpp directly when ASIO_SEPARATE_COMPILATION is defined; link native ndutil.lib instead.
#endif

#if defined(_NDSPI_H_)
#error Include rdma/nd/impl/networkdirect.hpp from a dedicated implementation TU before any NetworkDirect or RDMA ND headers.
#endif

#include "asio/detail/socket_types.hpp"

#define ASIO_RDMA_NETWORKDIRECT_SOURCE 1

#include "ndaddr.cpp"
#include "ndprov.cpp"
#include "ndfrmwrk.cpp"
