/* ===========================================================================
 * http.c — small libcurl wrapper (see http.h)
 * =========================================================================== */

#include "http.h"
#include <curl/curl.h>
#include <string.h>

/* write_cb() — libcurl write callback appending into a GString.             */
static size_t
write_cb(char *data, size_t size, size_t nmemb, void *user)
{
    GString *buf = user;             /* the accumulating response body      */
    g_string_append_len(buf, data, size * nmemb);
    return size * nmemb;
}

/* ---------------------------------------------------------------------------
 * bt_http_request() — perform one HTTPS request (see http.h).
 * ------------------------------------------------------------------------- */
gchar *
bt_http_request(const gchar *method, const gchar *url,
                const gchar *bearer, const gchar *content_type,
                const gchar *extra_header, const gchar *body,
                glong *status, gchar **err)
{
    *status = 0;
    if (err != NULL)
        *err = NULL;

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        if (err != NULL)
            *err = g_strdup("could not initialize libcurl");
        return NULL;
    }

    GString *buf = g_string_new(NULL);       /* response body accumulator   */
    struct curl_slist *hdrs = NULL;          /* request header list         */
    gchar errbuf[CURL_ERROR_SIZE] = "";      /* libcurl's error detail      */

    if (bearer != NULL) {
        gchar *h = g_strdup_printf("Authorization: Bearer %s", bearer);
        hdrs = curl_slist_append(hdrs, h);
        g_free(h);
    }
    if (content_type != NULL) {
        gchar *h = g_strdup_printf("Content-Type: %s", content_type);
        hdrs = curl_slist_append(hdrs, h);
        g_free(h);
    }
    if (extra_header != NULL)
        hdrs = curl_slist_append(hdrs, extra_header);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  /* threads + timeouts    */
    if (body != NULL) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    } else if (err != NULL) {
        *err = g_strdup(errbuf[0] != '\0' ? errbuf
                                          : curl_easy_strerror(rc));
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        g_string_free(buf, TRUE);
        return NULL;
    }
    return g_string_free(buf, FALSE);
}
