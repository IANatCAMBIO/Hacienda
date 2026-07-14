/* ===========================================================================
 * json.c — minimal JSON parser + string escaping (see json.h)
 *
 * A plain recursive-descent parser over a byte cursor.  It accepts every
 * valid JSON document the Google APIs emit; on the first syntax error the
 * whole parse returns NULL.  Depth is capped so a malicious response
 * cannot blow the C stack.
 * =========================================================================== */

#include "json.h"
#include <string.h>
#include <stdlib.h>

#define BT_JSON_MAX_DEPTH 64         /* recursion cap for arrays/objects    */

/* The parse cursor: current position and end of input.                     */
typedef struct {
    const gchar *p;
    const gchar *end;
} BtJsonCursor;

static BtJson *parse_value(BtJsonCursor *c, gint depth);

/* json_new() — allocate a zeroed value of `type`.                           */
static BtJson *
json_new(BtJsonType type)
{
    BtJson *v = g_new0(BtJson, 1);
    v->type = type;
    return v;
}

/* ---------------------------------------------------------------------------
 * bt_json_free() — free a value and its whole subtree.  NULL-safe.
 * ------------------------------------------------------------------------- */
void
bt_json_free(BtJson *v)
{
    if (v == NULL)
        return;
    g_free(v->str);
    if (v->items != NULL) {
        for (guint i = 0; i < v->items->len; i++)
            bt_json_free(g_ptr_array_index(v->items, i));
        g_ptr_array_free(v->items, TRUE);
    }
    if (v->keys != NULL) {
        for (guint i = 0; i < v->keys->len; i++)
            g_free(g_ptr_array_index(v->keys, i));
        g_ptr_array_free(v->keys, TRUE);
    }
    g_free(v);
}

/* skip_ws() — advance the cursor past JSON whitespace.                      */
static void
skip_ws(BtJsonCursor *c)
{
    while (c->p < c->end &&
           (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        c->p++;
}

/* lit() — consume the literal `word` if it is next; TRUE on match.          */
static gboolean
lit(BtJsonCursor *c, const gchar *word)
{
    gsize n = strlen(word);
    if ((gsize)(c->end - c->p) < n || strncmp(c->p, word, n) != 0)
        return FALSE;
    c->p += n;
    return TRUE;
}

/* hex4() — parse exactly four hex digits into `out`; TRUE on success.       */
static gboolean
hex4(BtJsonCursor *c, gunichar *out)
{
    if (c->end - c->p < 4)
        return FALSE;
    gunichar u = 0;                  /* accumulated code unit               */
    for (gint i = 0; i < 4; i++) {
        gchar ch = c->p[i];
        u <<= 4;
        if      (ch >= '0' && ch <= '9') u |= (gunichar)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') u |= (gunichar)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') u |= (gunichar)(ch - 'A' + 10);
        else return FALSE;
    }
    c->p += 4;
    *out = u;
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * parse_string() — parse a quoted string (cursor on the opening quote)
 * into a newly allocated UTF-8 buffer.  \uXXXX escapes are decoded,
 * including UTF-16 surrogate pairs.  Returns NULL on syntax error.
 * ------------------------------------------------------------------------- */
static gchar *
parse_string(BtJsonCursor *c)
{
    if (c->p >= c->end || *c->p != '"')
        return NULL;
    c->p++;
    GString *s = g_string_new(NULL);
    while (c->p < c->end && *c->p != '"') {
        guchar ch = (guchar)*c->p;
        if (ch < 0x20)               /* raw control chars are invalid       */
            goto fail;
        if (ch != '\\') {
            g_string_append_c(s, (gchar)ch);
            c->p++;
            continue;
        }
        c->p++;                      /* consume the backslash               */
        if (c->p >= c->end)
            goto fail;
        gchar e = *c->p++;
        switch (e) {
        case '"':  g_string_append_c(s, '"');  break;
        case '\\': g_string_append_c(s, '\\'); break;
        case '/':  g_string_append_c(s, '/');  break;
        case 'b':  g_string_append_c(s, '\b'); break;
        case 'f':  g_string_append_c(s, '\f'); break;
        case 'n':  g_string_append_c(s, '\n'); break;
        case 'r':  g_string_append_c(s, '\r'); break;
        case 't':  g_string_append_c(s, '\t'); break;
        case 'u': {
            gunichar u;              /* decoded code unit / code point      */
            if (!hex4(c, &u))
                goto fail;
            if (u >= 0xD800 && u <= 0xDBFF) {      /* high surrogate        */
                gunichar lo;         /* the required low surrogate          */
                if (!lit(c, "\\u") || !hex4(c, &lo) ||
                    lo < 0xDC00 || lo > 0xDFFF)
                    goto fail;
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            } else if (u >= 0xDC00 && u <= 0xDFFF) {
                goto fail;           /* lone low surrogate                  */
            }
            g_string_append_unichar(s, u);
            break;
        }
        default:
            goto fail;
        }
    }
    if (c->p >= c->end)
        goto fail;
    c->p++;                          /* consume the closing quote           */
    return g_string_free(s, FALSE);
fail:
    g_string_free(s, TRUE);
    return NULL;
}

/* parse_number() — parse a JSON number with strtod (a superset that is
 * fine here).  The candidate digits are copied into a bounded,
 * NUL-terminated buffer first: strtod itself knows nothing of c->end,
 * and in the explicit-length mode of bt_json_parse a document ending in
 * digits would otherwise let it read past the buffer.                       */
static BtJson *
parse_number(BtJsonCursor *c)
{
    gchar buf[64];                   /* longer than any sane JSON number    */
    gsize n = MIN((gsize)(c->end - c->p), sizeof buf - 1);
    memcpy(buf, c->p, n);
    buf[n] = '\0';

    gchar *after = NULL;             /* first char strtod didn't consume    */
    gdouble d = g_ascii_strtod(buf, &after);
    if (after == buf)
        return NULL;
    c->p += after - buf;
    BtJson *v = json_new(BT_JSON_NUMBER);
    v->num = d;
    return v;
}

/* parse_array() — parse "[ value, ... ]" (cursor past the '[').             */
static BtJson *
parse_array(BtJsonCursor *c, gint depth)
{
    BtJson *v = json_new(BT_JSON_ARRAY);
    v->items = g_ptr_array_new();
    skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        c->p++;
        return v;
    }
    for (;;) {
        BtJson *item = parse_value(c, depth);
        if (item == NULL)
            goto fail;
        g_ptr_array_add(v->items, item);
        skip_ws(c);
        if (c->p >= c->end)
            goto fail;
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == ']') { c->p++; return v; }
        goto fail;
    }
fail:
    bt_json_free(v);
    return NULL;
}

/* parse_object() — parse "{ \"key\": value, ... }" (cursor past the '{').   */
static BtJson *
parse_object(BtJsonCursor *c, gint depth)
{
    BtJson *v = json_new(BT_JSON_OBJECT);
    v->items = g_ptr_array_new();
    v->keys  = g_ptr_array_new();
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        return v;
    }
    for (;;) {
        skip_ws(c);
        gchar *key = parse_string(c);
        if (key == NULL)
            goto fail;
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':') {
            g_free(key);
            goto fail;
        }
        c->p++;
        BtJson *item = parse_value(c, depth);
        if (item == NULL) {
            g_free(key);
            goto fail;
        }
        g_ptr_array_add(v->keys, key);
        g_ptr_array_add(v->items, item);
        skip_ws(c);
        if (c->p >= c->end)
            goto fail;
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == '}') { c->p++; return v; }
        goto fail;
    }
fail:
    bt_json_free(v);
    return NULL;
}

/* parse_value() — dispatch on the next non-space character.                 */
static BtJson *
parse_value(BtJsonCursor *c, gint depth)
{
    if (depth > BT_JSON_MAX_DEPTH)
        return NULL;
    skip_ws(c);
    if (c->p >= c->end)
        return NULL;
    switch (*c->p) {
    case '{': c->p++; return parse_object(c, depth + 1);
    case '[': c->p++; return parse_array(c, depth + 1);
    case '"': {
        gchar *s = parse_string(c);
        if (s == NULL)
            return NULL;
        BtJson *v = json_new(BT_JSON_STRING);
        v->str = s;
        return v;
    }
    case 't':
        if (!lit(c, "true"))
            return NULL;
        { BtJson *v = json_new(BT_JSON_BOOL); v->b = TRUE; return v; }
    case 'f':
        if (!lit(c, "false"))
            return NULL;
        return json_new(BT_JSON_BOOL);
    case 'n':
        if (!lit(c, "null"))
            return NULL;
        return json_new(BT_JSON_NULL);
    default:
        return parse_number(c);
    }
}

/* ---------------------------------------------------------------------------
 * bt_json_parse() — parse a complete document (see json.h).
 * ------------------------------------------------------------------------- */
BtJson *
bt_json_parse(const gchar *text, gssize len)
{
    if (text == NULL)
        return NULL;
    BtJsonCursor c = { text, text + (len < 0 ? (gssize)strlen(text) : len) };
    BtJson *v = parse_value(&c, 0);
    if (v == NULL)
        return NULL;
    skip_ws(&c);
    if (c.p != c.end) {              /* trailing garbage                    */
        bt_json_free(v);
        return NULL;
    }
    return v;
}

/* ---------------------------------------------------------------------------
 * Tree accessors (see json.h for the NULL-safety contract).
 * ------------------------------------------------------------------------- */
BtJson *
bt_json_get(BtJson *obj, const gchar *key)
{
    if (obj == NULL || obj->type != BT_JSON_OBJECT)
        return NULL;
    for (guint i = 0; i < obj->keys->len; i++)
        if (strcmp(g_ptr_array_index(obj->keys, i), key) == 0)
            return g_ptr_array_index(obj->items, i);
    return NULL;
}

const gchar *
bt_json_str(BtJson *obj, const gchar *key)
{
    BtJson *v = bt_json_get(obj, key);
    return (v != NULL && v->type == BT_JSON_STRING) ? v->str : NULL;
}

gboolean
bt_json_bool(BtJson *obj, const gchar *key, gboolean def)
{
    BtJson *v = bt_json_get(obj, key);
    return (v != NULL && v->type == BT_JSON_BOOL) ? v->b : def;
}

guint
bt_json_len(BtJson *arr)
{
    return (arr != NULL && arr->type == BT_JSON_ARRAY) ? arr->items->len : 0;
}

BtJson *
bt_json_at(BtJson *arr, guint i)
{
    if (arr == NULL || arr->type != BT_JSON_ARRAY || i >= arr->items->len)
        return NULL;
    return g_ptr_array_index(arr->items, i);
}

/* ---------------------------------------------------------------------------
 * bt_json_write() — compact serializer (see json.h).
 * ------------------------------------------------------------------------- */
void
bt_json_write(GString *out, BtJson *v)
{
    if (v == NULL) {
        g_string_append(out, "null");
        return;
    }
    switch (v->type) {
    case BT_JSON_NULL:
        g_string_append(out, "null");
        break;
    case BT_JSON_BOOL:
        g_string_append(out, v->b ? "true" : "false");
        break;
    case BT_JSON_NUMBER: {
        gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr(buf, sizeof buf, v->num);
        g_string_append(out, buf);
        break;
    }
    case BT_JSON_STRING:
        bt_json_escape(out, v->str);
        break;
    case BT_JSON_ARRAY:
        g_string_append_c(out, '[');
        for (guint i = 0; i < v->items->len; i++) {
            if (i > 0)
                g_string_append_c(out, ',');
            bt_json_write(out, g_ptr_array_index(v->items, i));
        }
        g_string_append_c(out, ']');
        break;
    case BT_JSON_OBJECT:
        g_string_append_c(out, '{');
        for (guint i = 0; i < v->items->len; i++) {
            if (i > 0)
                g_string_append_c(out, ',');
            bt_json_escape(out, g_ptr_array_index(v->keys, i));
            g_string_append_c(out, ':');
            bt_json_write(out, g_ptr_array_index(v->items, i));
        }
        g_string_append_c(out, '}');
        break;
    }
}

/* ---------------------------------------------------------------------------
 * bt_json_escape() — append `s` as a quoted JSON string (see json.h).
 * ------------------------------------------------------------------------- */
void
bt_json_escape(GString *out, const gchar *s)
{
    if (s == NULL) {
        g_string_append(out, "null");
        return;
    }
    g_string_append_c(out, '"');
    for (const gchar *p = s; *p != '\0'; p++) {
        guchar ch = (guchar)*p;
        switch (ch) {
        case '"':  g_string_append(out, "\\\""); break;
        case '\\': g_string_append(out, "\\\\"); break;
        case '\b': g_string_append(out, "\\b");  break;
        case '\f': g_string_append(out, "\\f");  break;
        case '\n': g_string_append(out, "\\n");  break;
        case '\r': g_string_append(out, "\\r");  break;
        case '\t': g_string_append(out, "\\t");  break;
        default:
            if (ch < 0x20)
                g_string_append_printf(out, "\\u%04x", ch);
            else
                g_string_append_c(out, (gchar)ch);
        }
    }
    g_string_append_c(out, '"');
}
