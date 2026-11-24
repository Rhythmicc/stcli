/* Standalone SYCL adapter. No upstream kernels or signing material.
 * Binding is blocked until the delivered SDK's zero-UUID defect is fixed.
 * This implements the documented name+memory contract for comparison only. */
#include "gpu_id_backend.h"
#include <sycl/sycl.hpp>
#include <cstdint>
#include <cstdio>
#include <string>
#include <exception>

extern "C" int gpu_id_query_sycl(Row *rows, int capacity, char *error, unsigned error_size) {
    try {
        const sycl::device dev{sycl::default_selector_v};
        if(!dev.is_gpu()) {
            std::snprintf(error,error_size,"SYCL selected a non-GPU device; use the same ONEAPI_DEVICE_SELECTOR as PanguLU.");
            return -1;
        }
        if(capacity<1) return -1;
        const std::string name=dev.get_info<sycl::info::device::name>();
        const auto memory=dev.get_info<sycl::info::device::global_mem_size>();
        const std::string input=name+std::to_string(static_cast<unsigned long long>(memory));
        std::uint64_t hash=1469598103934665603ULL;
        for(unsigned char ch:input) hash=(hash^ch)*1099511628211ULL;
        unsigned char bytes[16];
        for(unsigned i=0;i<16;++i) {
            bytes[i]=static_cast<unsigned char>(hash>>((i%8)*8));
            if(i%8==7) hash=hash*6364136223846793005ULL+1442695040888963407ULL;
        }
        Row row{};
        std::snprintf(row.backend,sizeof(row.backend),"sycl");
        std::snprintf(row.name,sizeof(row.name),"%s",name.c_str());
        std::snprintf(row.kind,sizeof(row.kind),"derived-name-memory");
        std::snprintf(row.id,sizeof(row.id),
            "sycl-%02x%02x%02x%02x-%02x%02x%02x%02x-%02x%02x%02x%02x-%02x%02x%02x%02x",
            bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],
            bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
        /* Do not emit an order-ready ID for the incompatible public SDK.
         * Remove this gate only after comparison with its actual verifier. */
        (void)rows;
        std::fprintf(stderr,"SYCL device: %s; memory: %llu bytes\n",name.c_str(),static_cast<unsigned long long>(memory));
        std::snprintf(error,error_size,"Published PanguLU SYCL SDK has a zero UUID instead of the documented derived identity. A corrected SDK is required before license IDs can be issued.");
        return -1;
    } catch(const std::exception &exc) {
        std::snprintf(error,error_size,"SYCL query failed: %s",exc.what());
        return -1;
    }
}
