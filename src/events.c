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

/* Called on the input thread when intf-event fires. */
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
        vlc_tick_t t = var_GetTime(p_input, "time");
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
            bool    need_seek  = !p_sys->b_resumed && p_sys->psz_current_mrl;
            char   *psz_mrl    = need_seek ? strdup(p_sys->psz_current_mrl) : NULL;
            char   *psz_file   = need_seek ? strdup(p_sys->psz_state_file)  : NULL;
            p_sys->b_resumed   = true;
            vlc_mutex_unlock(&p_sys->lock);

            if (need_seek && psz_mrl && psz_file) {
                int64_t i_ms = 0;
                if (state_load(psz_file, psz_mrl, &i_ms)
                    && i_ms > RESUME_THRESHOLD_MS)
                {
                    vlc_tick_t t = VLC_TICK_FROM_MS(i_ms);
                    var_SetTime(p_input, "time", t);
                }
            }

            free(psz_mrl);
            free(psz_file);
        }
        }
    }
    else if (i_event == INPUT_EVENT_EOF) {
        /* Track finished cleanly — remove the saved position. */
        vlc_mutex_lock(&p_sys->lock);
        char *psz_mrl  = p_sys->psz_current_mrl ? strdup(p_sys->psz_current_mrl) : NULL;
        char *psz_file = p_sys->psz_state_file   ? strdup(p_sys->psz_state_file)  : NULL;
        p_sys->b_dirty = false;
        vlc_mutex_unlock(&p_sys->lock);

        if (psz_mrl && psz_file)
            state_delete(psz_file, psz_mrl);

        free(psz_mrl);
        free(psz_file);
    }

    return VLC_SUCCESS;
}

/* Called on the playlist object when input-current changes. */
int InputCurrentCallback(vlc_object_t *p_obj, const char *psz_var,
                         vlc_value_t old_val, vlc_value_t new_val,
                         void *p_data)
{
    VLC_UNUSED(psz_var); VLC_UNUSED(old_val); VLC_UNUSED(p_obj);

    intf_thread_t *p_intf    = (intf_thread_t *)p_data;
    intf_sys_t    *p_sys     = p_intf->p_sys;
    input_thread_t *p_input_new = new_val.p_address;

    vlc_mutex_lock(&p_sys->lock);

    /* Detach callback from the old input. */
    if (p_sys->p_input) {
        vlc_object_release(p_sys->p_input);
        p_sys->p_input = NULL;
    }

    free(p_sys->psz_current_mrl);
    p_sys->psz_current_mrl = NULL;
    p_sys->i_time_ms       = 0;
    p_sys->b_dirty         = false;
    p_sys->b_resumed       = false;

    if (p_input_new) {
        /* Grab MRL before the input thread might disappear. */
        char *psz_uri = input_item_GetURI(input_GetItem(p_input_new));
        if (psz_uri) {
            p_sys->psz_current_mrl = psz_uri;
            p_sys->p_input = vlc_object_hold(p_input_new);
        }
    }

    vlc_mutex_unlock(&p_sys->lock);

    /* Attach event callback to new input outside the lock. */
    if (p_input_new)
        var_AddCallback(p_input_new, "intf-event", IntfEventCallback, p_intf);

    return VLC_SUCCESS;
}
