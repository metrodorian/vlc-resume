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

void *TimerThread(void *p_data)
{
    intf_thread_t *p_intf = (intf_thread_t *)p_data;
    intf_sys_t    *p_sys  = p_intf->p_sys;

    vlc_tick_t next_save = mdate() + SAVE_INTERVAL;

    for (;;) {
        vlc_tick_t now  = mdate();
        vlc_tick_t wait = next_save - now;
        if (wait > 0)
            msleep(wait);

        vlc_testcancel();

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
