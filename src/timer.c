#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_interface.h>
#include <vlc_tick.h>

#include "timer.h"
#include "state.h"

/* Forward declaration — intf_sys_t is defined in plugin.c */
typedef struct intf_sys_t intf_sys_t;

#include "plugin.h"

/* Interval between disk writes (5 seconds). */
#define SAVE_INTERVAL VLC_TICK_FROM_SEC(5)

void *TimerThread(void *p_data)
{
    intf_thread_t *p_intf = (intf_thread_t *)p_data;
    intf_sys_t    *p_sys  = p_intf->p_sys;

    vlc_tick_t next_save = vlc_tick_now() + SAVE_INTERVAL;

    for (;;) {
        vlc_tick_t now  = vlc_tick_now();
        vlc_tick_t wait = next_save - now;
        if (wait > 0)
            vlc_tick_sleep(wait);

        /* Check cancellation after wakeup. */
        vlc_testcancel();

        next_save = vlc_tick_now() + SAVE_INTERVAL;

        vlc_mutex_lock(&p_sys->lock);
        bool dirty       = p_sys->b_dirty;
        char *psz_mrl    = dirty ? strdup(p_sys->psz_current_mrl) : NULL;
        int64_t i_ms     = p_sys->i_time_ms;
        char *psz_file   = dirty ? strdup(p_sys->psz_state_file)  : NULL;
        p_sys->b_dirty   = false;
        vlc_mutex_unlock(&p_sys->lock);

        if (dirty && psz_mrl && psz_file && i_ms > 0)
            state_save(psz_file, psz_mrl, i_ms);

        free(psz_mrl);
        free(psz_file);
    }

    return NULL;
}
