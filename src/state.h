#ifndef VLC_RESUME_STATE_H
#define VLC_RESUME_STATE_H

#include <stdint.h>
#include <stdbool.h>

/* Load position_ms for a given MRL from the state file.
   Returns true and sets *p_ms on success, false if not found. */
bool state_load(const char *psz_file, const char *psz_mrl, int64_t *p_ms);

/* Save or update the entry for psz_mrl in the state file (atomic write). */
bool state_save(const char *psz_file, const char *psz_mrl, int64_t i_ms);

/* Remove the entry for psz_mrl from the state file (track finished). */
void state_delete(const char *psz_file, const char *psz_mrl);

#endif /* VLC_RESUME_STATE_H */
