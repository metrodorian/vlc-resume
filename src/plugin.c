#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_input.h>
#include <vlc_configuration.h>

#include "plugin.h"
#include "events.h"
#include "timer.h"
#include "state.h"

static int  Open (vlc_object_t *);
static void Close(vlc_object_t *);

vlc_module_begin()
    set_shortname("Resume")
    set_description("Resume playback from last position")
    set_category(CAT_INTERFACE)
    set_subcategory(SUBCAT_INTERFACE_CONTROL)
    set_capability("interface", 0)
    set_callbacks(Open, Close)
vlc_module_end()

static int Open(vlc_object_t *p_this)
{
    intf_thread_t *p_intf = (intf_thread_t *)p_this;

    intf_sys_t *p_sys = calloc(1, sizeof(*p_sys));
    if (!p_sys)
        return VLC_ENOMEM;

    p_intf->p_sys = p_sys;
    vlc_mutex_init(&p_sys->lock);
    p_sys->b_is_startup      = true;
    p_sys->i_playlist_index  = -1;

    /* Build path to state file: <vlc-data-dir>/resume.json */
    char *psz_dir = config_GetUserDir(VLC_DATA_DIR);
    if (!psz_dir) {
        free(p_sys);
        return VLC_EGENERIC;
    }
    if (asprintf(&p_sys->psz_state_file, "%s/resume.json", psz_dir) == -1) {
        free(psz_dir);
        free(p_sys);
        return VLC_ENOMEM;
    }
    free(psz_dir);

    /* Grab the playlist and install the track-change callback. */
    p_sys->p_playlist = pl_Get(p_intf);
    var_AddCallback(p_sys->p_playlist, "input-current",
                    InputCurrentCallback, p_intf);

    /* Start the timer thread. */
    if (vlc_clone(&p_sys->timer_thread, TimerThread, p_intf,
                  VLC_THREAD_PRIORITY_LOW)) {
        var_DelCallback(p_sys->p_playlist, "input-current",
                        InputCurrentCallback, p_intf);
        free(p_sys->psz_state_file);
        vlc_mutex_destroy(&p_sys->lock);
        free(p_sys);
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static void Close(vlc_object_t *p_this)
{
    intf_thread_t *p_intf = (intf_thread_t *)p_this;
    intf_sys_t    *p_sys  = p_intf->p_sys;

    /* Stop timer thread first. */
    vlc_cancel(p_sys->timer_thread);
    vlc_join(p_sys->timer_thread, NULL);

    /* Remove playlist callback. */
    var_DelCallback(p_sys->p_playlist, "input-current",
                    InputCurrentCallback, p_intf);

    /* Remove input callback and release hold if still attached. */
    vlc_mutex_lock(&p_sys->lock);
    input_thread_t *p_input = p_sys->p_input;
    p_sys->p_input = NULL;
    vlc_mutex_unlock(&p_sys->lock);

    if (p_input) {
        var_DelCallback(p_input, "intf-event", IntfEventCallback, p_intf);
        vlc_object_release(p_input);
    }

    /* Final flush of dirty state. */
    if (p_sys->b_dirty && p_sys->psz_current_mrl && p_sys->i_time_ms > 0)
        state_save_position(p_sys->psz_state_file, p_sys->psz_current_mrl,
                            p_sys->i_time_ms);

    free(p_sys->psz_current_mrl);
    free(p_sys->psz_state_file);
    vlc_mutex_destroy(&p_sys->lock);
    free(p_sys);
}
