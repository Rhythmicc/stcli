#ifndef SOLVED_TECH_GPU_ID_BACKEND_H
#define SOLVED_TECH_GPU_ID_BACKEND_H
#define ID_TEXT 160
typedef struct {
    char backend[8], name[ID_TEXT], pci[32], id[ID_TEXT], kind[24]; int ordinal;
    /* Informational only: never used to construct the license ID. */
    char vendor[24], vendor_source[24], pci_vendor[7], pci_device[7];
    char pci_subsystem_vendor[7], pci_subsystem_device[7];
    unsigned long long vram_bytes;
    char board_part_number[ID_TEXT], vbios_version[ID_TEXT];
} Row;
#ifdef __cplusplus
extern "C" {
#endif
int gpu_id_query_sycl(Row *rows, int capacity, char *error, unsigned error_size);
#ifdef __cplusplus
}
#endif
#endif
