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
 * Return the index of psz_mrl within a snapshot, or -1 if absent.
 * Matching by MRL string is robust against VLC's subitem handling, where
 * the playing track's input_item_t pointer does not equal the playlist
 * child's pointer (e.g. tracks expanded from an .m3u8 container).
 */
static inline int snapshot_index_of(char * const *snap, int n,
                                     const char *psz_mrl)
{
    if (!snap || !psz_mrl) return -1;
    for (int i = 0; i < n; i++)
        if (snap[i] && strcmp(snap[i], psz_mrl) == 0)
            return i;
    return -1;
}

#endif /* VLC_RESUME_UTILS_H */
