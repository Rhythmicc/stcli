/* Solved Tech CLI (stcli). Independent client-side implementation.
 * Uses installed vendor drivers; no network, signing keys or license issuance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include "gpu_id_backend.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#define API __stdcall
typedef HMODULE Lib;
static Lib library(const char *name) { return LoadLibraryExA(name, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32); }
#define symbol GetProcAddress
#define close_lib FreeLibrary
#else
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>
#define API
typedef void *Lib;
static Lib library(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
#define symbol dlsym
/* GPU runtimes can retain worker threads and C++ TLS destructor callbacks.
 * This short-lived query owns each loaded runtime until process exit: an
 * early dlclose can unmap code still referenced by those callbacks (HIP/DTK).
 * Keep normal exit/TLS cleanup and output error handling; do not use _Exit. */
static void close_lib(Lib lib) { (void)lib; }
#endif
#define VERSION "1.3.0"
#include "gpu_id_vendor.h"
#define LIMIT 256
/* S80 driver 5.1 writes a trailing NUL after its 16 UUID bytes.
 * Reserve scratch space without changing the 16-byte ID contract. */
typedef struct { unsigned char bytes[16]; unsigned char driver_padding[16]; } Uuid;
typedef int (API *Init)(unsigned);
typedef int (API *Count)(int *);
typedef int (API *Device)(int *, int);
typedef int (API *Name)(char *, int, int);
typedef int (API *Pci)(char *, int, int);
typedef int (API *GetUuid)(Uuid *, int);
typedef int (API *NvInit)(void);
typedef int (API *NvHandle)(const char *, void **);
typedef int (API *NvSerial)(void *, char *, unsigned);
/* Level Zero (Intel) dynamic ABI. Only used on non-Windows hosts; the layout
 * below matches the public oneAPI Level Zero headers. */
typedef struct { unsigned char id[16]; } ZeUuid;
typedef struct {
    uint32_t stype; void *pNext; uint32_t type; uint32_t vendorId; uint32_t deviceId;
    uint32_t flags; uint32_t subdeviceId; uint32_t coreClockRate; uint64_t maxMemAllocSize;
    uint32_t maxHardwareContexts; uint32_t maxCommandQueuePriority; uint32_t numThreadsPerEU;
    uint32_t physicalEUSimdWidth; uint32_t numEUsPerSubslice; uint32_t numSubslicesPerSlice;
    uint32_t numSlices; uint64_t timerResolution; uint32_t timestampValidBits;
    uint32_t kernelTimestampValidBits; ZeUuid uuid; char name[256];
} ZeDeviceProperties;
typedef struct { uint32_t domain, bus, device, function; } ZePciAddress;
typedef struct {
    uint32_t stype; void *pNext; ZePciAddress address;
    int32_t genVersion; int32_t width; int64_t maxBandwidth;
} ZePciProperties;
typedef int (API *ZeInit)(uint32_t);
typedef int (API *ZeDriverGet)(uint32_t *, void **);
typedef int (API *ZeDeviceGet)(void *, uint32_t *, void **);
typedef int (API *ZeDeviceGetProperties)(void *, ZeDeviceProperties *);
typedef int (API *ZeDevicePciGetPropertiesExt)(void *, ZePciProperties *);
/* OpenCL dynamic ABI (Lisuan). cl_platform_id / cl_device_id are pointers and
 * cl_device_type is a 64-bit bitfield; GPU is bit 2. */
typedef int (API *ClGetPlatformIDs)(uint32_t, void *, uint32_t *);
typedef int (API *ClGetDeviceIDs)(void *, uint64_t, uint32_t, void *, uint32_t *);
typedef int (API *ClGetDeviceInfo)(void *, uint32_t, size_t, void *, size_t *);
#define CL_DEVICE_TYPE_GPU 4u
#define CL_DEVICE_NAME 0x102Bu
#define CL_DEVICE_VENDOR 0x102Cu
#define CL_DEVICE_UUID_KHR 0x106Au
#define ZE_INIT_FLAG_GPU_ONLY 1u
#define ZE_RESULT_ERROR_UNINITIALIZED 0x78000001
#define ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES 3u
#define ZE_STRUCTURE_TYPE_PCI_EXT_PROPERTIES 0x10008u
static Row rows[LIMIT];
static int row_count, failures;
static void problem(const char *backend, const char *reason) {
    fprintf(stderr, "%s: %s\n", backend, reason); failures++;
}
static Lib first_library(const char *const *names) {
    for (int i=0; names[i]; i++) { Lib lib=library(names[i]); if(lib) return lib; }
    return NULL;
}
static void uuid_text(char *out, size_t size, const char *prefix, const Uuid *uuid) {
    snprintf(out, size, "%s-%02x%02x%02x%02x-%02x%02x%02x%02x-%02x%02x%02x%02x-%02x%02x%02x%02x", prefix,
        uuid->bytes[0],uuid->bytes[1],uuid->bytes[2],uuid->bytes[3],uuid->bytes[4],uuid->bytes[5],uuid->bytes[6],uuid->bytes[7],
        uuid->bytes[8],uuid->bytes[9],uuid->bytes[10],uuid->bytes[11],uuid->bytes[12],uuid->bytes[13],uuid->bytes[14],uuid->bytes[15]);
}
#ifndef _WIN32
/* Level Zero returns Intel's 128-bit UUID least-significant byte first; the
 * license inventory uses the canonical 8-4-4-4-12 form, so serialize it
 * most-significant byte first. */
static void uuid_canonical(char *out, size_t size, const Uuid *uuid) {
    const unsigned char *b = uuid->bytes;
    snprintf(out, size, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[15],b[14],b[13],b[12],b[11],b[10],b[9],b[8],b[7],b[6],b[5],b[4],b[3],b[2],b[1],b[0]);
}
#endif
static int uuid_nonzero(const Uuid *uuid) { for(int i=0;i<16;i++) if(uuid->bytes[i]) return 1; return 0; }
static int hip_uuid_placeholder(const Uuid *uuid) {
    const unsigned char *b=uuid->bytes;
    if((b[0]=='X' || b[0]=='x') && (b[1]=='X' || b[1]=='x')) {
        int tail_zero=1;
        for(int i=2;i<16;i++) if(b[i]) { tail_zero=0; break; }
        if(tail_zero) return 1;
    }
    if(!memcmp(b,"GPU-XX",6)) return 1;
    return 0;
}
static int pci_text(char *out, size_t size, const char *prefix, const char *pci) {
    unsigned domain,bus,device,function; char tail;
    if(sscanf(pci,"%x:%x:%x.%x%c",&domain,&bus,&device,&function,&tail)!=4 || domain>65535 || bus>255 || device>31 || function>7) return 0;
    snprintf(out,size,"%s-%04x:%02x:%02x",prefix,domain,bus,device); return 1;
}
static int usable_serial(const char *text) {
    if(!text[0] || !strcmp(text,"0") || !strcmp(text,"N/A") || !strcmp(text,"Unknown")) return 0;
    for(const unsigned char *p=(const unsigned char *)text;*p;p++)
        if(!isalnum(*p) && *p!='-' && *p!='_' && *p!='.' && *p!=':') return 0;
    return 1;
}
#ifndef _WIN32
static int contains_ci(const char *haystack, const char *needle) {
    size_t n=strlen(needle); if(!n) return 1;
    for(const char *p=haystack; *p; p++) {
        size_t i=0;
        while(i<n && p[i] && (tolower((unsigned char)p[i])==tolower((unsigned char)needle[i]))) i++;
        if(i==n) return 1;
    }
    return 0;
}
static void copy_text(char *dst, size_t dst_size, const char *src) {
    size_t i=0; while(i+1<dst_size && src[i]) { dst[i]=src[i]; i++; } dst[i]=0;
}
#endif
#ifdef __linux__
#ifndef GPU_ID_KFD_ROOT
#define GPU_ID_KFD_ROOT "/sys/class/kfd/kfd/topology/nodes"
#endif
#ifndef GPU_ID_KFD_DEV
#define GPU_ID_KFD_DEV "/dev/kfd"
#endif
#ifndef GPU_ID_NVCTL_DEV
#define GPU_ID_NVCTL_DEV "/dev/nvidiactl"
#endif
enum { ST_UNTRIED=0, ST_NO_LIB=1, ST_INIT_ERR=2, ST_API_ERR=3, ST_NO_DEV=4, ST_OK=5, ST_SKIPPED=6 };
typedef struct { int state; int err; } BackendState;
static BackendState bs_cuda, bs_musa, bs_hip, bs_hcr, bs_intel, bs_lisuan;
static BackendState *backend_state_slot(const char *backend) {
    if(!strcmp(backend,"cuda")) return &bs_cuda;
    if(!strcmp(backend,"musa")) return &bs_musa;
    if(!strcmp(backend,"hip")) return &bs_hip;
    if(!strcmp(backend,"hcr")) return &bs_hcr;
    if(!strcmp(backend,"intel")) return &bs_intel;
    if(!strcmp(backend,"lisuan")) return &bs_lisuan;
    return NULL;
}
static void set_backend_state(const char *backend, int state, int err) {
    BackendState *s=backend_state_slot(backend);
    if(s) { s->state=state; s->err=err; }
}
static int parse_bdf(const char *bdf, unsigned *domain, unsigned *bus, unsigned *device, unsigned *function) {
    char tail;
    if(sscanf(bdf,"%x:%x:%x.%x%c",domain,bus,device,function,&tail)!=4 ||
       *domain>65535 || *bus>255 || *device>31 || *function>7) return 0;
    return 1;
}
static int parse_unique_id_hex16(const char *text, unsigned char out[16]) {
    while(*text && isspace((unsigned char)*text)) text++;
    if(text[0]=='0' && (text[1]=='x' || text[1]=='X')) text+=2;
    int nonzero=0;
    for(int i=0;i<16;i++) {
        unsigned char c=(unsigned char)text[i];
        if(!isxdigit(c)) return 0;
        c=(unsigned char)tolower(c);
        if(c!='0') nonzero=1;
        out[i]=c;
    }
    const char *p=text+16;
    while(*p && isspace((unsigned char)*p)) p++;
    return (*p==0 && nonzero);
}
static int read_hip_unique_id(const char *bdf, Uuid *out) {
    unsigned domain,bus,device,function;
    if(!parse_bdf(bdf,&domain,&bus,&device,&function)) return 0;
    char path[512], text[64];
    int n=snprintf(path,sizeof(path),GPU_ID_PCI_ROOT "/%04x:%02x:%02x.%x/unique_id",domain,bus,device,function);
    if(n>0 && (size_t)n<sizeof(path)) {
        FILE *f=fopen(path,"r");
        if(f) {
            int ok=fgets(text,sizeof(text),f)!=NULL;
            fclose(f);
            if(ok) {
                memset(out,0,sizeof(*out));
                if(parse_unique_id_hex16(text,out->bytes)) return 1;
            }
        }
    }
    /* Secondary Linux kernel source: amdkfd topology properties (same 64-bit hardware unique_id). */
    DIR *dir=opendir(GPU_ID_KFD_ROOT);
    if(!dir) return 0;
    const unsigned long want_loc=((unsigned long)bus<<8) | ((unsigned long)device<<3) | (unsigned long)function;
    struct dirent *ent;
    int found=0;
    while(!found && (ent=readdir(dir))!=NULL) {
        if(!isdigit((unsigned char)ent->d_name[0])) continue;
        unsigned node_id=0; char tail;
        if(sscanf(ent->d_name,"%u%c",&node_id,&tail)!=1) continue;
        n=snprintf(path,sizeof(path),GPU_ID_KFD_ROOT "/%u/properties",node_id);
        if(n<=0 || (size_t)n>=sizeof(path)) continue;
        FILE *f=fopen(path,"r");
        if(!f) continue;
        int has_dom=0, has_loc=0, has_uid=0;
        unsigned long dom_val=0, loc_val=0;
        unsigned long long uid_val=0;
        char line[128];
        while(fgets(line,sizeof(line),f)) {
            unsigned long lv=0; unsigned long long uv=0;
            if(sscanf(line,"domain %lu",&lv)==1) { dom_val=lv; has_dom=1; }
            else if(sscanf(line,"location_id %lu",&lv)==1) { loc_val=lv; has_loc=1; }
            else if(sscanf(line,"unique_id %llu",&uv)==1) { uid_val=uv; has_uid=1; }
        }
        fclose(f);
        if(has_dom && has_loc && has_uid && dom_val==domain && loc_val==want_loc && uid_val!=0ULL) {
            char hex[20];
            snprintf(hex,sizeof(hex),"%016llx",uid_val);
            memset(out,0,sizeof(*out));
            memcpy(out->bytes,hex,16);
            found=1;
        }
    }
    closedir(dir);
    return found;
}
static Lib open_hip_ld_path(void) {
    const char *ld=getenv("LD_LIBRARY_PATH");
    if(!ld || !ld[0]) return NULL;
    static const char *const names[]={
        "libamdhip64.so","libamdhip64.so.7","libamdhip64.so.6","libamdhip64.so.5","libamdhip64.so.4",NULL
    };
    const char *p=ld;
    while(*p) {
        const char *colon=strchr(p,':');
        size_t len=colon?(size_t)(colon-p):strlen(p);
        while(len>1 && p[len-1]=='/') len--;
        if(len>=1 && len<=440) {
            for(int i=0;names[i];i++) {
                char path[512];
                int n=snprintf(path,sizeof(path),"%.*s/%s",(int)len,p,names[i]);
                if(n>0 && (size_t)n<sizeof(path)) {
                    Lib lib=library(path);
                    if(lib) return lib;
                }
            }
        }
        if(!colon) break;
        p=colon+1;
    }
    return NULL;
}
static Lib open_hip_env_root(const char *env_name) {
    const char *root=getenv(env_name);
    if(!root || root[0]!='/' || strstr(root,"..")) return NULL;
    size_t len=strlen(root);
    while(len>1 && root[len-1]=='/') len--;
    if(len<2 || len>384) return NULL;
    static const char *const rels[]={
        "lib/libamdhip64.so","lib/libamdhip64.so.7","lib/libamdhip64.so.6","lib/libamdhip64.so.5","lib/libamdhip64.so.4",
        "hip/lib/libamdhip64.so","hip/lib/libamdhip64.so.4","hip/lib/libamdhip64.so.5","hip/lib/libamdhip64.so.6","hip/lib/libamdhip64.so.7",
        NULL
    };
    for(int i=0;rels[i];i++) {
        char path[512];
        int n=snprintf(path,sizeof(path),"%.*s/%s",(int)len,root,rels[i]);
        if(n>0 && (size_t)n<sizeof(path)) {
            Lib lib=library(path);
            if(lib) return lib;
        }
    }
    return NULL;
}
#if defined(__linux__) && !defined(GPU_ID_SYCL)
static int read_pci_hex_u32(unsigned domain, unsigned bus, unsigned device, unsigned function, const char *attr, unsigned *out) {
    char path[512], text[32], *end=NULL;
    int n=snprintf(path,sizeof(path),GPU_ID_PCI_ROOT "/%04x:%02x:%02x.%x/%s",domain,bus,device,function,attr);
    if(n<=0 || (size_t)n>=sizeof(path)) return 0;
    FILE *f=fopen(path,"r");
    if(!f) return 0;
    int ok=fgets(text,sizeof(text),f)!=NULL;
    fclose(f);
    if(!ok) return 0;
    errno=0;
    unsigned long parsed=strtoul(text,&end,16);
    if(errno || end==text || parsed>0xffffffUL) return 0;
    while(isspace((unsigned char)*end)) end++;
    if(*end) return 0;
    *out=(unsigned)parsed;
    return 1;
}
typedef struct { int count; char first_bdf[16]; unsigned first_dev; } PciSeen;
static void record_pci_seen(PciSeen *seen, unsigned domain, unsigned bus, unsigned device, unsigned function, unsigned dev_id) {
    if(!seen->count) {
        snprintf(seen->first_bdf,sizeof(seen->first_bdf),"%04x:%02x:%02x.%x",domain,bus,device,function);
        seen->first_dev=dev_id;
    }
    seen->count++;
}
static int want_backend(const char *requested, const char *name) {
    return !strcmp(requested,"auto") || !strcmp(requested,name);
}
static void diagnose_hip_pci(const char *label, const char *vendor_hex, const PciSeen *seen, int is_hygon) {
    char msg[512];
    int kfd_missing=(access(GPU_ID_KFD_DEV,F_OK)!=0 && errno==ENOENT);
    int kfd_denied=(!kfd_missing && access(GPU_ID_KFD_DEV,R_OK|W_OK)!=0 && (errno==EACCES || errno==EPERM));
    int vis_set=(getenv("HIP_VISIBLE_DEVICES")!=NULL || getenv("ROCR_VISIBLE_DEVICES")!=NULL || getenv("GPU_DEVICE_ORDINAL")!=NULL);
    if(bs_hip.state==ST_NO_LIB) {
        if(is_hygon) {
            snprintf(msg,sizeof(msg),
                "detected %d %s GPU(s) on PCI (first: %s, PCI %s:%04x), but DTK/HIP runtime library (libamdhip64.so) was not found. Load the DTK environment or set LD_LIBRARY_PATH=/opt/dtk/hip/lib.",
                seen->count,label,seen->first_bdf,vendor_hex,seen->first_dev);
        } else {
            snprintf(msg,sizeof(msg),
                "detected %d %s GPU(s) on PCI (first: %s, PCI %s:%04x), but ROCm/HIP runtime library (libamdhip64.so) was not found. Install ROCm/HIP runtime, run inside your ROCm environment, or set LD_LIBRARY_PATH to the ROCm lib directory (e.g. LD_LIBRARY_PATH=/opt/rocm/lib).",
                seen->count,label,seen->first_bdf,vendor_hex,seen->first_dev);
        }
        problem("hip",msg);
    } else if(bs_hip.state==ST_NO_DEV || bs_hip.state==ST_INIT_ERR) {
        snprintf(msg,sizeof(msg),
            "detected %d %s GPU(s) on PCI (first: %s, PCI %s:%04x), but HIP reported no usable GPU (check %s runtime compatibility with this GPU architecture and driver configuration).",
            seen->count,label,seen->first_bdf,vendor_hex,seen->first_dev,is_hygon?"DTK":"ROCm");
        problem("hip",msg);
    }
    if(kfd_missing)
        problem("hip","/dev/kfd is missing; amdgpu/amdkfd kernel compute driver is not loaded or does not expose KFD.");
    else if(kfd_denied)
        problem("hip","/dev/kfd is not accessible (permission denied); add this user to the render/video group and re-login.");
    if(vis_set && (bs_hip.state==ST_NO_DEV || bs_hip.state==ST_INIT_ERR))
        problem("hip","GPU visibility filter is set (ROCR_VISIBLE_DEVICES / HIP_VISIBLE_DEVICES / GPU_DEVICE_ORDINAL); check that an allocated GPU is exposed.");
}
static void diagnose_linux_environment(const char *requested) {
    DIR *dir=opendir(GPU_ID_PCI_ROOT);
    if(!dir) return;
    PciSeen amd={0}, hygon={0}, nvidia={0}, musa={0}, metax={0}, intel={0};
    struct dirent *ent;
    while((ent=readdir(dir))!=NULL) {
        unsigned domain,bus,device,function,cls=0,ven=0,dev=0;
        if(!parse_bdf(ent->d_name,&domain,&bus,&device,&function)) continue;
        if(!read_pci_hex_u32(domain,bus,device,function,"class",&cls)) continue;
        unsigned base_cls=(cls>>16)&0xffu;
        if(base_cls!=0x03u && base_cls!=0x12u) continue;
        if(!read_pci_hex_u32(domain,bus,device,function,"vendor",&ven)) continue;
        (void)read_pci_hex_u32(domain,bus,device,function,"device",&dev);
        if(ven==0x1002u) record_pci_seen(&amd,domain,bus,device,function,dev);
        else if(ven==0x1d94u) record_pci_seen(&hygon,domain,bus,device,function,dev);
        else if(ven==0x10deu) record_pci_seen(&nvidia,domain,bus,device,function,dev);
        else if(ven==0x1ed5u) record_pci_seen(&musa,domain,bus,device,function,dev);
        else if(ven==0x9999u) record_pci_seen(&metax,domain,bus,device,function,dev);
        else if(ven==0x8086u) record_pci_seen(&intel,domain,bus,device,function,dev);
    }
    closedir(dir);
    char msg[512];
    if(amd.count && want_backend(requested,"hip") && bs_hip.state!=ST_OK) diagnose_hip_pci("AMD","1002",&amd,0);
    if(hygon.count && want_backend(requested,"hip") && bs_hip.state!=ST_OK) diagnose_hip_pci("Hygon","1d94",&hygon,1);
    if(nvidia.count && want_backend(requested,"cuda") && bs_cuda.state!=ST_OK) {
        if(bs_cuda.state==ST_NO_LIB) {
            snprintf(msg,sizeof(msg),
                "detected %d NVIDIA GPU(s) on PCI (first: %s, PCI 10de:%04x), but CUDA driver library (libcuda.so.1) was not found. Install the NVIDIA driver or run inside the GPU container runtime.",
                nvidia.count,nvidia.first_bdf,nvidia.first_dev);
            problem("cuda",msg);
        } else if(bs_cuda.state==ST_NO_DEV || bs_cuda.state==ST_INIT_ERR) {
            if(bs_cuda.state==ST_NO_DEV) {
                snprintf(msg,sizeof(msg),
                    "detected %d NVIDIA GPU(s) on PCI (first: %s, PCI 10de:%04x), but CUDA reported no usable GPU (check driver compatibility and device visibility).",
                    nvidia.count,nvidia.first_bdf,nvidia.first_dev);
                problem("cuda",msg);
            }
            if(access(GPU_ID_NVCTL_DEV,R_OK|W_OK)!=0 && (errno==EACCES || errno==EPERM))
                problem("cuda","/dev/nvidiactl is not accessible (permission denied); check GPU device permissions.");
            if(getenv("CUDA_VISIBLE_DEVICES")!=NULL)
                problem("cuda","CUDA_VISIBLE_DEVICES is set; verify that an allocated GPU is visible.");
        }
    }
    if(musa.count && want_backend(requested,"musa") && bs_musa.state==ST_NO_LIB) {
        snprintf(msg,sizeof(msg),
            "detected %d Moore Threads GPU(s) on PCI (first: %s, PCI 1ed5:%04x), but MUSA driver library (libmusa.so.1) was not found. Install the MUSA driver/runtime.",
            musa.count,musa.first_bdf,musa.first_dev);
        problem("musa",msg);
    }
    if(metax.count && want_backend(requested,"hcr") && bs_hcr.state==ST_NO_LIB) {
        snprintf(msg,sizeof(msg),
            "detected %d MetaX GPU(s) on PCI (first: %s, PCI 9999:%04x), but HPCC/HCR runtime library (libhcruntime.so) was not found in default paths or /opt/hpcc.",
            metax.count,metax.first_bdf,metax.first_dev);
        problem("hcr",msg);
    }
    int has_discrete=(amd.count || hygon.count || nvidia.count || musa.count || metax.count);
    if(intel.count && want_backend(requested,"intel") && bs_intel.state!=ST_OK && (!strcmp(requested,"intel") || !has_discrete)) {
        if(bs_intel.state==ST_NO_LIB) {
            snprintf(msg,sizeof(msg),
                "detected %d Intel GPU(s) on PCI (first: %s, PCI 8086:%04x), but Level Zero loader (libze_loader.so.1) was not found. Install level-zero / Intel GPU compute runtime.",
                intel.count,intel.first_bdf,intel.first_dev);
            problem("intel",msg);
        } else if(bs_intel.state==ST_SKIPPED) {
            snprintf(msg,sizeof(msg),
                "detected %d Intel GPU(s) on PCI (first: %s, PCI 8086:%04x) and Level Zero loader is installed, but the Intel GPU Level Zero runtime did not initialize (check Intel compute runtime and /dev/dri/renderD* permissions).",
                intel.count,intel.first_bdf,intel.first_dev);
            problem("intel",msg);
        }
    }
}
#endif
#else
static void set_backend_state(const char *backend, int state, int err) { (void)backend; (void)state; (void)err; }
enum { ST_UNTRIED=0, ST_NO_LIB=1, ST_INIT_ERR=2, ST_API_ERR=3, ST_NO_DEV=4, ST_OK=5, ST_SKIPPED=6 };
#endif
static void enumerate(const char *backend, int explicit_backend) {
#ifdef GPU_ID_SYCL
    if(!strcmp(backend,"sycl")) {
        char error[512]={0};
        int n=gpu_id_query_sycl(rows+row_count,LIMIT-row_count,error,sizeof(error));
        if(n<0) problem("sycl",error);
        else row_count+=n;
        return;
    }
#endif
    const int cuda=!strcmp(backend,"cuda"), musa=!strcmp(backend,"musa"), hcr=!strcmp(backend,"hcr"), hip=!strcmp(backend,"hip");
    /* The published HPCC library binds hc- IDs, not hcr- IDs. */
    const char *prefix=hcr?"hc":backend;
#ifdef _WIN32
    const char *const cuda_libs[]={"nvcuda.dll",NULL};
    const char *const musa_libs[]={"musa.dll",NULL};
    const char *const hip_libs[]={"amdhip64.dll",NULL};
    const char *const hcr_libs[]={"hcruntime.dll",NULL};
    const char *const nvml_libs[]={"nvml.dll",NULL};
    Lib lib=first_library(cuda?cuda_libs:musa?musa_libs:hcr?hcr_libs:hip_libs);
#else
    const char *const cuda_libs[]={"libcuda.so.1","libcuda.so",NULL};
    const char *const musa_libs[]={"libmusa.so.1","libmusa.so",NULL};
    const char *const hip_dyn_libs[]={"libamdhip64.so","libamdhip64.so.7","libamdhip64.so.6","libamdhip64.so.5","libamdhip64.so.4",NULL};
    const char *const hip_std_libs[]={
        "/opt/dtk/hip/lib/libamdhip64.so","/opt/dtk/lib/libamdhip64.so",
        "/opt/rocm/lib/libamdhip64.so","/opt/rocm/lib/libamdhip64.so.7","/opt/rocm/lib/libamdhip64.so.6",
        "/opt/rocm/lib/libamdhip64.so.5","/opt/rocm/lib/libamdhip64.so.4",
        "/opt/rocm/hip/lib/libamdhip64.so","/opt/rocm/hip/lib/libamdhip64.so.4",
        "/opt/rocm-4.5.2/hip/lib/libamdhip64.so","/opt/rocm-4.5.2/hip/lib/libamdhip64.so.4",NULL
    };
    const char *const hcr_libs[]={"libhcruntime.so","/opt/hpcc/lib/libhcruntime.so","/opt/hpcc/lib64/libhcruntime.so",NULL};
    const char *const nvml_libs[]={"libnvidia-ml.so.1",NULL};
    Lib lib=NULL;
    if(hip) {
#ifdef __linux__
        lib=open_hip_ld_path();
        if(!lib) lib=open_hip_env_root("ROCM_PATH");
        if(!lib) lib=open_hip_env_root("HIP_PATH");
#endif
        if(!lib) lib=first_library(hip_dyn_libs);
        if(!lib) lib=first_library(hip_std_libs);
    } else {
        lib=first_library(cuda?cuda_libs:musa?musa_libs:hcr_libs);
    }
#endif
    if(!lib) {
        set_backend_state(backend,ST_NO_LIB,0);
        if(explicit_backend) problem(backend,"driver library unavailable; install the matching GPU driver/runtime.");
        return;
    }
    Init init=(Init)symbol(lib,cuda?"cuInit":musa?"muInit":hcr?"hcInit":"hipInit");
    Count count=(Count)symbol(lib,cuda?"cuDeviceGetCount":musa?"muDeviceGetCount":hcr?"hcGetDeviceCount":"hipGetDeviceCount");
    Device device=(Device)symbol(lib,cuda?"cuDeviceGet":musa?"muDeviceGet":hcr?"hcDeviceGet":"hipDeviceGet");
    Name name=(Name)symbol(lib,cuda?"cuDeviceGetName":musa?"muDeviceGetName":hcr?"hcDeviceGetName":"hipDeviceGetName");
    Pci pci=(Pci)symbol(lib,cuda?"cuDeviceGetPCIBusId":musa?"muDeviceGetPCIBusId":hcr?"hcDeviceGetPCIBusId":"hipDeviceGetPCIBusId");
    /* Legacy cuDeviceGetUuid matches cudaDeviceProp.uuid used by PanguLU.
     * ROCm 4.5.2 (libamdhip64.so.4) predates hipDeviceGetUuid; on Linux HIP can
     * read the identical 64-bit hardware unique_id from amdgpu/amdkfd sysfs. */
    GetUuid uuid=(GetUuid)symbol(lib,cuda?"cuDeviceGetUuid":musa?"muDeviceGetUuid":hcr?"hcDeviceGetUuid":"hipDeviceGetUuid");
    if(!init || !count || !device || !name || !pci || (!hip && !uuid)) {
        set_backend_state(backend,ST_API_ERR,0);
        problem(backend,"required driver API unavailable; no identifier guessed.");
        close_lib(lib); return;
    }
    int n=0,rc=init(0);
    if(rc || (rc=count(&n))) {
        char msg[96]; snprintf(msg,sizeof(msg),"GPU initialization failed (error %d); check driver and GPU allocation.",rc);
        set_backend_state(backend,ST_INIT_ERR,rc);
        problem(backend,msg); close_lib(lib); return;
    }
    if(n<0 || n>LIMIT-row_count) { set_backend_state(backend,ST_INIT_ERR,n); problem(backend,"device count out of bounds (maximum 256)."); close_lib(lib); return; }
    if(!n) {
        set_backend_state(backend,ST_NO_DEV,0);
        if(explicit_backend) problem(backend,"no visible GPU; check allocation and visibility settings.");
        close_lib(lib); return;
    }
    set_backend_state(backend,ST_OK,0);
    Lib nvml=NULL; NvInit nv_shutdown=NULL; NvHandle handle=NULL; NvSerial serial=NULL;
    if(cuda) {
        nvml=first_library(nvml_libs);
#ifdef _WIN32
        if(!nvml) {
            char path[1024]; DWORD len=GetEnvironmentVariableA("ProgramW6432",path,sizeof(path));
            const char *suffix="\\NVIDIA Corporation\\NVSMI\\nvml.dll";
            if(len>2 && len+strlen(suffix)<sizeof(path) && path[1]==':' && path[2]=='\\') {
                strcat(path,suffix); nvml=LoadLibraryExA(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            }
        }
#endif
        if(nvml) {
            NvInit nv_init=(NvInit)symbol(nvml,"nvmlInit_v2");
            nv_shutdown=(NvInit)symbol(nvml,"nvmlShutdown");
            handle=(NvHandle)symbol(nvml,"nvmlDeviceGetHandleByPciBusId_v2");
            serial=(NvSerial)symbol(nvml,"nvmlDeviceGetSerial");
            if(!nv_init || !nv_shutdown || !handle || !serial || nv_init()) { close_lib(nvml); nvml=NULL; }
        }
    }
    for(int ordinal=0;ordinal<n;ordinal++) {
        int dev; Row row={0}; Uuid value={0};
        row.ordinal=ordinal; snprintf(row.backend,sizeof(row.backend),"%s",backend);
        if(device(&dev,ordinal)) { problem(backend,"cannot access a visible device."); continue; }
        if(name(row.name,sizeof(row.name)-1,dev)) strcpy(row.name,"Unknown GPU");
        const int pci_ok=pci(row.pci,sizeof(row.pci)-1,dev)==0;
        if(cuda && nvml && pci_ok) {
            void *gpu=NULL;
            if(!handle(row.pci,&gpu) && !serial(gpu,row.id,sizeof(row.id)-1) && usable_serial(row.id)) strcpy(row.kind,"hardware-serial");
            else row.id[0]=0;
        }
        if(!row.id[0] && hip) {
            int got_hip_id=0;
            if(uuid) {
                int uuid_rc=uuid(&value,dev);
                if(!uuid_rc && uuid_nonzero(&value) && !hip_uuid_placeholder(&value)) {
                    uuid_text(row.id,sizeof(row.id),prefix,&value); strcpy(row.kind,"uuid");
                    got_hip_id=1;
                }
            }
#ifdef __linux__
            if(!got_hip_id && pci_ok && read_hip_unique_id(row.pci,&value)) {
                uuid_text(row.id,sizeof(row.id),prefix,&value); strcpy(row.kind,"sysfs-unique-id");
                got_hip_id=1;
            }
#endif
            if(!got_hip_id) {
                problem("hip",uuid
                    ? "cannot read a hardware-unique GPU ID (hipDeviceGetUuid failed/invalid and kernel sysfs unique_id is absent or zero); PCI address cannot uniquely identify a GPU card."
                    : "cannot read a hardware-unique GPU ID (hipDeviceGetUuid unavailable in this ROCm/HIP runtime and kernel sysfs unique_id is absent or zero); PCI address cannot uniquely identify a GPU card.");
                continue;
            }
        }
        if(!row.id[0]) {
            int uuid_rc=uuid(&value,dev);
            if(!uuid_rc && (!(musa || hcr) || uuid_nonzero(&value))) {
                uuid_text(row.id,sizeof(row.id),prefix,&value); strcpy(row.kind,"uuid");
            } else if(musa && pci_ok && pci_text(row.id,sizeof(row.id),prefix,row.pci)) strcpy(row.kind,"pci-fallback");
            else { problem(backend,"cannot read a license-compatible identifier; no value guessed."); continue; }
        }
        rows[row_count++]=row;
    }
    if(nvml) { nv_shutdown(); close_lib(nvml); }
    close_lib(lib);
}
static void enumerate_intel(int explicit_backend) {
#ifdef _WIN32
    if(explicit_backend) problem("intel","Intel Level Zero backend requires Linux.");
    return;
#else
    const char *const libs[]={"libze_loader.so.1","libze_loader.so",NULL};
    Lib lib=first_library(libs);
    if(!lib) { set_backend_state("intel",ST_NO_LIB,0); if(explicit_backend) problem("intel","Level Zero loader unavailable; install intel-gpu-tools / level-zero."); return; }
    ZeInit init=(ZeInit)symbol(lib,"zeInit");
    ZeDriverGet driver_get=(ZeDriverGet)symbol(lib,"zeDriverGet");
    ZeDeviceGet device_get=(ZeDeviceGet)symbol(lib,"zeDeviceGet");
    ZeDeviceGetProperties device_properties=(ZeDeviceGetProperties)symbol(lib,"zeDeviceGetProperties");
    ZeDevicePciGetPropertiesExt pci_properties=(ZeDevicePciGetPropertiesExt)symbol(lib,"zeDevicePciGetPropertiesExt");
    if(!init || !driver_get || !device_get || !device_properties) { set_backend_state("intel",ST_API_ERR,0); problem("intel","required Level Zero API unavailable; no identifier read."); close_lib(lib); return; }
    int rc=init(ZE_INIT_FLAG_GPU_ONLY);
    /* A Level Zero loader can be installed on a CUDA-only node. Its lack of
     * an initialized Intel driver is normal during discovery, not a failed
     * read of an enumerated GPU. Keep explicit Intel requests strict and do
     * not suppress other initialization or per-device errors. */
    if(rc==ZE_RESULT_ERROR_UNINITIALIZED && !explicit_backend) { set_backend_state("intel",ST_SKIPPED,rc); close_lib(lib); return; }
    if(rc) {
        char msg[96]; snprintf(msg,sizeof(msg),"Level Zero initialization failed (error %d); check driver and GPU allocation.",rc);
        set_backend_state("intel",ST_INIT_ERR,rc);
        problem("intel",msg); close_lib(lib); return;
    }
    uint32_t driver_count=0;
    if(driver_get(&driver_count,NULL) || driver_count==0) { set_backend_state("intel",ST_NO_DEV,0); if(explicit_backend) problem("intel","no Level Zero driver reported; check GPU allocation."); close_lib(lib); return; }
    set_backend_state("intel",ST_OK,0);
    if(driver_count>LIMIT) driver_count=LIMIT;
    void **drivers=(void **)calloc(driver_count,sizeof(void *));
    if(!drivers) { problem("intel","out of memory."); close_lib(lib); return; }
    if(driver_get(&driver_count,drivers)) { problem("intel","cannot enumerate Level Zero drivers."); free(drivers); close_lib(lib); return; }
    for(uint32_t d=0; d<driver_count && row_count<LIMIT; d++) {
        uint32_t device_count=0;
        if(device_get(drivers[d],&device_count,NULL)) continue;
        uint32_t remaining=(uint32_t)(LIMIT-row_count);
        if(device_count>remaining) device_count=remaining;
        void **devices=(void **)calloc(device_count?device_count:1,sizeof(void *));
        if(!devices) { problem("intel","out of memory."); continue; }
        uint32_t fetched=device_count;
        if(fetched && device_get(drivers[d],&fetched,devices)) { free(devices); continue; }
        for(uint32_t i=0;i<fetched;i++) {
            ZeDeviceProperties props; memset(&props,0,sizeof(props)); props.stype=ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES;
            if(device_properties(devices[i],&props)) { problem("intel","cannot read device properties."); continue; }
            Uuid value; memset(&value,0,sizeof(value)); memcpy(value.bytes,props.uuid.id,16);
            if(!uuid_nonzero(&value)) { problem("intel","Level Zero returned a zero device UUID; no identifier read."); continue; }
            Row row={0}; row.ordinal=(int)i; snprintf(row.backend,sizeof(row.backend),"intel");
            copy_text(row.name,sizeof(row.name),props.name[0]?props.name:"Intel GPU");
            strcpy(row.kind,"level-zero-uuid");
            uuid_canonical(row.id,sizeof(row.id),&value);
            if(pci_properties) {
                ZePciProperties pci; memset(&pci,0,sizeof(pci)); pci.stype=ZE_STRUCTURE_TYPE_PCI_EXT_PROPERTIES;
                if(!pci_properties(devices[i],&pci))
                    snprintf(row.pci,sizeof(row.pci),"%04x:%02x:%02x.%x",pci.address.domain,pci.address.bus,pci.address.device,pci.address.function);
            }
            rows[row_count++]=row;
        }
        free(devices);
    }
    free(drivers);
    close_lib(lib);
#endif
}
static void enumerate_lisuan(int explicit_backend) {
#ifdef _WIN32
    if(explicit_backend) problem("lisuan","OpenCL backend requires Linux.");
    return;
#else
    const char *const libs[]={"libOpenCL.so.1","libOpenCL.so",NULL};
    Lib lib=first_library(libs);
    if(!lib) { set_backend_state("lisuan",ST_NO_LIB,0); if(explicit_backend) problem("lisuan","OpenCL loader unavailable; install the Lisuan OpenCL runtime."); return; }
    ClGetPlatformIDs get_platforms=(ClGetPlatformIDs)symbol(lib,"clGetPlatformIDs");
    ClGetDeviceIDs get_devices=(ClGetDeviceIDs)symbol(lib,"clGetDeviceIDs");
    ClGetDeviceInfo get_info=(ClGetDeviceInfo)symbol(lib,"clGetDeviceInfo");
    if(!get_platforms || !get_devices || !get_info) { set_backend_state("lisuan",ST_API_ERR,0); problem("lisuan","required OpenCL API unavailable; no identifier read."); close_lib(lib); return; }
    uint32_t platform_count=0;
    if(get_platforms(0,NULL,&platform_count) || platform_count==0) { set_backend_state("lisuan",ST_NO_DEV,0); if(explicit_backend) problem("lisuan","no OpenCL platform found; install the Lisuan runtime."); close_lib(lib); return; }
    if(platform_count>LIMIT) platform_count=LIMIT;
    void **platforms=(void **)calloc(platform_count,sizeof(void *));
    if(!platforms) { problem("lisuan","out of memory."); close_lib(lib); return; }
    if(get_platforms(platform_count,platforms,NULL)) { problem("lisuan","cannot enumerate OpenCL platforms."); free(platforms); close_lib(lib); return; }
    int found_lisuan=0;
    for(uint32_t p=0;p<platform_count && row_count<LIMIT;p++) {
        uint32_t device_count=0;
        if(get_devices(platforms[p],CL_DEVICE_TYPE_GPU,0,NULL,&device_count) || device_count==0) continue;
        uint32_t remaining=(uint32_t)(LIMIT-row_count);
        if(device_count>remaining) device_count=remaining;
        void **devices=(void **)calloc(device_count?device_count:1,sizeof(void *));
        if(!devices) { problem("lisuan","out of memory."); continue; }
        uint32_t fetched=device_count;
        if(fetched && get_devices(platforms[p],CL_DEVICE_TYPE_GPU,fetched,devices,NULL)) { free(devices); continue; }
        for(uint32_t i=0;i<fetched;i++) {
            char vendor[ID_TEXT]={0}; size_t vendor_size=0;
            if(get_info(devices[i],CL_DEVICE_VENDOR,sizeof(vendor),vendor,&vendor_size)) vendor[0]=0;
            if(!contains_ci(vendor,"lisuan")) continue;
            found_lisuan=1;
            char name[ID_TEXT]={0}; size_t name_size=0;
            if(get_info(devices[i],CL_DEVICE_NAME,sizeof(name),name,&name_size)) strcpy(name,"Lisuan GPU");
            unsigned char ub[16]={0}; size_t uuid_size=0;
            (void)get_info(devices[i],CL_DEVICE_UUID_KHR,sizeof(ub),ub,&uuid_size);
            Uuid value; memset(&value,0,sizeof(value)); memcpy(value.bytes,ub,16);
            Row row={0}; row.ordinal=(int)i; snprintf(row.backend,sizeof(row.backend),"lisuan");
            snprintf(row.name,sizeof(row.name),"%s",name);
            strcpy(row.kind,"opencl-uuid");
            uuid_text(row.id,sizeof(row.id),"lisuan",&value);
            rows[row_count++]=row;
        }
        free(devices);
    }
    free(platforms);
    set_backend_state("lisuan",found_lisuan?ST_OK:ST_NO_DEV,0);
    if(explicit_backend && !found_lisuan) problem("lisuan","no Lisuan OpenCL device found; check the runtime and allocation.");
    close_lib(lib);
#endif
}
static void json_string(FILE *out,const char *s) {
    fputc('"',out); for(const unsigned char *p=(const unsigned char*)s;*p;p++) {
        if(*p=='"' || *p=='\\') fprintf(out,"\\%c",*p);
        else if(*p<32 || *p>=127) fprintf(out,"\\u%04x",*p);
        else fputc(*p,out);
    } fputc('"',out);
}
static int st_use_color(void) {
    static int cached=-1;
    if(cached>=0) return cached;
    const char *no_color=getenv("NO_COLOR");
    if(no_color && *no_color) { cached=0; return 0; }
#ifdef _WIN32
    if(!_isatty(_fileno(stdout))) { cached=0; return 0; }
    HANDLE h=GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode=0;
    if(h==INVALID_HANDLE_VALUE || h==NULL || !GetConsoleMode(h,&mode) || !SetConsoleMode(h,mode | 0x0004u)) {
        cached=0; return 0;
    }
    cached=1; return 1;
#else
    const char *term=getenv("TERM");
    if(!isatty(STDOUT_FILENO) || (term && !strcmp(term,"dumb"))) { cached=0; return 0; }
    cached=1; return 1;
#endif
}
#include "gpu_id_license.h"
static void st_sanitize_display_name(const char *src, char *dst, size_t cap) {
    if (!cap) return;
    size_t pos = 0;
    int last_space = 1;
    if (src) {
        for (const unsigned char *p = (const unsigned char *)src; *p && pos + 1 < cap; p++) {
            unsigned char c = *p;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
                if (!last_space) {
                    dst[pos++] = ' ';
                    last_space = 1;
                }
            } else if (c < 32u || c == 127u) {
                continue;
            } else {
                dst[pos++] = (c == '|') ? '/' : (char)c;
                last_space = 0;
            }
        }
    }
    while (pos > 0 && dst[pos - 1] == ' ') pos--;
    dst[pos] = '\0';
    if (!pos) {
        snprintf(dst, cap, "%s", "Unknown GPU");
    }
}
static size_t st_append_ascii_lower(char *dst, size_t cap, size_t pos, const char *s) {
    if (!s) return pos;
    while (*s && pos + 1 < cap) {
        unsigned char c = (unsigned char)*s++;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        dst[pos++] = (char)c;
    }
    return pos;
}
static size_t st_append_norm_name(char *dst, size_t cap, size_t pos, const char *s) {
    if (!s) return pos;
    size_t start = pos;
    int last_space = 1;
    for (const unsigned char *p = (const unsigned char *)s; *p && pos + 1 < cap; p++) {
        unsigned char c = *p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!last_space) {
                dst[pos++] = ' ';
                last_space = 1;
            }
        } else {
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
            dst[pos++] = (char)c;
            last_space = 0;
        }
    }
    while (pos > start && dst[pos - 1] == ' ') pos--;
    return pos;
}
static void st_format_b_checksum(const char *backend, const char *vendor, const char *display_name, const char *license_id, char out[5]) {
    static const char ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    char payload[1024];
    size_t pos = 0;
    pos = st_append_ascii_lower(payload, sizeof(payload), pos, backend);
    if (pos + 1 < sizeof(payload)) payload[pos++] = '\n';
    pos = st_append_ascii_lower(payload, sizeof(payload), pos, vendor);
    if (pos + 1 < sizeof(payload)) payload[pos++] = '\n';
    pos = st_append_norm_name(payload, sizeof(payload), pos, display_name);
    if (pos + 1 < sizeof(payload)) payload[pos++] = '\n';
    if (license_id) {
        const char *start = license_id;
        while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
        size_t len = strlen(start);
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t' || start[len - 1] == '\r' || start[len - 1] == '\n')) len--;
        for (size_t i = 0; i < len && pos + 1 < sizeof(payload); i++) {
            payload[pos++] = start[i];
        }
    }
    payload[pos] = '\0';
    uint32_t crc = st_crc32((const unsigned char *)payload, pos);
    uint32_t v = (crc >> 12) & 0xFFFFFu;
    out[0] = ALPHABET[(v >> 15) & 31u];
    out[1] = ALPHABET[(v >> 10) & 31u];
    out[2] = ALPHABET[(v >> 5) & 31u];
    out[3] = ALPHABET[v & 31u];
    out[4] = '\0';
}
static void usage(void) {
    int c=st_use_color();
    const char *b=c?"\033[1m":"", *d=c?"\033[2m":"", *cy=c?"\033[36m":"", *r=c?"\033[0m":"";
    printf("%sstcli%s %s%s%s\n\n"
           "%sUsage%s\n"
           "  stcli <command> [options]\n\n"
           "%sCommands%s\n"
           "  %sgpu-id%s   Collect GPU license IDs\n"
           "  %sapply%s    Install license code\n"
           "  %slist%s     List installed licenses\n\n"
           "%sRun 'stcli <command> --help' for options.%s\n",
           b,r,d,VERSION,r,
           b,r,
           b,r,
           cy,r,cy,r,cy,r,
           d,r);
}
static void usage_gpu_id(void) {
    int c=st_use_color();
    const char *b=c?"\033[1m":"", *d=c?"\033[2m":"", *r=c?"\033[0m":"";
    printf("%sUsage:%s stcli gpu-id [--backend auto|cuda|musa|hip|hcr|intel|lisuan]\n\n"
           "Collect GPU license IDs on the target host.\n\n"
           "%sOptions%s\n"
           "  --backend NAME   Select driver backend %s(default: auto)%s\n",
           b,r,b,r,d,r);
}
int main(int argc,char **argv) {
    int pause_console=0; const char *backend="auto";
#ifdef GPU_ID_SYCL
    backend="sycl";
#endif
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    DWORD processes[2]; pause_console=argc==1 && GetConsoleProcessList(processes,2)==1 && _isatty(_fileno(stdout));
#endif
    if(argc==1) {
        usage();
        if(pause_console) { fputs("Press Enter to close...",stderr); fflush(stderr); (void)getchar(); }
        return 0;
    }
    if(argc>=2 && !strcmp(argv[1],"help")) { usage(); return 0; }
    if(argc>=2 && !strcmp(argv[1],"apply")) return cmd_apply(argc-2,argv+2);
    if(argc>=2 && !strcmp(argv[1],"list")) return cmd_list(argc-2,argv+2);
    int is_gpu_id_sub=(argc>=2 && !strcmp(argv[1],"gpu-id"));
    int arg_start=is_gpu_id_sub?2:1;
    for(int i=arg_start;i<argc;i++) {
        if(!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h")) { if(is_gpu_id_sub) usage_gpu_id(); else usage(); return 0; }
        else if(!strcmp(argv[i],"--version")) { puts(VERSION); return 0; }
        else if(!strcmp(argv[i],"--backend") && i+1<argc) backend=argv[++i];
        else if(!strcmp(argv[i],"--no-pause")) pause_console=0;
        else { fprintf(stderr,"Invalid option: %s. Use --help.\n",argv[i]); return 2; }
    }
#ifdef GPU_ID_SYCL
    if(strcmp(backend,"auto") && strcmp(backend,"sycl")) { fputs("This build supports --backend sycl or auto.\n",stderr); return 2; }
    enumerate("sycl",1);
#else
    if(strcmp(backend,"auto") && strcmp(backend,"cuda") && strcmp(backend,"musa") && strcmp(backend,"hip") && strcmp(backend,"hcr") && strcmp(backend,"intel") && strcmp(backend,"lisuan")) { fputs("Unknown backend. Use auto, cuda, musa, hip, hcr, intel or lisuan.\n",stderr); return 2; }
    const char *backends[]={"cuda","musa","hip","hcr","intel","lisuan"};
    for(int i=0;i<6;i++) {
        if(strcmp(backend,"auto") && strcmp(backend,backends[i])) continue;
        int explicit_backend=strcmp(backend,"auto")!=0;
        if(!strcmp(backends[i],"intel")) enumerate_intel(explicit_backend);
        else if(!strcmp(backends[i],"lisuan")) enumerate_lisuan(explicit_backend);
        else enumerate(backends[i],explicit_backend);
    }
#endif
    for(int i=0;i<row_count;i++) vendor_metadata(&rows[i]);
    for(int i=0;i<row_count;i++) for(int j=0;j<i;j++) if(!strcmp(rows[i].id,rows[j].id)) {
        problem(rows[i].backend,"duplicate license IDs (shared board/MIG); contact support before ordering."); break;
    }
    if(!row_count) {
#if defined(__linux__) && !defined(GPU_ID_SYCL)
        diagnose_linux_environment(backend);
#endif
        if(!failures)
            problem("GPU","no license IDs found. Install drivers and run on the allocated GPU host.");
        else
            problem("GPU","no license IDs collected; resolve the environment or driver errors above and retry.");
    }
    if(failures) {
        if(pause_console) { fputs("Press Enter to close...",stderr); fflush(stderr); (void)getchar(); }
        return 1;
    }
    for(int i=0;i<row_count;i++) {
        Row *r=&rows[i];
        char clean_name[ID_TEXT];
        char checksum[5];
        st_sanitize_display_name(r->name,clean_name,sizeof(clean_name));
        st_format_b_checksum(r->backend,r->vendor,clean_name,r->id,checksum);
        if(!strcmp(r->backend,"hip")) {
            printf("# hip | %s | %s\n%s [%s]\n",r->vendor,clean_name,r->id,checksum);
        } else {
            printf("# %s | %s\n%s [%s]\n",r->backend,clean_name,r->id,checksum);
        }
    }
    if(ferror(stdout) || fflush(stdout)) { fputs("Failed to write output.\n",stderr); return 2; }
    if(pause_console) { fputs("Press Enter to close...",stderr); fflush(stderr); (void)getchar(); }
    return 0;
}
