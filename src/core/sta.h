// sta.h: the m0boy pc build's save-state file (<SD>.STA) — deepeel writes a
// snapshot as slot 0, the firmware's own menu (m0_state.cpp) then loads it.
// The format lives in m0boy targets/common/platform/m0_state.cpp: 10 slots of
// GBL_STATE_SLOT_SECTORS sectors, sector 0 a header ("GBLSTATE", format tag,
// FNV checksum, frame, title), the machine image from sector 1 on, the
// header written last.
#ifndef DP_STA_H
#define DP_STA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DpSta {
    char sta_path[512];  // <project>/<GBL_STATE_NAME>
    char sav_path[512];  // <project>/<GBL_SAVE_NAME>, empty when none
    uint32_t slots, sectors;
    uint32_t format;     // hash of GBL_GEN_VERSION and the state size
    char title[16];
} DpSta;

// reads <project_dir>/gbl_config.h (the generated pc project) for the state
// constants; state_bytes is the machine image size this deepeel build
// produces — refused when the project's slot sectors cannot hold it
int dp_sta_from_project(DpSta *st, const char *project_dir, uint32_t state_bytes, char *err,
                        size_t errsz);

// writes one slot (0..slots-1): data sectors from sector 1, header last,
// exactly as m0_state_save does — a firmware can then load the slot
int dp_sta_write(const DpSta *st, unsigned slot, const uint8_t *img, uint32_t nbytes,
                 uint32_t frame, char *err, size_t errsz);

#ifdef __cplusplus
}
#endif

#endif
