#ifndef SOLVED_TECH_GPU_ID_VENDOR_H
#define SOLVED_TECH_GPU_ID_VENDOR_H
/* Hardware observations, never inputs to the license ID algorithm. */
static int vendor_contains(const char *text, const char *word) {
    for (; *text; text++) {
        size_t i=0;
        while(word[i] && text[i] && tolower((unsigned char)text[i])==tolower((unsigned char)word[i])) i++;
        if(!word[i]) return 1;
    }
    return 0;
}
#ifdef __linux__
#ifndef GPU_ID_PCI_ROOT
#define GPU_ID_PCI_ROOT "/sys/bus/pci/devices"
#endif
static unsigned vendor_pci_value(const char *bdf, const char *attribute, char out[7]) {
    unsigned domain,bus,device,function,value=0; char tail, path[512], text[32];
    if(sscanf(bdf,"%x:%x:%x.%x%c",&domain,&bus,&device,&function,&tail)!=4 ||
       domain>65535 || bus>255 || device>31 || function>7) return 0;
    snprintf(path,sizeof(path),GPU_ID_PCI_ROOT "/%04x:%02x:%02x.%x/%s",domain,bus,device,function,attribute);
    FILE *f=fopen(path,"r"); if(!f) return 0;
    int ok=fgets(text,sizeof(text),f)!=NULL; fclose(f);
    char *end=NULL;
    if(!ok) return 0;
    unsigned long parsed=strtoul(text,&end,16);
    if(end==text || parsed>65535) return 0;
    while(isspace((unsigned char)*end)) end++;
    if(*end) return 0;
    value=(unsigned)parsed;
    snprintf(out,7,"0x%04x",value);
    return value;
}
static unsigned long long vendor_vram_bytes(const char *bdf) {
    unsigned domain,bus,device,function; char tail,path[512],text[64],*end=NULL;
    if(sscanf(bdf,"%x:%x:%x.%x%c",&domain,&bus,&device,&function,&tail)!=4 ||
       domain>65535 || bus>255 || device>31 || function>7) return 0;
    snprintf(path,sizeof(path),GPU_ID_PCI_ROOT "/%04x:%02x:%02x.%x/mem_info_vram_total",domain,bus,device,function);
    FILE *f=fopen(path,"r"); if(!f) return 0;
    int ok=fgets(text,sizeof(text),f)!=NULL; fclose(f);
    if(!ok || !isdigit((unsigned char)text[0])) return 0;
    errno=0;
    unsigned long long value=strtoull(text,&end,10);
    if(errno || end==text || !value || value>0x7fffffffffffffffULL) return 0;
    while(isspace((unsigned char)*end)) end++;
    return *end ? 0 : value;
}
static void vendor_board_text(const char *bdf,const char *attribute,char out[ID_TEXT]) {
    unsigned domain,bus,device,function; char tail,path[512],text[ID_TEXT+2];
    if(sscanf(bdf,"%x:%x:%x.%x%c",&domain,&bus,&device,&function,&tail)!=4 ||
       domain>65535 || bus>255 || device>31 || function>7) return;
    snprintf(path,sizeof(path),GPU_ID_PCI_ROOT "/%04x:%02x:%02x.%x/%s",domain,bus,device,function,attribute);
    FILE *f=fopen(path,"r"); if(!f) return;
    int ok=fgets(text,sizeof(text),f)!=NULL; fclose(f);
    if(!ok) return;
    size_t n=strlen(text);
    while(n && isspace((unsigned char)text[n-1])) text[--n]=0;
    if(n>=ID_TEXT) return;
    for(size_t i=0;i<n;i++) if((unsigned char)text[i]<32 || (unsigned char)text[i]>126) return;
    memcpy(out,text,n+1);
}
#endif
static void vendor_metadata(Row *r) {
    const char *vendor=NULL, *source="backend";
    if(!strcmp(r->backend,"hip")) {
        unsigned pci_vendor=0;
#ifdef __linux__
        pci_vendor=vendor_pci_value(r->pci,"vendor",r->pci_vendor);
        (void)vendor_pci_value(r->pci,"device",r->pci_device);
        (void)vendor_pci_value(r->pci,"subsystem_vendor",r->pci_subsystem_vendor);
        (void)vendor_pci_value(r->pci,"subsystem_device",r->pci_subsystem_device);
        r->vram_bytes=vendor_vram_bytes(r->pci);
        vendor_board_text(r->pci,"product_number",r->board_part_number);
        vendor_board_text(r->pci,"vbios_version",r->vbios_version);
#endif
        if(pci_vendor==0x1d94) { vendor="hygon"; source="pci-vendor-id"; }
        else if(pci_vendor==0x1002) { vendor="amd"; source="pci-vendor-id"; }
        else if(vendor_contains(r->name,"HYGON") || vendor_contains(r->name,"DCU") ||
                strstr(r->name,"海光") || !strcmp(r->name,"Z100")) {
            vendor="hygon"; source="device-name";
        } else if(vendor_contains(r->name,"AMD") || vendor_contains(r->name,"RADEON") ||
                  vendor_contains(r->name,"INSTINCT") || vendor_contains(r->name,"VEGA")) {
            vendor="amd"; source="device-name";
        } else { vendor="hip_unknown"; source="backend-family"; }
    } else if(!strcmp(r->backend,"cuda")) vendor="nvidia";
    else if(!strcmp(r->backend,"musa")) vendor="moore_threads";
    else if(!strcmp(r->backend,"hcr")) vendor="hcr";
    else if(!strcmp(r->backend,"intel") || !strcmp(r->backend,"sycl")) vendor="intel";
    else if(!strcmp(r->backend,"lisuan")) vendor="lisuan";
    if(vendor) {
        snprintf(r->vendor,sizeof(r->vendor),"%s",vendor);
        snprintf(r->vendor_source,sizeof(r->vendor_source),"%s",source);
    }
}
#endif
