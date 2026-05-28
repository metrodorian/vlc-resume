#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <windows.h>
#else
# include <unistd.h>
#endif

#include "cJSON.h"
#include "state.h"

/*
 * State file schema:
 * {
 *   "positions": { "<mrl>": <ms>, ... },
 *   "last_session": {
 *     "tracks": ["<mrl>", ...],
 *     "index":  <int>,
 *     "mrl":    "<mrl>",
 *     "ms":     <int>
 *   }
 * }
 */

/* ---------- internal helpers ---------------------------------------------- */

static char *read_file(const char *psz_path)
{
    FILE *f = fopen(psz_path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf); fclose(f); return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static bool write_atomic(const char *psz_path, const char *buf)
{
    size_t n = strlen(psz_path);
    char *tmp = malloc(n + 5);
    if (!tmp) return false;
    memcpy(tmp, psz_path, n);
    memcpy(tmp + n, ".tmp", 5);

    FILE *f = fopen(tmp, "wb");
    if (!f) { free(tmp); return false; }
    size_t len = strlen(buf);
    bool ok = fwrite(buf, 1, len, f) == len;
    fclose(f);

    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(tmp, psz_path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
        ok = rename(tmp, psz_path) == 0;
#endif
    }
    if (!ok) remove(tmp);
    free(tmp);
    return ok;
}

/* Parse entire state file. Returns empty object on failure/missing. */
static cJSON *load_root(const char *psz_file)
{
    char *buf = read_file(psz_file);
    cJSON *root = NULL;
    if (buf) { root = cJSON_Parse(buf); free(buf); }
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    return root;
}

/* Get or create the "positions" sub-object. */
static cJSON *get_or_create_positions(cJSON *root)
{
    cJSON *pos = cJSON_GetObjectItemCaseSensitive(root, "positions");
    if (!pos || !cJSON_IsObject(pos)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "positions");
        pos = cJSON_AddObjectToObject(root, "positions");
    }
    return pos;
}

/* ---------- per-track position -------------------------------------------- */

bool state_load_position(const char *psz_file, const char *psz_mrl,
                         int64_t *p_ms)
{
    cJSON *root = load_root(psz_file);
    bool found = false;
    cJSON *pos = cJSON_GetObjectItemCaseSensitive(root, "positions");
    if (pos && cJSON_IsObject(pos)) {
        cJSON *entry = cJSON_GetObjectItemCaseSensitive(pos, psz_mrl);
        if (entry && cJSON_IsNumber(entry)) {
            *p_ms = (int64_t)entry->valuedouble;
            found = true;
        }
    }
    cJSON_Delete(root);
    return found;
}

bool state_save_position(const char *psz_file, const char *psz_mrl,
                         int64_t i_ms)
{
    cJSON *root = load_root(psz_file);
    cJSON *pos  = get_or_create_positions(root);
    if (!pos) { cJSON_Delete(root); return false; }

    cJSON *existing = cJSON_GetObjectItemCaseSensitive(pos, psz_mrl);
    if (existing)
        cJSON_SetNumberValue(existing, (double)i_ms);
    else
        cJSON_AddNumberToObject(pos, psz_mrl, (double)i_ms);

    /* Also keep last_session.ms in sync for the currently active track. */
    cJSON *sess = cJSON_GetObjectItemCaseSensitive(root, "last_session");
    if (sess && cJSON_IsObject(sess)) {
        cJSON *sess_mrl = cJSON_GetObjectItemCaseSensitive(sess, "mrl");
        if (sess_mrl && cJSON_IsString(sess_mrl)
            && strcmp(sess_mrl->valuestring, psz_mrl) == 0)
        {
            cJSON *sess_ms = cJSON_GetObjectItemCaseSensitive(sess, "ms");
            if (sess_ms) cJSON_SetNumberValue(sess_ms, (double)i_ms);
        }
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return false;
    bool ok = write_atomic(psz_file, out);
    free(out);
    return ok;
}

void state_delete_position(const char *psz_file, const char *psz_mrl)
{
    cJSON *root = load_root(psz_file);
    cJSON *pos  = cJSON_GetObjectItemCaseSensitive(root, "positions");
    if (pos && cJSON_IsObject(pos))
        cJSON_DeleteItemFromObjectCaseSensitive(pos, psz_mrl);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return;
    write_atomic(psz_file, out);
    free(out);
}

/* ---------- session -------------------------------------------------------- */

bool session_load(const char *psz_file, session_t *p_out)
{
    memset(p_out, 0, sizeof(*p_out));
    cJSON *root = load_root(psz_file);
    bool ok = false;

    cJSON *sess = cJSON_GetObjectItemCaseSensitive(root, "last_session");
    if (!sess || !cJSON_IsObject(sess))
        goto done;

    cJSON *j_tracks = cJSON_GetObjectItemCaseSensitive(sess, "tracks");
    cJSON *j_index  = cJSON_GetObjectItemCaseSensitive(sess, "index");
    cJSON *j_mrl    = cJSON_GetObjectItemCaseSensitive(sess, "mrl");
    cJSON *j_ms     = cJSON_GetObjectItemCaseSensitive(sess, "ms");

    if (!cJSON_IsArray(j_tracks) || !cJSON_IsNumber(j_index)
        || !cJSON_IsString(j_mrl) || !cJSON_IsNumber(j_ms))
        goto done;

    int n = cJSON_GetArraySize(j_tracks);
    if (n <= 0) goto done;

    char **ppsz = malloc((size_t)n * sizeof(char *));
    if (!ppsz) goto done;

    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(j_tracks, i);
        ppsz[i] = (item && cJSON_IsString(item))
                  ? strdup(item->valuestring) : strdup("");
    }

    p_out->ppsz_tracks   = ppsz;
    p_out->i_track_count = n;
    p_out->i_index       = (int)j_index->valuedouble;
    p_out->psz_mrl       = strdup(j_mrl->valuestring);
    p_out->i_ms          = (int64_t)j_ms->valuedouble;
    ok = true;

done:
    cJSON_Delete(root);
    return ok;
}

bool session_save(const char *psz_file,
                  const char * const *ppsz_tracks, int i_track_count,
                  int i_index, const char *psz_mrl, int64_t i_ms)
{
    cJSON *root = load_root(psz_file);

    /* Replace last_session entirely. */
    cJSON_DeleteItemFromObjectCaseSensitive(root, "last_session");
    cJSON *sess = cJSON_AddObjectToObject(root, "last_session");
    if (!sess) { cJSON_Delete(root); return false; }

    cJSON *j_tracks = cJSON_AddArrayToObject(sess, "tracks");
    for (int i = 0; i < i_track_count; i++)
        cJSON_AddItemToArray(j_tracks,
                             cJSON_CreateString(ppsz_tracks[i] ? ppsz_tracks[i] : ""));
    cJSON_AddNumberToObject(sess, "index", (double)i_index);
    cJSON_AddStringToObject(sess, "mrl",   psz_mrl);
    cJSON_AddNumberToObject(sess, "ms",    (double)i_ms);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return false;
    bool ok = write_atomic(psz_file, out);
    free(out);
    return ok;
}

void session_free(session_t *p_sess)
{
    for (int i = 0; i < p_sess->i_track_count; i++)
        free(p_sess->ppsz_tracks[i]);
    free(p_sess->ppsz_tracks);
    free(p_sess->psz_mrl);
    memset(p_sess, 0, sizeof(*p_sess));
}
