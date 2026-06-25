#ifndef RDMA_IBV_IMPL_IBV_OPS_VERBS_IPP
#define RDMA_IBV_IMPL_IBV_OPS_VERBS_IPP

#include "rdma/ibv/detail/ibv_ops_verbs.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail::verbs_ops {

native_comp_channel_t* create_comp_channel(native_context_t* context,
                                           asio::error_code& ec) {
  native_comp_channel_t* channel = ::ibv_create_comp_channel(context);
  if (!channel) {
    ec = last_system_error();
    return nullptr;
  }
  int opt = ::fcntl(channel->fd, F_GETFL);
  if (opt == -1 || ::fcntl(channel->fd, F_SETFL, opt | O_NONBLOCK) == -1) {
    ec = last_system_error();
    ::ibv_destroy_comp_channel(channel);
    return nullptr;
  }
  ec.clear();
  return channel;
}

native_cq_t* create_cq(native_context_t* context, int cqe,
                       void* cq_context, native_comp_channel_t* channel,
                       int comp_vector, asio::error_code& ec) {
  native_cq_t* cq = ::ibv_create_cq(context, cqe, cq_context, channel,
                                    comp_vector);
  if (!cq) {
    ec = last_system_error();
    return nullptr;
  }
  ec.clear();
  return cq;
}

// rdma_create_qp creates the QP on cm_id (id->qp), on the cm_id's context.
int create_qp(native_cm_id_t* cm_id, native_pd_t* pd,
              native_qp_init_attr_t const& attr, asio::error_code& ec) {
  int const rc =
      ::rdma_create_qp(cm_id, pd, const_cast<native_qp_init_attr_t*>(&attr));
  if (rc) {
    ec = last_system_error();
  }
  else {
    ec.clear();
  }
  return rc;
}

int req_notify_cq(native_cq_t* cq, bool solicited_only,
                  asio::error_code& ec) {
  int const rc = ::ibv_req_notify_cq(cq, solicited_only ? 1 : 0);
  if (rc) {
    ec = last_system_error();
  }
  else {
    ec.clear();
  }
  return rc;
}

// Non-blocking (channel fd is O_NONBLOCK): returns 0 with cq/ctx on an event,
// -1 + EAGAIN when there is none.
int get_cq_event(native_comp_channel_t* channel, native_cq_t** out_cq,
                 void** out_ctx) {
  return ::ibv_get_cq_event(channel, out_cq, out_ctx);
}

void ack_cq_events(native_cq_t* cq, unsigned int n) {
  ::ibv_ack_cq_events(cq, n);
}

native_mr_t* reg_mr(native_pd_t* pd, void* addr, std::size_t length,
                    mr_acccess_flag_t flag, int extra_flag,
                    asio::error_code& ec) {
  // Default to a permissive access set covering send/recv + RDMA read/write.
  int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
               IBV_ACCESS_REMOTE_READ | extra_flag;
  switch (flag) {
    case mr_access_local_write:
      access = IBV_ACCESS_LOCAL_WRITE | extra_flag;
      break;
    case mr_access_remote_read:
      access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | extra_flag;
      break;
    case mr_access_remote_write:
      access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
               IBV_ACCESS_REMOTE_READ | extra_flag;
      break;
  }
  native_mr_t* mr = ::ibv_reg_mr(pd, addr, length, access);
  if (!mr) {
    ec = last_system_error();
    return nullptr;
  }
  ec.clear();
  return mr;
}

int dereg_mr(native_mr_t* mr, asio::error_code& ec) {
  int const rc = ::ibv_dereg_mr(mr);
  if (rc) {
    ec = make_system_error_code(rc);
  }
  else {
    ec.clear();
  }
  return rc;
}

// ibv_modify_qp wrapper. Returns the error_code (empty on success). Used for
// setup-time / control-plane QP attribute changes that rdma_cm does not expose
// through rdma_conn_param (e.g. min_rnr_timer applied once the QP is RTS). Never
// call on the data path.
asio::error_code modify_qp(native_qp_t* qp, native_qp_attr_t* attr,
                           int attr_mask) {
  int const rc = ::ibv_modify_qp(qp, attr, attr_mask);
  return rc ? make_system_error_code(rc) : asio::error_code{};
}

}  // namespace asio::rdma::detail::verbs_ops

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_IBV_IMPL_IBV_OPS_VERBS_IPP
