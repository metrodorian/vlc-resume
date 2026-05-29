#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_interface.h>
#include <vlc_input.h>
#include <vlc_playlist.h>
#include <vlc_mtime.h>
#include <vlc_variables.h>
#include <vlc_url.h>

#include "plugin.h"
#include "events.h"
#include "state.h"
#include "vlc_resume_utils.h"

/* Minimum stored position before we attempt a resume seek (10 seconds). */
#define RESUME_THRESHOLD_MS 10000

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
        /* var_GetInteger("time") returns vlc_tick_t (microseconds). */
        int64_t t_us = var_GetInteger(p_input, "time");
        int64_t ms   = MS_FROM_VLC_TICK(t_us);

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
                    /* Defer the actual seek to the timer thread. Calling the
                     * blocking var_SetInteger("time") here would re-enter the
                     * input thread (this callback runs on it) and deadlock
                     * against the input control / UI threads. */
                    vlc_mutex_lock(&p_sys->lock);
                    p_sys->i_seek_target_ms = i_ms;
                    p_sys->b_seek_pending   = true;
                    vlc_mutex_unlock(&p_sys->lock);
                }
            }
            free(psz_mrl);
            free(psz_file);
        }
        else if (i_state == END_S) {
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
    }

    return VLC_SUCCESS;
}

/* ---------- input-current callback ---------------------------------------- */

/*
 * Lock discipline:
 *   p_sys->lock and playlist_Lock must NEVER be held simultaneously.
 *   All disk I/O and playlist queries run without either or only one lock.
 */
int InputCurrentCallback(vlc_object_t *p_obj, const char *psz_var,
                         vlc_value_t old_val, vlc_value_t new_val,
                         void *p_data)
{
    VLC_UNUSED(psz_var); VLC_UNUSED(old_val); VLC_UNUSED(p_obj);

    intf_thread_t  *p_intf      = (intf_thread_t *)p_data;
    intf_sys_t     *p_sys       = p_intf->p_sys;
    input_thread_t *p_input_new = new_val.p_address;

    /* ---- 1. Grab everything about the leaving track under lock. ---------- */
    vlc_mutex_lock(&p_sys->lock);

    input_thread_t *p_input_old = p_sys->p_input;
    p_sys->p_input = NULL;

    char   *psz_old_mrl = p_sys->psz_current_mrl; /* take ownership */
    p_sys->psz_current_mrl = NULL;

    int64_t i_old_ms  = p_sys->i_time_ms;
    bool    b_dirty   = p_sys->b_dirty;
    bool    b_pending = p_sys->b_resume_pending;
    char   *psz_file  = p_sys->psz_state_file
                        ? strdup(p_sys->psz_state_file) : NULL;

    p_sys->i_time_ms        = 0;
    p_sys->b_dirty          = false;
    p_sys->b_resumed        = false;
    p_sys->b_seek_pending   = false;

    vlc_mutex_unlock(&p_sys->lock);

    /* ---- 2. Detach callback from old input (no lock held). --------------- */
    if (p_input_old) {
        var_DelCallback(p_input_old, "intf-event", IntfEventCallback, p_intf);
        vlc_object_release(p_input_old);
    }

    /* ---- 3. Persist leaving-track state (no lock, no playlist_Lock). ---- */
    if (b_dirty && psz_old_mrl && psz_file && i_old_ms > 0)
        state_save_position(psz_file, psz_old_mrl, i_old_ms);

    if (psz_old_mrl && psz_file) {
        int    snap_n = 0;
        char **snap   = playlist_snapshot(p_sys->p_playlist, &snap_n);
        int    idx    = snapshot_index_of(snap, snap_n, psz_old_mrl);
        if (snap && snap_n > 0 && idx >= 0)
            session_save(psz_file, (const char * const *)snap, snap_n,
                         idx, psz_old_mrl, i_old_ms);
        snapshot_free(snap, snap_n);
    }

    free(psz_old_mrl);

    /* ---- 4. Set up arriving track (playlist_Lock only, then p_sys->lock). */
    char *psz_new_mrl = NULL;

    if (p_input_new) {
        input_item_t *p_iitem = input_GetItem(p_input_new);
        psz_new_mrl = input_item_GetURI(p_iitem);

        vlc_mutex_lock(&p_sys->lock);
        if (psz_new_mrl) {
            p_sys->psz_current_mrl  = psz_new_mrl;
            p_sys->p_input          = vlc_object_hold(p_input_new);
        }
        vlc_mutex_unlock(&p_sys->lock);

        var_AddCallback(p_input_new, "intf-event", IntfEventCallback, p_intf);
    }

    /* ---- 5. One-time playlist resume jump. ------------------------------
     * Defer the decision until the arriving track is a real playlist member.
     * Opening an .m3u8 first plays the container item (its MRL is not in the
     * expanded track list); we skip that and keep b_resume_pending set until
     * the first real track arrives, by which time the playlist is populated. */
    if (b_pending && p_input_new && psz_file && psz_new_mrl) {
        int    snap_n = 0;
        char **snap   = playlist_snapshot(p_sys->p_playlist, &snap_n);

        session_t sess;
        if (session_load(psz_file, &sess)) {
            /* Identify "the same playlist" loosely: the current playlist must
             * open with the same first track as the saved session AND still
             * contain the saved track. We deliberately do NOT require exact
             * count/order equality — VLC may append or duplicate items (e.g.
             * macosx-continue-playback restoring the old list alongside an
             * explicitly opened .m3u8), which would defeat a strict compare.
             *
             * The first callback after opening an .m3u8 sees only the
             * unexpanded container (snap_n == 1, snap[0] == the .m3u8 path);
             * that fails the first-track test, so we leave b_resume_pending
             * set and retry on the next track change, by which point the
             * playlist is expanded and the real first track is present. */
            int target = snapshot_index_of(snap, snap_n, sess.psz_mrl);
            bool same_playlist =
                snap_n > 0 && sess.i_track_count > 0 && target >= 0
                && snap[0] && sess.ppsz_tracks[0]
                && strcmp(snap[0], sess.ppsz_tracks[0]) == 0;

            if (same_playlist) {
                int cur = snapshot_index_of(snap, snap_n, psz_new_mrl);
                if (target != cur) {
                    playlist_Lock(p_sys->p_playlist);
                    playlist_item_t *p_playing = p_sys->p_playlist->p_playing;
                    if (p_playing && target < p_playing->i_children)
                        playlist_ViewPlay(p_sys->p_playlist, p_playing,
                                          p_playing->pp_children[target]);
                    playlist_Unlock(p_sys->p_playlist);
                }

                /* Decision made once the playlist matches — stop retrying. */
                vlc_mutex_lock(&p_sys->lock);
                p_sys->b_resume_pending = false;
                vlc_mutex_unlock(&p_sys->lock);
            }
            session_free(&sess);
        }

        snapshot_free(snap, snap_n);
    }

    free(psz_file);
    return VLC_SUCCESS;
}

bool playlists_match(const char * const *a, int na,
                     const char * const *b, int nb)
{
    if (na != nb || na == 0) return false;
    for (int i = 0; i < na; i++)
        if (strcmp(a[i] ? a[i] : "", b[i] ? b[i] : "") != 0)
            return false;
    return true;
}
