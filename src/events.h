#ifndef VLC_RESUME_EVENTS_H
#define VLC_RESUME_EVENTS_H

#include <vlc_common.h>
#include <vlc_interface.h>

int  InputCurrentCallback(vlc_object_t *p_obj, const char *psz_var,
                          vlc_value_t old_val, vlc_value_t new_val,
                          void *p_data);

int  IntfEventCallback(vlc_object_t *p_obj, const char *psz_var,
                       vlc_value_t old_val, vlc_value_t new_val,
                       void *p_data);

#endif /* VLC_RESUME_EVENTS_H */
