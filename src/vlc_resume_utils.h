#ifndef VLC_RESUME_UTILS_H
#define VLC_RESUME_UTILS_H

#include <stdlib.h>
#include <string.h>
#include <vlc_common.h>
#include <vlc_playlist.h>
#include <vlc_input.h>

/*
 * Snapshot the MRL list of the current playing scope.
 * Uses playlist_Lock/Unlock directly to avoid the PL_LOCK macro's
 * requirement for a local variable named 'p_playlist'.
 * Caller must call snapshot_free().
 */
static inline char **playlist_snapshot(playlist_t *p_pl, int *p_count)
{
    playlist_Lock(p_pl);
    playlist_item_t *p_playing = p_pl->p_playing;
    int n = p_playing ? p_playing->i_children : 0;
    char **ppsz = (n > 0) ? malloc((size_t)n * sizeof(char *)) : NULL;
    if (ppsz) {
        for (int i = 0; i < n; i++) {
            playlist_item_t *ch = p_playing->pp_children[i];
            ppsz[i] = (ch && ch->p_input)
                      ? input_item_GetURI(ch->p_input)
                      : strdup("");
        }
    }
    playlist_Unlock(p_pl);
    *p_count = ppsz ? n : 0;
    return ppsz;
}

static inline void snapshot_free(char **ppsz, int n)
{
    for (int i = 0; i < n; i++) free(ppsz[i]);
    free(ppsz);
}

/*
 * Find the 0-based pp_children index of the item whose p_input matches
 * p_iitem. Returns -1 if not found.
 */
static inline int playlist_find_index(playlist_t *p_pl, input_item_t *p_iitem)
{
    int idx = -1;
    playlist_Lock(p_pl);
    playlist_item_t *p_playing = p_pl->p_playing;
    if (p_playing && p_iitem) {
        for (int i = 0; i < p_playing->i_children; i++) {
            playlist_item_t *ch = p_playing->pp_children[i];
            if (ch && ch->p_input == p_iitem) {
                idx = i;
                break;
            }
        }
    }
    playlist_Unlock(p_pl);
    return idx;
}

#endif /* VLC_RESUME_UTILS_H */
