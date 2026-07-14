/* ===========================================================================
 * json.h — minimal JSON parser + string escaping for Blue Tasks
 *
 * Neither json-glib nor json-c is a Blue Notes-family dependency, and the
 * Google Tasks API only needs a small subset of JSON: parse a response
 * into a tree, read strings/objects/arrays out of it, and escape strings
 * into hand-built request bodies (GString).  This module is exactly that.
 * =========================================================================== */

#ifndef BT_JSON_H
#define BT_JSON_H

#include <glib.h>

/* The JSON value kinds.                                                     */
typedef enum {
    BT_JSON_NULL = 0,
    BT_JSON_BOOL,
    BT_JSON_NUMBER,
    BT_JSON_STRING,
    BT_JSON_ARRAY,
    BT_JSON_OBJECT
} BtJsonType;

/* ---------------------------------------------------------------------------
 * BtJson — one parsed JSON value (a tagged union).
 *   type     — which member is live.
 *   b/num/str — scalar payloads (str is owned, UTF-8, \u-escapes decoded).
 *   items    — BT_JSON_ARRAY: BtJson* children (owned).
 *   keys     — BT_JSON_OBJECT: gchar* member names, parallel to items.
 * ------------------------------------------------------------------------- */
typedef struct BtJson {
    BtJsonType  type;
    gboolean    b;
    gdouble     num;
    gchar      *str;
    GPtrArray  *items;
    GPtrArray  *keys;
} BtJson;

/* ---------------------------------------------------------------------------
 * bt_json_parse() — parse a complete JSON document.
 *   text — the document; len — its byte length, or -1 for NUL-terminated.
 * Returns the root value (free with bt_json_free), or NULL on any syntax
 * error (this client never needs error detail beyond "bad response").
 * ------------------------------------------------------------------------- */
BtJson *bt_json_parse(const gchar *text, gssize len);

/* bt_json_free() — free a value and its whole subtree.  NULL-safe.          */
void bt_json_free(BtJson *v);

/* ---------------------------------------------------------------------------
 * Tree accessors — every getter is NULL-safe and type-checked, returning
 * NULL / the fallback when the path or type doesn't match, so response
 * handling can chain lookups without intermediate checks.
 * ------------------------------------------------------------------------- */
BtJson      *bt_json_get(BtJson *obj, const gchar *key);   /* object member  */
const gchar *bt_json_str(BtJson *obj, const gchar *key);   /* string member  */
gboolean     bt_json_bool(BtJson *obj, const gchar *key, gboolean def);
guint        bt_json_len(BtJson *arr);                     /* array length   */
BtJson      *bt_json_at(BtJson *arr, guint i);             /* array element  */

/* ---------------------------------------------------------------------------
 * bt_json_escape() — append `s` to `out` as a JSON string INCLUDING the
 * surrounding quotes (control characters and "\ escaped; UTF-8 passes
 * through verbatim).  NULL appends the literal `null` token (no quotes).
 * ------------------------------------------------------------------------- */
void bt_json_escape(GString *out, const gchar *s);

#endif /* BT_JSON_H */
