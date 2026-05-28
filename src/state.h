#ifndef VLC_RESUME_STATE_H
#define VLC_RESUME_STATE_H

#include <stdint.h>
#include <stdbool.h>

/* ---- Per-track position -------------------------------------------------- */

/* Load saved position_ms for psz_mrl. Returns true and sets *p_ms on hit. */
bool state_load_position(const char *psz_file, const char *psz_mrl,
                         int64_t *p_ms);

/* Save or update position for psz_mrl. */
bool state_save_position(const char *psz_file, const char *psz_mrl,
                         int64_t i_ms);

/* Remove position entry for psz_mrl (track finished cleanly). */
void state_delete_position(const char *psz_file, const char *psz_mrl);

/* ---- Last session -------------------------------------------------------- */

typedef struct {
    char   **ppsz_tracks;   /* MRL array of the playlist at save time */
    int      i_track_count;
    int      i_index;       /* playlist index of the last-playing track */
    char    *psz_mrl;       /* MRL of the last-playing track */
    int64_t  i_ms;          /* position in that track */
} session_t;

/* Load last_session from state file. Returns true on success.
   Caller must call session_free() when done. */
bool session_load(const char *psz_file, session_t *p_out);

/* Save last_session to state file (atomic). */
bool session_save(const char *psz_file,
                  const char * const *ppsz_tracks, int i_track_count,
                  int i_index, const char *psz_mrl, int64_t i_ms);

/* Free all heap memory owned by a session_t (does not free p_out itself). */
void session_free(session_t *p_sess);

#endif /* VLC_RESUME_STATE_H */
