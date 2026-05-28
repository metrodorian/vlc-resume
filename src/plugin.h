#ifndef VLC_RESUME_PLUGIN_H
#define VLC_RESUME_PLUGIN_H

#include <vlc_common.h>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_input.h>

struct intf_sys_t {
    vlc_thread_t     timer_thread;
    vlc_mutex_t      lock;

    char            *psz_state_file;    /* absolute path to resume.json */
    char            *psz_current_mrl;   /* MRL of the currently playing item */
    int64_t          i_time_ms;         /* last known position in milliseconds */
    int              i_playlist_index;  /* index of current track in playlist */
    bool             b_dirty;           /* position changed since last write */
    bool             b_resumed;         /* seek already performed for this track */
    bool             b_is_startup;      /* true until first InputCurrentCallback */

    playlist_t      *p_playlist;
    input_thread_t  *p_input;           /* held reference, may be NULL */
};

#endif /* VLC_RESUME_PLUGIN_H */
