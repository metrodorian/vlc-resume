#ifndef VLC_RESUME_UTILS_H
#define VLC_RESUME_UTILS_H

#include <stdlib.h>
#include <string.h>
#include <vlc_common.h>
#include <vlc_playlist.h>
#include <vlc_input.h>

/*
 * Recursively collect the MRLs of all *leaf* items under a node into a
 * growable array. Container nodes (i_children > 0) are descended into but
 * not themselves recorded. This is essential for .m3u8 / directory inputs,
 * where the real tracks are nested one level below the playing root as
 * children of the container item, not direct children of p_playing.
 */
static inline void snapshot_collect_(playlist_item_t *node,
                                     char ***pppsz, int *pn, int *pcap)
{
    for (int i = 0; i < node->i_children; i++) {
        playlist_item_t *ch = node->pp_children[i];
        if (ch->i_children > 0) {
            snapshot_collect_(ch, pppsz, pn, pcap);
            continue;
        }
        char *uri = (ch->p_input) ? input_item_GetURI(ch->p_input)
                                  : strdup("");
        if (*pn >= *pcap) {
            int nc = (*pcap > 0) ? (*pcap * 2) : 16;
            char **tmp = realloc(*pppsz, (size_t)nc * sizeof(char *));
            if (!tmp) { free(uri); return; }
            *pppsz = tmp;
            *pcap  = nc;
        }
        (*pppsz)[(*pn)++] = uri;
    }
}

/*
 * Snapshot the flat list of leaf-track MRLs under the current playing scope.
 * Uses playlist_Lock/Unlock directly to avoid the PL_LOCK macro's
 * requirement for a local variable named 'p_playlist'.
 * Caller must call snapshot_free().
 */
static inline char **playlist_snapshot(playlist_t *p_pl, int *p_count)
{
    char **ppsz = NULL;
    int    n    = 0, cap = 0;

    playlist_Lock(p_pl);
    if (p_pl->p_playing)
        snapshot_collect_(p_pl->p_playing, &ppsz, &n, &cap);
    playlist_Unlock(p_pl);

    *p_count = n;
    return ppsz;
}

/*
 * Recursively find the leaf playlist item whose MRL equals psz_mrl, or NULL.
 * The playlist must be locked by the caller. Returns the item so the caller
 * can start playback within its parent node (handles nested m3u8 tracks).
 */
static inline playlist_item_t *playlist_find_item_by_mrl(playlist_item_t *node,
                                                         const char *psz_mrl)
{
    if (!node || !psz_mrl) return NULL;
    for (int i = 0; i < node->i_children; i++) {
        playlist_item_t *ch = node->pp_children[i];
        if (ch->i_children > 0) {
            playlist_item_t *r = playlist_find_item_by_mrl(ch, psz_mrl);
            if (r) return r;
            continue;
        }
        if (ch->p_input) {
            char *u = input_item_GetURI(ch->p_input);
            bool match = (u && strcmp(u, psz_mrl) == 0);
            free(u);
            if (match) return ch;
        }
    }
    return NULL;
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
