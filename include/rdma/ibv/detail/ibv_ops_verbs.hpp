#pragma once

#include <cstdint>
#include <fcntl.h>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include "asio.hpp"
#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "rdma/ibv/ibv_error.hpp"
#include "rdma/ibv/ibv_types.hpp"
#include "rdma/ibv/detail/ibv_impl_types.hpp"

// Thin wrappers over libibverbs / rdma_cm data-plane calls, mirroring the style
// of nd/detail/nd_ops_verbs.hpp: each takes an asio::error_code& out param. The
// op pointer is carried as wr_id so completions resolve back to the op.
namespace asio::rdma::detail::verbs_ops {

ASIO_DECL native_comp_channel_t* create_comp_channel(native_context_t* context,
                                                     asio::error_code& ec);

ASIO_DECL native_cq_t* create_cq(native_context_t* context, int cqe,
                                 void* cq_context,
                                 native_comp_channel_t* channel,
                                 int comp_vector, asio::error_code& ec);

// rdma_create_qp creates the QP on cm_id (id->qp), on the cm_id's context.
ASIO_DECL int create_qp(native_cm_id_t* cm_id, native_pd_t* pd,
                        native_qp_init_attr_t const& attr,
                        asio::error_code& ec);

ASIO_DECL int req_notify_cq(native_cq_t* cq, bool solicited_only,
                            asio::error_code& ec);

// Non-blocking (channel fd is O_NONBLOCK): returns 0 with cq/ctx on an event,
// -1 + EAGAIN when there is none.
ASIO_DECL int get_cq_event(native_comp_channel_t* channel, native_cq_t** out_cq,
                           void** out_ctx);

ASIO_DECL void ack_cq_events(native_cq_t* cq, unsigned int n);

inline int poll_cq(native_cq_t* cq, int num_entries, native_wc_t* wcs) {
  return ::ibv_poll_cq(cq, num_entries, wcs);
}

ASIO_DECL native_mr_t* reg_mr(native_pd_t* pd, void* addr, std::size_t length,
                              mr_acccess_flag_t flag, int extra_flag,
                              asio::error_code& ec);

ASIO_DECL int dereg_mr(native_mr_t* mr, asio::error_code& ec);

// ibv_modify_qp wrapper. Returns the error_code (empty on success). Used for
// setup-time / control-plane QP attribute changes that rdma_cm does not expose
// through rdma_conn_param (e.g. min_rnr_timer applied once the QP is RTS). Never
// call on the data path.
ASIO_DECL asio::error_code modify_qp(native_qp_t* qp, native_qp_attr_t* attr,
                                     int attr_mask);

// --- post wrappers: wr_id carries the op pointer (request_context) ---

inline int post_recv(native_qp_t* qp, void* request_context,
                     native_sge_t* sge_list, std::size_t sge_count,
                     asio::error_code& ec) {
  ibv_recv_wr wr{};
  ibv_recv_wr* bad = nullptr;
  wr.wr_id = reinterpret_cast<std::uint64_t>(request_context);
  wr.sg_list = sge_list;
  wr.num_sge = static_cast<int>(sge_count);
  int const rc = ::ibv_post_recv(qp, &wr, &bad);
  if (rc) {
    ec = make_system_error_code(rc);
  }
  else {
    ec.clear();
  }
  return rc;
}

inline int post_send(native_qp_t* qp, void* request_context,
                     native_sge_t* sge_list, std::size_t sge_count,
                     int /*flag*/, asio::error_code& ec) {
  ibv_send_wr wr{};
  ibv_send_wr* bad = nullptr;
  wr.wr_id = reinterpret_cast<std::uint64_t>(request_context);
  wr.sg_list = sge_list;
  wr.num_sge = static_cast<int>(sge_count);
  wr.opcode = IBV_WR_SEND;
  wr.send_flags = IBV_SEND_SIGNALED;
  int const rc = ::ibv_post_send(qp, &wr, &bad);
  if (rc) {
    ec = make_system_error_code(rc);
  }
  else {
    ec.clear();
  }
  return rc;
}

inline int post_rdma(native_qp_t* qp, void* request_context,
                     native_sge_t* sge_list, std::size_t sge_count,
                     ibv_wr_opcode opcode, std::uint64_t remote_addr,
                     std::uint32_t rkey, asio::error_code& ec) {
  ibv_send_wr wr{};
  ibv_send_wr* bad = nullptr;
  wr.wr_id = reinterpret_cast<std::uint64_t>(request_context);
  wr.sg_list = sge_list;
  wr.num_sge = static_cast<int>(sge_count);
  wr.opcode = opcode;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.wr.rdma.remote_addr = remote_addr;
  wr.wr.rdma.rkey = rkey;
  int const rc = ::ibv_post_send(qp, &wr, &bad);
  if (rc) {
    ec = make_system_error_code(rc);
  }
  else {
    ec.clear();
  }
  return rc;
}

inline int post_read(native_qp_t* qp, void* request_context,
                     native_sge_t* sge_list, std::size_t sge_count,
                     std::uint64_t remote_addr, std::uint32_t rkey,
                     int /*flag*/, asio::error_code& ec) {
  return post_rdma(qp, request_context, sge_list, sge_count, IBV_WR_RDMA_READ,
                   remote_addr, rkey, ec);
}

inline int post_write(native_qp_t* qp, void* request_context,
                      native_sge_t* sge_list, std::size_t sge_count,
                      std::uint64_t remote_addr, std::uint32_t rkey,
                      int /*flag*/, asio::error_code& ec) {
  return post_rdma(qp, request_context, sge_list, sge_count, IBV_WR_RDMA_WRITE,
                   remote_addr, rkey, ec);
}

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/ibv/detail/impl/ibv_ops_verbs.ipp"
#endif
