#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_interface.h>
#include <vlc_mtime.h>     /* mdate(), msleep(), vlc_tick_t, VLC_TICK_FROM_SEC */

#include "timer.h"
#include "state.h"
#include "plugin.h"
#include "vlc_resume_utils.h"

#define SAVE_INTERVAL VLC_TICK_FROM_SEC(5)
/* Poll fast so a deferred resume seek is applied with minimal latency. */
#define POLL_INTERVAL VLC_TICK_FROM_MS(200)

void *TimerThread(void *p_data)
{
    intf_thread_t *p_intf = (intf_thread_t *)p_data;
    intf_sys_t    *p_sys  = p_intf->p_sys;

    vlc_tick_t next_save = mdate() + SAVE_INTERVAL;

    /* Resume-jump state, persisted across poll iterations (timer is single
     * threaded). We keep re-asserting the jump until the target track is
     * confirmed playing and stable, because VLC's own autoplay of track 0 can
     * fire AFTER our jump and clobber it. A deadline bounds the assertion so
     * we never fight a user who manually navigates right after opening. */
    vlc_tick_t resume_start  = 0;   /* first poll the playlist matched */
    vlc_tick_t target_since  = 0;   /* first poll cur == target observed */
    vlc_tick_t last_jump     = 0;   /* last ViewPlay issue (rate limit) */

    for (;;) {
        msleep(POLL_INTERVAL);
        vlc_testcancel();

        /* Perform any seek deferred by IntfEventCallback. Running it here,
         * off the input event thread, avoids the re-entrant deadlock that
         * a synchronous var_SetInteger("time") would cause inside the
         * callback. We hold a reference to the input across the blocking
         * call so it cannot be released underneath us. */
        vlc_mutex_lock(&p_sys->lock);
        bool             do_seek = p_sys->b_seek_pending;
        int64_t          seek_ms = p_sys->i_seek_target_ms;
        input_thread_t  *p_seek  = NULL;
        if (do_seek) {
            p_sys->b_seek_pending = false;
            p_seek = p_sys->p_input;
            if (p_seek)
                vlc_object_hold(p_seek);
        }
        vlc_mutex_unlock(&p_sys->lock);

        if (p_seek) {
            var_SetInteger(p_seek, "time", VLC_TICK_FROM_MS(seek_ms));
            vlc_object_release(p_seek);
        }

        /* One-time playlist resume jump. Retried every poll until the
         * playlist has expanded enough to match the saved session. Doing it
         * here (rather than in a track-change callback) is what makes it
         * reliable: an .m3u8 expands asynchronously, frequently after the
         * first track is already playing, with no further callback to react
         * to. We jump to wherever the saved track now sits in the playlist. */
        vlc_mutex_lock(&p_sys->lock);
        bool  do_resume   = p_sys->b_resume_pending && p_sys->p_input
                            && p_sys->psz_current_mrl && p_sys->psz_state_file;
        char *cur_mrl     = do_resume ? strdup(p_sys->psz_current_mrl)  : NULL;
        char *resume_file = do_resume ? strdup(p_sys->psz_state_file)   : NULL;
        vlc_mutex_unlock(&p_sys->lock);

        if (do_resume && cur_mrl && resume_file) {
            vlc_tick_t now = mdate();
            if (resume_start == 0)
                resume_start = now;
            /* Bound the whole resume attempt. After this we stop trying (and,
             * crucially, allow session saving again) whether or not we ever
             * found a matching session — otherwise a fresh start with no saved
             * session would keep b_resume_pending set forever and never save. */
            bool give_up = (now - resume_start) > VLC_TICK_FROM_SEC(8);
            bool done    = false;

            int       snap_n = 0;
            char    **snap   = playlist_snapshot(p_sys->p_playlist, &snap_n);
            session_t sess;
            if (session_load(resume_file, &sess)) {
                int  target = snapshot_index_of(snap, snap_n, sess.psz_mrl);
                bool same   = snap_n > 0 && sess.i_track_count > 0
                              && target >= 0 && snap[0] && sess.ppsz_tracks[0]
                              && strcmp(snap[0], sess.ppsz_tracks[0]) == 0;
                if (same) {
                    int cur = snapshot_index_of(snap, snap_n, cur_mrl);
                    if (cur == target) {
                        /* Arrived. Require it to stay put briefly so a late
                         * autoplay clobber is caught and re-corrected. */
                        if (target_since == 0)
                            target_since = now;
                        if ((now - target_since) > VLC_TICK_FROM_MS(1200))
                            done = true;
                    } else {
                        target_since = 0; /* not there / clobbered — re-assert */
                        if (!give_up
                            && (now - last_jump) > VLC_TICK_FROM_MS(500)) {
                            last_jump = now;
                            playlist_Lock(p_sys->p_playlist);
                            playlist_item_t *it = playlist_find_item_by_mrl(
                                p_sys->p_playlist->p_playing, sess.psz_mrl);
                            if (it && it->p_parent)
                                playlist_ViewPlay(p_sys->p_playlist,
                                                  it->p_parent, it);
                            playlist_Unlock(p_sys->p_playlist);
                        }
                    }
                }
                session_free(&sess);
            }
            snapshot_free(snap, snap_n);

            if (done || give_up) {
                vlc_mutex_lock(&p_sys->lock);
                p_sys->b_resume_pending = false;
                vlc_mutex_unlock(&p_sys->lock);
            }
        }
        free(cur_mrl);
        free(resume_file);

        if (mdate() < next_save)
            continue;

        next_save = mdate() + SAVE_INTERVAL;

        /* Snapshot the fields we need under lock, then release immediately.
         * Disk I/O and PL_LOCK must NOT be taken while holding p_sys->lock. */
        vlc_mutex_lock(&p_sys->lock);
        bool    b_dirty   = p_sys->b_dirty;
        bool    b_pending = p_sys->b_resume_pending;
        char   *psz_mrl   = (b_dirty && p_sys->psz_current_mrl)
                            ? strdup(p_sys->psz_current_mrl) : NULL;
        int64_t i_ms      = p_sys->i_time_ms;
        char   *psz_file  = (b_dirty && p_sys->psz_state_file)
                            ? strdup(p_sys->psz_state_file) : NULL;
        p_sys->b_dirty    = false;
        vlc_mutex_unlock(&p_sys->lock);

        if (!b_dirty || !psz_mrl || !psz_file || i_ms <= 0) {
            free(psz_mrl);
            free(psz_file);
            continue;
        }

        /* Write per-track position. */
        state_save_position(psz_file, psz_mrl, i_ms);

        /* Write last_session — crash-safe playlist resume.
         * PL_LOCK taken inside playlist_snapshot, never with p_sys->lock.
         * Derive the index from the snapshot by MRL: skips container items
         * (e.g. the .m3u8 itself) whose MRL is not among the real tracks.
         *
         * Guards (critical): do NOT overwrite the session while a resume jump
         * is still pending — that would clobber the very data we are reading
         * to resume. Also require a real multi-track playlist (snap_n >= 2):
         * during the brief pre-expansion window an .m3u8 snapshots as a single
         * container item, and persisting that destroys the saved session. */
        /* Write last_session — crash-safe playlist resume.
         * PL_LOCK taken inside playlist_snapshot, never with p_sys->lock.
         * Derive the index from the snapshot by MRL: skips container items
         * (e.g. the .m3u8 itself) whose MRL is not among the real tracks.
         *
         * Guards:
         *  • skip while b_resume_pending (would clobber data being read)
         *  • skip for container-only snapshots (snap_n < 2)
         *  • skip if the currently playing track is NOT part of the saved
         *    session — this protects against VLC appending a random external
         *    file to the playlist (e.g. user opens Vanilla Ice while a
         *    playlist is running). Without this guard the session mrl/index
         *    would be overwritten with the external file, and the next open
         *    of the original playlist would find target=-1 and never jump. */
        if (!b_pending) {
            int    snap_n = 0;
            char **snap   = playlist_snapshot(p_sys->p_playlist, &snap_n);
            int    idx    = snapshot_index_of(snap, snap_n, psz_mrl);
            if (snap && snap_n >= 2 && idx >= 0) {
                /* Only save if the current track belongs to the existing
                 * session's original track list, AND cap the saved track
                 * count to that list's length. This prevents an externally
                 * appended file (e.g. the user opens Vanilla Ice while a
                 * playlist is running) from ever entering the session:
                 *   – the in_sess check stops saving once the alien track
                 *     is current;
                 *   – the save_n cap stops the alien track from being
                 *     written into "tracks" even during the transition
                 *     callback (where the snapshot already includes it but
                 *     the leaving track is still a legitimate playlist item).
                 * On first run (no prior session) we fall through and save
                 * the full snapshot. */
                session_t old = {0};
                bool has_old  = session_load(psz_file, &old);
                bool in_sess  = !has_old;
                int  save_n   = snap_n;
                for (int i = 0; i < old.i_track_count && !in_sess; i++)
                    if (old.ppsz_tracks[i]
                        && strcmp(old.ppsz_tracks[i], psz_mrl) == 0)
                        in_sess = true;
                if (has_old && in_sess && old.i_track_count < snap_n)
                    save_n = old.i_track_count; /* cap: never grow the list */
                if (has_old) session_free(&old);

                if (in_sess && idx < save_n)
                    session_save(psz_file, (const char * const *)snap, save_n,
                                 idx, psz_mrl, i_ms);
            }
            snapshot_free(snap, snap_n);
        }

        free(psz_mrl);
        free(psz_file);
    }

    return NULL;
}
