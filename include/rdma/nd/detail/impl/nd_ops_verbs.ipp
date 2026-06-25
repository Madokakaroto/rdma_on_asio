#ifndef RDMA_ND_DETAIL_IMPL_ND_OPS_VERBS_IPP
#define RDMA_ND_DETAIL_IMPL_ND_OPS_VERBS_IPP

#include <cassert>
#include <memory>

#include "rdma/nd/detail/nd_ops_verbs.hpp"

#include "asio/detail/push_options.hpp"

namespace asio::rdma::detail::verbs_ops {

// simulate ibv_allocate_pd
native_pd_t* allocate_pd(native_context_t* context, asio::error_code& ec) {
  if (!context)
    [[unlikely]] {
    return nullptr;
  }

  unique_handle_t handle{create_overlapped_file(context, ec)};
  if (ec) {
    return nullptr;
  }

  auto new_pd = std::make_unique<native_pd_t>();
#ifndef __cpp_exceptions
  if (!new_pd)
  {
    ec = std::errc::not_enough_memory;
    return nullptr;
  }
#endif

  new_pd->context_ = context;
  new_pd->sync_handle_ = std::move(handle);
  return new_pd.release();
}

/// cq ops
// create cq
native_cq_t* create_cq(native_context_t* context, size_type cqe,
                       native_cq_init_attr const& init_attr,
                       asio::error_code& ec) {
  assert(context);
  native_cq_t* result{nullptr};
  auto const hr = context->CreateCompletionQueue(
      IID_IND2CompletionQueue, init_attr.overlapped_handle_, cqe,
      init_attr.processor_group_, init_attr.processor_affinity_,
      reinterpret_cast<LPVOID*>(std::addressof(result)));
  ec = static_cast<nd_errc>(hr);
  return result;
}

// notify cq
result_type notify_cq(native_cq_t* cq, native_cq_notify_attr const& attr,
                      asio::error_code& ec) {
  assert(cq);
  auto const hr = cq->Notify(attr.type_, attr.op_);
  if (hr != ND_SUCCESS && hr != ND_PENDING) {
    ec = static_cast<nd_errc>(hr);
  } else {
    ec.clear();
  }
  return hr;
}

/// qp ops
// create qp
native_qp_t* create_qp(native_pd_t* pd,
                       native_qp_init_attr const& qp_init_attr,
                       asio::error_code& ec) {
  assert(pd && pd->context_);
  native_qp_t* result{nullptr};
  auto const hr = pd->context_->CreateQueuePair(
      IID_IND2QueuePair, qp_init_attr.rcq_, qp_init_attr.icq_,
      qp_init_attr.qp_context_, qp_init_attr.max_recv_wr_,
      qp_init_attr.max_send_wr_, qp_init_attr.max_recv_sge_,
      qp_init_attr.max_send_sge_, qp_init_attr.max_inline_data_,
      reinterpret_cast<LPVOID*>(std::addressof(result)));
  ec = static_cast<nd_errc>(hr);
  return result;
}

/// memory region ops
ULONG to_native_access_flag(mr_acccess_flag_t access_flag,
                            int extra_access_flag) {
  ULONG native_access_flag = ND_MR_FLAG_ALLOW_LOCAL_WRITE |
                             ND_MR_FLAG_ALLOW_REMOTE_WRITE |
                             ND_MR_FLAG_ALLOW_REMOTE_READ |
                             static_cast<ULONG>(extra_access_flag);
  switch (access_flag) {
    case mr_access_local_write:
      native_access_flag = ND_MR_FLAG_ALLOW_LOCAL_WRITE |
                           static_cast<ULONG>(extra_access_flag);
      break;
    case mr_access_remote_read:
      native_access_flag = ND_MR_FLAG_ALLOW_LOCAL_WRITE |
                           ND_MR_FLAG_ALLOW_REMOTE_READ |
                           static_cast<ULONG>(extra_access_flag);
      break;
    case mr_access_remote_write:
      native_access_flag = ND_MR_FLAG_ALLOW_LOCAL_WRITE |
                           ND_MR_FLAG_ALLOW_REMOTE_WRITE |
                           ND_MR_FLAG_ALLOW_REMOTE_READ |
                           static_cast<ULONG>(extra_access_flag);
      break;
  }
  return native_access_flag;
}

// register memory region
native_mr_t* reg_mr(native_pd_t* pd, void* addr, size_t length,
                    mr_acccess_flag_t access_flag, int extra_access_flag,
                    asio::error_code& ec) {
  assert(pd && pd->context_);
  nd2_memory_region_ptr result{};
  // create memory region interface
  auto hr = pd->context_->CreateMemoryRegion(
      IID_IND2MemoryRegion, pd->sync_handle_.get(),
      reinterpret_cast<LPVOID*>(result.GetAddressOf()));
  ec = static_cast<nd_errc>(hr);
  if (ec) {
    return nullptr;
  }
  // sync register memory region
  assert(result);
  OVERLAPPED sync_ov{0};
  auto const native_access_flag =
      to_native_access_flag(access_flag, extra_access_flag);
  hr = result->Register(addr, length, native_access_flag, &sync_ov);
  switch (hr) {
    case ND_SUCCESS:
      ec.clear();
      break;
    case ND_PENDING:
      hr = result->GetOverlappedResult(&sync_ov, TRUE);
      [[fallthrough]];
    default:
      ec = static_cast<nd_errc>(hr);
      break;
  }
  if (ec) {
    return nullptr;
  }
  return result.Detach();
}

// deregister memory region
result_type dereg_mr(native_mr_t* mr, asio::error_code& ec) {
  assert(mr);
  OVERLAPPED sync_ov{0};
  auto hr = mr->Deregister(&sync_ov);
  switch (hr) {
    case ND_SUCCESS:
      break;
    case ND_PENDING:
      hr = mr->GetOverlappedResult(&sync_ov, TRUE);
      [[fallthrough]];
    default:
      ec = static_cast<nd_errc>(hr);
      break;
  }
  return hr;
}

}  // namespace asio::rdma::detail::verbs_ops

#include "asio/detail/pop_options.hpp"

#endif  // RDMA_ND_DETAIL_IMPL_ND_OPS_VERBS_IPP
