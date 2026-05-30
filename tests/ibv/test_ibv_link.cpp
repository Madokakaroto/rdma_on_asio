// Compile/link smoke test for the ibv backend.
// Confirms libibverbs + librdmacm headers are found and symbols link.
// No RDMA hardware required: device list is queried and immediately freed.
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include <iostream>

int main() {
    int num_devices = 0;
    ibv_device** list = ibv_get_device_list(&num_devices);
    if (list) {
        std::cout << "ibv devices: " << num_devices << "\n";
        ibv_free_device_list(list);
    } else {
        std::cout << "ibv_get_device_list returned none (no RDMA hardware)\n";
    }

    // librdmacm symbol reference to ensure it links.
    rdma_event_channel* ch = rdma_create_event_channel();
    if (ch) {
        rdma_destroy_event_channel(ch);
    }
    return 0;
}
