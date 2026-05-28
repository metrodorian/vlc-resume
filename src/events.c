#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_interface.h>
#include <vlc_input.h>
#include <vlc_playlist.h>
#include <vlc_tick.h>
#include <vlc_url.h>

#include "plugin.h"
#include "events.h"
#include "state.h"

/* Minimum stored position before we attempt a resume seek (10 seconds). */
#define RESUME_THRESHOLD_MS 10000

/* ---------- Playlist snapshot --------------------------------------------- */

/*
 * Collect MRLs of all direct children of p_playlist->p_playing into a
 * freshly malloc'd array. Caller must free each string and the array itself.
 * Returns NULL on failure.
 */
static char **playlist_snapshot(playlist_t *p_pl, int *p_count)
{
    PL_LOCK;
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
    PL_UNLOCK;
    *p_count = ppsz ? n : 0;
    return ppsz;
}

static void snapshot_free(char **ppsz, int n)
{
    for (int i = 0; i < n; i++) free(ppsz[i]);
    free(ppsz);
}

/*
 * Return true if both MRL arrays have the same length and identical content.
 */
static bool playlists_match(const char * const *a, int na,
                            const char * const *b, int nb)
{
    if (na != nb || na == 0) return false;
    for (int i = 0; i < na; i++)
        if (strcmp(a[i] ? a[i] : "", b[i] ? b[i] : "") != 0)
            return false;
    return true;
}

/* ---------- intf-event callback ------------------------------------------- */

int IntfEventCallback(vlc_object_t *p_obj, const char *psz_var,
                      vlc_value_t old_val, vlc_value_t new_val,
                      void *p_data)
{
    VLC_UNUSED(psz_var); VLC_UNUSED(old_val);

    intf_thread_t  *p_intf  = (intf_thread_t *)p_data;
    intf_sys_t     *p_sys   = p_intf->p_sys;
    input_thread_t *p_input = (input_thread_t *)p_obj;
    int             i_event = new_val.i_int;

    if (i_event == INPUT_EVENT_POSITION) {
        vlc_tick_t t  = var_GetTime(p_input, "time");
        int64_t    ms = MS_FROM_VLC_TICK(t);

        vlc_mutex_lock(&p_sys->lock);
        if (p_sys->psz_current_mrl) {
            p_sys->i_time_ms = ms;
            p_sys->b_dirty   = true;
        }
        vlc_mutex_unlock(&p_sys->lock);
    }
    else if (i_event == INPUT_EVENT_STATE) {
        int i_state = var_GetInteger(p_input, "state");

        if (i_state == PLAYING_S) {
            vlc_mutex_lock(&p_sys->lock);
            bool  need_seek = !p_sys->b_resumed && p_sys->psz_current_mrl;
            char *psz_mrl   = need_seek ? strdup(p_sys->psz_current_mrl) : NULL;
            char *psz_file  = need_seek ? strdup(p_sys->psz_state_file)  : NULL;
            p_sys->b_resumed = true;
            vlc_mutex_unlock(&p_sys->lock);

            if (need_seek && psz_mrl && psz_file) {
                int64_t i_ms = 0;
                if (state_load_position(psz_file, psz_mrl, &i_ms)
                    && i_ms > RESUME_THRESHOLD_MS)
                {
                    var_SetTime(p_input, "time", VLC_TICK_FROM_MS(i_ms));
                }
            }
            free(psz_mrl);
            free(psz_file);
        }
    }
    else if (i_event == INPUT_EVENT_EOF) {
        /* Track finished cleanly — remove saved position. */
        vlc_mutex_lock(&p_sys->lock);
        char *psz_mrl  = p_sys->psz_current_mrl
                         ? strdup(p_sys->psz_current_mrl) : NULL;
        char *psz_file = p_sys->psz_state_file
                         ? strdup(p_sys->psz_state_file)  : NULL;
        p_sys->b_dirty = false;
        vlc_mutex_unlock(&p_sys->lock);

        if (psz_mrl && psz_file)
            state_delete_position(psz_file, psz_mrl);

        free(psz_mrl);
        free(psz_file);
    }

    return VLC_SUCCESS;
}

/* ---------- input-current callback ---------------------------------------- */

int InputCurrentCallback(vlc_object_t *p_obj, const char *psz_var,
                         vlc_value_t old_val, vlc_value_t new_val,
                         void *p_data)
{
    VLC_UNUSED(psz_var); VLC_UNUSED(old_val); VLC_UNUSED(p_obj);

    intf_thread_t  *p_intf       = (intf_thread_t *)p_data;
    intf_sys_t     *p_sys        = p_intf->p_sys;
    input_thread_t *p_input_new  = new_val.p_address;

    vlc_mutex_lock(&p_sys->lock);

    /* Detach callback + release hold on the old input. */
    input_thread_t *p_input_old = p_sys->p_input;
    p_sys->p_input = NULL;

    /* Save current position before we lose the MRL. */
    if (p_sys->b_dirty && p_sys->psz_current_mrl && p_sys->i_time_ms > 0)
        state_save_position(p_sys->psz_state_file,
                            p_sys->psz_current_mrl, p_sys->i_time_ms);

    /* Update last_session with the track we just left. */
    if (p_sys->psz_current_mrl && p_sys->i_playlist_index >= 0) {
        int    snap_count = 0;
        char **ppsz_snap  = NULL;
        /* Unlock briefly to call playlist_snapshot (it takes PL_LOCK). */
        vlc_mutex_unlock(&p_sys->lock);
        ppsz_snap = playlist_snapshot(p_sys->p_playlist, &snap_count);
        vlc_mutex_lock(&p_sys->lock);

        if (ppsz_snap && snap_count > 0)
            session_save(p_sys->psz_state_file,
                         (const char * const *)ppsz_snap, snap_count,
                         p_sys->i_playlist_index,
                         p_sys->psz_current_mrl,
                         p_sys->i_time_ms);
        snapshot_free(ppsz_snap, snap_count);
    }

    bool b_is_startup = p_sys->b_is_startup;
    p_sys->b_is_startup = false;

    free(p_sys->psz_current_mrl);
    p_sys->psz_current_mrl  = NULL;
    p_sys->i_time_ms        = 0;
    p_sys->i_playlist_index = -1;
    p_sys->b_dirty          = false;
    p_sys->b_resumed        = false;

    if (p_input_new) {
        char *psz_uri = input_item_GetURI(input_GetItem(p_input_new));
        if (psz_uri) {
            p_sys->psz_current_mrl = psz_uri;
            p_sys->p_input = vlc_object_hold(p_input_new);

            /* Determine current playlist index. */
            playlist_item_t *p_item =
                playlist_ItemGetByInput(p_sys->p_playlist,
                                        input_GetItem(p_input_new));
            if (p_item)
                p_sys->i_playlist_index = p_item->i_id; /* use array index below */
        }
    }

    vlc_mutex_unlock(&p_sys->lock);

    /* Detach callback from the old input now that we hold no lock. */
    if (p_input_old) {
        var_DelCallback(p_input_old, "intf-event", IntfEventCallback, p_intf);
        vlc_object_release(p_input_old);
    }

    /* Attach callback to new input. */
    if (p_input_new)
        var_AddCallback(p_input_new, "intf-event", IntfEventCallback, p_intf);

    /*
     * Playlist resume on startup: if this is the very first track and it does
     * NOT match the saved session's last track, check whether the current
     * playlist matches the saved session. If so, jump to the saved track index.
     * The subsequent PLAYING_S event will then seek to the saved millisecond.
     */
    if (b_is_startup && p_input_new) {
        session_t sess;
        char     *psz_state = NULL;

        vlc_mutex_lock(&p_sys->lock);
        psz_state = p_sys->psz_state_file ? strdup(p_sys->psz_state_file) : NULL;
        vlc_mutex_unlock(&p_sys->lock);

        if (psz_state && session_load(psz_state, &sess)) {
            int    snap_count = 0;
            char **ppsz_snap  = playlist_snapshot(p_sys->p_playlist,
                                                   &snap_count);

            if (playlists_match((const char * const *)ppsz_snap, snap_count,
                                (const char * const *)sess.ppsz_tracks,
                                sess.i_track_count))
            {
                /* Find the saved track by MRL inside the current playlist. */
                int target_idx = -1;
                for (int i = 0; i < snap_count; i++) {
                    if (ppsz_snap[i] && strcmp(ppsz_snap[i], sess.psz_mrl) == 0) {
                        target_idx = i;
                        break;
                    }
                }
                if (target_idx >= 0 && target_idx != 0) {
                    /* Navigate to the saved track; PLAYING_S will seek. */
                    PL_LOCK;
                    playlist_item_t *p_playing =
                        p_sys->p_playlist->p_playing;
                    if (p_playing && target_idx < p_playing->i_children)
                        playlist_ViewPlay(p_sys->p_playlist, p_playing,
                                          p_playing->pp_children[target_idx]);
                    PL_UNLOCK;
                }
            }

            snapshot_free(ppsz_snap, snap_count);
            session_free(&sess);
        }
        free(psz_state);
    }

    return VLC_SUCCESS;
}
