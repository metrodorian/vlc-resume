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

/* Read entire file into a malloc'd buffer. Caller must free. */
static char *read_file(const char *psz_path)
{
    FILE *f = fopen(psz_path, "rb");
    if (!f)
        return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0) {
        fclose(f);
        return NULL;
    }

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* Write buf atomically to psz_path via a .tmp side-file. */
static bool write_atomic(const char *psz_path, const char *buf)
{
    size_t path_len = strlen(psz_path);
    char *psz_tmp = malloc(path_len + 5);
    if (!psz_tmp)
        return false;
    memcpy(psz_tmp, psz_path, path_len);
    memcpy(psz_tmp + path_len, ".tmp", 5);

    FILE *f = fopen(psz_tmp, "wb");
    if (!f) {
        free(psz_tmp);
        return false;
    }

    size_t len = strlen(buf);
    bool ok = fwrite(buf, 1, len, f) == len;
    fclose(f);

    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(psz_tmp, psz_path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
        ok = rename(psz_tmp, psz_path) == 0;
#endif
    }

    if (!ok)
        remove(psz_tmp);

    free(psz_tmp);
    return ok;
}

/* Parse state file into a cJSON object. Returns empty object if missing/corrupt. */
static cJSON *load_json(const char *psz_file)
{
    char *buf = read_file(psz_file);
    cJSON *root = NULL;
    if (buf) {
        root = cJSON_Parse(buf);
        free(buf);
    }
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    return root;
}

bool state_load(const char *psz_file, const char *psz_mrl, int64_t *p_ms)
{
    cJSON *root = load_json(psz_file);
    bool found = false;

    cJSON *entry = cJSON_GetObjectItemCaseSensitive(root, psz_mrl);
    if (entry && cJSON_IsNumber(entry)) {
        *p_ms = (int64_t)entry->valuedouble;
        found = true;
    }

    cJSON_Delete(root);
    return found;
}

bool state_save(const char *psz_file, const char *psz_mrl, int64_t i_ms)
{
    cJSON *root = load_json(psz_file);

    /* Update or insert the entry for this MRL. */
    cJSON *existing = cJSON_GetObjectItemCaseSensitive(root, psz_mrl);
    if (existing)
        cJSON_SetNumberValue(existing, (double)i_ms);
    else
        cJSON_AddNumberToObject(root, psz_mrl, (double)i_ms);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out)
        return false;

    bool ok = write_atomic(psz_file, out);
    free(out);
    return ok;
}

void state_delete(const char *psz_file, const char *psz_mrl)
{
    cJSON *root = load_json(psz_file);
    cJSON_DeleteItemFromObjectCaseSensitive(root, psz_mrl);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out)
        return;

    write_atomic(psz_file, out);
    free(out);
}
