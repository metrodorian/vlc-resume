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

        if (mdate() < next_save)
            continue;

        next_save = mdate() + SAVE_INTERVAL;

        /* Snapshot the fields we need under lock, then release immediately.
         * Disk I/O and PL_LOCK must NOT be taken while holding p_sys->lock. */
        vlc_mutex_lock(&p_sys->lock);
        bool    b_dirty  = p_sys->b_dirty;
        char   *psz_mrl  = (b_dirty && p_sys->psz_current_mrl)
                           ? strdup(p_sys->psz_current_mrl) : NULL;
        int64_t i_ms     = p_sys->i_time_ms;
        char   *psz_file = (b_dirty && p_sys->psz_state_file)
                           ? strdup(p_sys->psz_state_file) : NULL;
        p_sys->b_dirty   = false;
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
         * (e.g. the .m3u8 itself) whose MRL is not among the real tracks. */
        {
            int    snap_n = 0;
            char **snap   = playlist_snapshot(p_sys->p_playlist, &snap_n);
            int    idx    = snapshot_index_of(snap, snap_n, psz_mrl);
            if (snap && snap_n > 0 && idx >= 0)
                session_save(psz_file, (const char * const *)snap, snap_n,
                             idx, psz_mrl, i_ms);
            snapshot_free(snap, snap_n);
        }

        free(psz_mrl);
        free(psz_file);
    }

    return NULL;
}
