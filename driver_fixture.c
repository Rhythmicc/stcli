/* Public, synthetic test driver. Never install into system driver directories. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
static int number(const char *name,int fallback) { const char *s=getenv(name); return s?atoi(s):fallback; }
/* Model a runtime which registers a callback in an independently resident
 * dispatcher without retaining the module containing that callback. */
extern void fixture_register_late_cleanup(void (*callback)(void)) __attribute__((weak));
static void fixture_cleanup(void) {
    const char *path=getenv("FIXTURE_EXIT_MARKER");
    FILE *out=path?fopen(path,"a"):NULL;
    if(out) { fputs("cleanup\n",out); fclose(out); }
}
int cuInit(unsigned x) {
    (void)x;
    if(getenv("FIXTURE_LATE_CLEANUP") && fixture_register_late_cleanup)
        fixture_register_late_cleanup(fixture_cleanup);
    return number("FIXTURE_INIT",0);
}
int cuDeviceGetCount(int *n) { *n=number("FIXTURE_COUNT",2); return 0; }
int cuDeviceGet(int *d,int n) { *d=n; return 0; }
int cuDeviceGetName(char *s,int size,int dev) { (void)dev; snprintf(s,size,"%s",getenv("FIXTURE_NAME")?getenv("FIXTURE_NAME"):"Fixture \"GPU\""); return 0; }
int cuDeviceGetPCIBusId(char *s,int size,int dev) { if(getenv("FIXTURE_PCI")) snprintf(s,size,"%s",getenv("FIXTURE_PCI")); else snprintf(s,size,"0000:%02x:00.0",dev+1); return 0; }
int cuDeviceGetUuid(void *p,int dev) {
    if(number("FIXTURE_UUID_ERROR",0)) return 1;
    unsigned char *b=p;
    const char *ascii=getenv("FIXTURE_UUID_ASCII");
    if(number("FIXTURE_HIP_PLACEHOLDER_UUID",0)) {
        memset(b,0,16); b[0]='X'; b[1]='X';
    } else if(ascii && strlen(ascii)==16) {
        memcpy(b,ascii,16);
    } else {
        for(int i=0;i<16;i++) b[i]=number("FIXTURE_ZERO_UUID",0)?0:(unsigned char)(i+dev);
    }
    if(number("FIXTURE_UUID_TRAILING_NUL",0)) b[16]=0;
    return 0;
}
int nvmlInit_v2(void) { return 0; }
int nvmlShutdown(void) { return 0; }
int nvmlDeviceGetHandleByPciBusId_v2(const char *s,void **p) { *p=(void *)(size_t)(s[6]-'0'); return 0; }
int nvmlDeviceGetSerial(void *p,char *s,unsigned size) {
    const char *fixed=getenv("FIXTURE_SERIAL");
    if(fixed) snprintf(s,size,"%s",fixed);
    else snprintf(s,size,"0000123%zu",(size_t)p);
    return 0;
}
/* Level Zero synthetic ABI (Intel). Layout mirrors the public oneAPI headers. */
typedef struct { unsigned char id[16]; } FixtureZeUuid;
typedef struct {
    uint32_t stype; void *pNext; uint32_t type; uint32_t vendorId; uint32_t deviceId;
    uint32_t flags; uint32_t subdeviceId; uint32_t coreClockRate; uint64_t maxMemAllocSize;
    uint32_t maxHardwareContexts; uint32_t maxCommandQueuePriority; uint32_t numThreadsPerEU;
    uint32_t physicalEUSimdWidth; uint32_t numEUsPerSubslice; uint32_t numSubslicesPerSlice;
    uint32_t numSlices; uint64_t timerResolution; uint32_t timestampValidBits;
    uint32_t kernelTimestampValidBits; FixtureZeUuid uuid; char name[256];
} FixtureZeDeviceProperties;
typedef struct { uint32_t domain, bus, device, function; } FixtureZePciAddress;
typedef struct {
    uint32_t stype; void *pNext; FixtureZePciAddress address;
    int32_t genVersion; int32_t width; int64_t maxBandwidth;
} FixtureZePciProperties;
int zeInit(uint32_t flags) { (void)flags; return number("FIXTURE_ZE_INIT",number("FIXTURE_INIT",0)); }
int zeDriverGet(uint32_t *count, void **drivers) {
    if(!count) return 1;
    uint32_t n=1;
    if(drivers) drivers[0]=(void *)(size_t)0x100;
    else *count=n;
    return 0;
}
int zeDeviceGet(void *driver, uint32_t *count, void **devices) {
    (void)driver;
    if(!count) return 1;
    uint32_t n=(uint32_t)number("FIXTURE_COUNT",1);
    if(devices) { for(uint32_t i=0;i<n;i++) devices[i]=(void *)(size_t)(0x1000+i); }
    else *count=n;
    return 0;
}
int zeDeviceGetProperties(void *device, FixtureZeDeviceProperties *props) {
    (void)device;
    if(!props) return 1;
    memset(props,0,sizeof(*props));
    props->type=1;
    snprintf(props->name,sizeof(props->name),"Fixture \"Intel GPU\"");
    if(number("FIXTURE_ZERO_UUID",0)) memset(props->uuid.id,0,sizeof(props->uuid.id));
    else {
        const unsigned char fixed[16]={0x86,0x80,0x0b,0xe2,0x00,0x00,0x00,0x00,
                                        0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
        memcpy(props->uuid.id,fixed,16);
    }
    return 0;
}
int zeDevicePciGetPropertiesExt(void *device, FixtureZePciProperties *pci) {
    (void)device;
    if(!pci) return 1;
    memset(pci,0,sizeof(*pci));
    pci->address.domain=0; pci->address.bus=3; pci->address.device=0; pci->address.function=0;
    return 0;
}
/* OpenCL synthetic ABI (Lisuan). cl_device_type GPU is bit 2. */
#define FIXTURE_CL_DEVICE_TYPE_GPU 4u
int clGetPlatformIDs(uint32_t num_entries, void **platforms, uint32_t *num_platforms) {
    uint32_t n=1;
    if(platforms && num_entries>0) platforms[0]=(void *)(size_t)0x200;
    if(num_platforms) *num_platforms=n;
    return 0;
}
int clGetDeviceIDs(void *platform, uint64_t device_type, uint32_t num_entries, void **devices, uint32_t *num_devices) {
    (void)platform;
    if(device_type != FIXTURE_CL_DEVICE_TYPE_GPU) { if(num_devices) *num_devices=0; return 0; }
    uint32_t n=(uint32_t)number("FIXTURE_COUNT",1);
    if(devices) { for(uint32_t i=0;i<n && i<num_entries;i++) devices[i]=(void *)(size_t)(0x2000+i); }
    if(num_devices) *num_devices=n;
    return 0;
}
int clGetDeviceInfo(void *device, uint32_t param, size_t size, void *value, size_t *size_ret) {
    (void)device;
    const char *text=NULL;
    unsigned char uuid[16];
    if(param==0x102Cu) text="LISUAN";
    else if(param==0x102Bu) text="Fixture \"Lisuan GPU\"";
    else if(param==0x106Au) {
        if(number("FIXTURE_OPENCL_UUID",0)) { for(int i=0;i<16;i++) uuid[i]=(unsigned char)(0x20+i); }
        else { for(int i=0;i<16;i++) uuid[i]=0; }
        size_t n=16; if(size<n) n=size;
        if(value) memcpy(value,uuid,n);
        if(size_ret) *size_ret=16;
        return 0;
    }
    else return -30;
    size_t n=strlen(text)+1; if(size<n) n=size;
    if(value) { memcpy(value,text,n); if(n>0) ((char *)value)[n-1]=0; }
    if(size_ret) *size_ret=strlen(text)+1;
    return 0;
}
#define ALIAS(NEW,OLD) extern __typeof(OLD) NEW __attribute__((alias(#OLD)));
ALIAS(muInit,cuInit) ALIAS(muDeviceGetCount,cuDeviceGetCount) ALIAS(muDeviceGet,cuDeviceGet)
ALIAS(muDeviceGetName,cuDeviceGetName) ALIAS(muDeviceGetPCIBusId,cuDeviceGetPCIBusId) ALIAS(muDeviceGetUuid,cuDeviceGetUuid)
ALIAS(hipInit,cuInit) ALIAS(hipGetDeviceCount,cuDeviceGetCount) ALIAS(hipDeviceGet,cuDeviceGet)
ALIAS(hipDeviceGetName,cuDeviceGetName) ALIAS(hipDeviceGetPCIBusId,cuDeviceGetPCIBusId)
#ifndef FIXTURE_OMIT_HIP_UUID
ALIAS(hipDeviceGetUuid,cuDeviceGetUuid)
#endif
ALIAS(hcInit,cuInit) ALIAS(hcGetDeviceCount,cuDeviceGetCount) ALIAS(hcDeviceGet,cuDeviceGet)
ALIAS(hcDeviceGetName,cuDeviceGetName) ALIAS(hcDeviceGetPCIBusId,cuDeviceGetPCIBusId) ALIAS(hcDeviceGetUuid,cuDeviceGetUuid)
