#pragma once

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "rdma/nd/nd_types.hpp"
#include "rdma/nd/nd_error.hpp"
#include "rdma/nd/detail/nd_device_impl.hpp"

namespace asio::rdma::detail::verbs_ops {

// post send
inline result_type post_send(native_qp_t* qp, void* request_context,
                             native_sge_t* sge_list, size_type sge_count,
                             int flag, asio::error_code& ec) {
  assert(qp);
  assert(sge_list);
  auto const hr = qp->Send(request_context, sge_list, sge_count, flag);
  ec = static_cast<nd_errc>(hr);
  return hr;
}

// Post receive.
inline result_type post_recv(native_qp_t* qp, void* request_context,
                             native_sge_t* sge_list, size_type sge_count,
                             asio::error_code& ec) {
  assert(qp);
  assert(sge_list);
  auto const hr = qp->Receive(request_context, sge_list, sge_count);
  ec = static_cast<nd_errc>(hr);
  return hr;
}

// post read
inline result_type post_read(native_qp_t* qp, void* request_context,
                             native_sge_t* sge_list, size_type sge_count,
                             uint64_t remote_addr, uint32_t remote_token,
                             int flag, asio::error_code& ec) {
  assert(qp);
  assert(sge_list);
  auto const hr = qp->Read(request_context, sge_list, sge_count, remote_addr,
                           remote_token, flag);
  ec = static_cast<nd_errc>(hr);
  return hr;
}

// post write
inline result_type post_write(native_qp_t* qp, void* request_context,
                              native_sge_t* sge_list, size_type sge_count,
                              uint64_t remote_addr, uint32_t remote_token,
                              int flag, asio::error_code& ec) {
  assert(qp);
  assert(sge_list);
  auto const hr = qp->Write(request_context, sge_list, sge_count, remote_addr,
                            remote_token, flag);
  ec = static_cast<nd_errc>(hr);
  return hr;
}

// simulate ibv_allocate_pd
ASIO_DECL native_pd_t* allocate_pd(native_context_t* context,
                                   asio::error_code& ec);

/// cq ops
// create cq
ASIO_DECL native_cq_t* create_cq(native_context_t* context, size_type cqe,
                                 native_cq_init_attr const& init_attr,
                                 asio::error_code& ec);

// notify cq
ASIO_DECL result_type notify_cq(native_cq_t* cq,
                                native_cq_notify_attr const& attr,
                                asio::error_code& ec);

// poll cq
inline size_type poll_cq(native_cq_t* cq, native_wc_t& wc) {
  assert(cq);
  return cq->GetResults(&wc, 1);
}
inline size_type poll_cq(native_cq_t* cq, native_wc_t* wcs, size_type count) {
  assert(cq);
  assert(wcs || count == 0);
  return cq->GetResults(wcs, count);
}
template <size_t Num>
inline size_type poll_cq(native_cq_t* cq, std::array<native_wc_t, Num>& wcs) {
  return poll_cq(cq, wcs.data(), static_cast<size_type>(Num));
}

/// qp ops
// create qp
ASIO_DECL native_qp_t* create_qp(native_pd_t* pd,
                                 native_qp_init_attr const& qp_init_attr,
                                 asio::error_code& ec);

/// memory region ops
ASIO_DECL ULONG to_native_access_flag(mr_access_flag_t access_flag,
                                      int extra_access_flag);

// register memory region
ASIO_DECL native_mr_t* reg_mr(native_pd_t* pd, void* addr, size_t length,
                              mr_access_flag_t access_flag,
                              int extra_access_flag, asio::error_code& ec);

// deregister memory region
ASIO_DECL result_type dereg_mr(native_mr_t* mr, asio::error_code& ec);

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/detail/impl/nd_ops_verbs.ipp"
#endif
