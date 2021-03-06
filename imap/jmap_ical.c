/* jmap_ical.c --Routines to convert calendar events between JMAP and iCalendar
 *
 * Copyright (c) 1994-2016 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>
#include <string.h>
#include <syslog.h>
#include <assert.h>

#include "acl.h"
#include "annotate.h"
#include "append.h"
#include "caldav_db.h"
#include "carddav_db.h"
#include "global.h"
#include "hash.h"
#include "httpd.h"
#include "http_caldav.h"
#include "http_carddav.h"
#include "http_caldav_sched.h"
#include "http_dav.h"
#include "http_jmap.h"
#include "http_proxy.h"
#include "ical_support.h"
#include "json_support.h"
#include "mailbox.h"
#include "mboxlist.h"
#include "mboxname.h"
#include "parseaddr.h"
#include "seen.h"
#include "statuscache.h"
#include "stristr.h"
#include "times.h"
#include "util.h"
#include "vcard_support.h"
#include "version.h"
#include "xmalloc.h"
#include "xsha1.h"
#include "xstrlcat.h"
#include "xstrlcpy.h"
#include "zoneinfo_db.h"

/* for sasl_encode64 */
#include <sasl/sasl.h>
#include <sasl/saslutil.h>

/* generated headers are not necessarily in current directory */
#include "imap/http_err.h"
#include "imap/imap_err.h"

#include "jmap_ical.h"

#define JMAPICAL_READ_MODE       0
#define JMAPICAL_WRITE_MODE      (1<<0)
#define JMAPICAL_EXC_MODE        (1<<8)

typedef struct context {
    jmapical_err_t *err;    /* conversion error, if any */
    jmapical_err_t *_err;   /* conversion error owned by context */

    int mode;               /* Flags indicating the current context mode. */

    /* Property context */
    json_t *invalid;        /* A JSON array of any invalid properties. */
    strarray_t propstr;
    struct buf propbuf;

    /* Conversion to JMAP context */
    json_t *wantprops;         /* which properties to fetch */
    icalcomponent *master;     /* the main event of an exception */
    const char *tzid_start;
    int is_allday;
    const char *uid;

    /* Conversion to iCalendar context */
    icalcomponent *comp;       /* The current main event of an exception. */

    icaltimezone *tzstart_old; /* The former startTimeZone. */
    icaltimezone *tzstart;     /* The current startTimeZone. */
    icaltimezone *tzend_old;   /* The former endTimeZone. */
    icaltimezone *tzend;       /* The current endTimeZone. */
} context_t;

static int is_valid_jmapid(const char *s)
{
    if (!s) return 0;
    size_t i;
    for (i = 0; s[i] && i < 256; i++) {
        char c = s[i];
        if (!((('0' <= c) && (c <= '9')) ||
              (('a' <= c) && (c <= 'z')) ||
              (('A' <= c) && (c <= 'Z')) ||
              ((c == '-' || c == '_')))) {
            return 0;
        }
    }
    return i > 0 && s[i] == '\0';
}

/* Forward declarations */
static json_t *calendarevent_from_ical(context_t *, icalcomponent *);
static void calendarevent_to_ical(context_t *, icalcomponent *, json_t*);

static char *sha1key(const char *val)
{
    unsigned char dest[SHA1_DIGEST_LENGTH];
    char idbuf[2*SHA1_DIGEST_LENGTH+1];
    int r;

    xsha1((const unsigned char *) val, strlen(val), dest);
    r = bin_to_hex(dest, SHA1_DIGEST_LENGTH, idbuf, BH_LOWER);
    assert(r == 2*SHA1_DIGEST_LENGTH);
    idbuf[2*SHA1_DIGEST_LENGTH] = '\0';
    return xstrdup(idbuf);
}

static context_t *context_new(json_t *wantprops,
                              jmapical_err_t *err,
                              int mode)
{
    context_t *ctx = xzmalloc(sizeof(struct context));
    if (!err) {
        ctx->_err = xzmalloc(sizeof(jmapical_err_t));
    }
    ctx->err = err ? err : ctx->_err;
    ctx->wantprops = wantprops;
    ctx->invalid = json_pack("{}");
    ctx->mode = mode;
    return ctx;
}

static void context_free(context_t *ctx)
{
    if (ctx->_err) {
        free(ctx->_err);
    }
    if (ctx->invalid) {
        json_decref(ctx->invalid);
    }
    strarray_fini(&ctx->propstr);
    buf_free(&ctx->propbuf);
    free(ctx);
}

static int wantprop(context_t *ctx, const char *name)
{
    if (!ctx->wantprops) {
        return 1;
    }
    return json_object_get(ctx->wantprops, name) != NULL;
}

static void beginprop_key(context_t *ctx, const char *name, const char *key)
{
    struct buf *buf = &ctx->propbuf;
    strarray_t *str = &ctx->propstr;

    if (json_pointer_needsencode(name)) {
        char *tmp = json_pointer_encode(name);
        buf_setcstr(buf, tmp);
        free(tmp);
    } else {
        buf_setcstr(buf, name);
    }

    buf_appendcstr(buf, "/");

    if (json_pointer_needsencode(key)) {
        char *tmp = json_pointer_encode(key);
        buf_appendcstr(buf, tmp);
        free(tmp);
    } else {
        buf_appendcstr(buf, key);
    }

    strarray_push(str, buf_cstring(buf));
    buf_reset(buf);
}

static void beginprop_idx(context_t *ctx, const char *name, size_t idx)
{
    struct buf *buf = &ctx->propbuf;
    strarray_t *str = &ctx->propstr;

    if (json_pointer_needsencode(name)) {
        char *tmp = json_pointer_encode(name);
        buf_setcstr(buf, tmp);
        free(tmp);
    } else {
        buf_setcstr(buf, name);
    }

    buf_appendcstr(buf, "/");
    buf_printf(buf, "%zu", idx);

    strarray_push(str, buf_cstring(buf));
    buf_reset(buf);
}

static void beginprop(context_t *ctx, const char *name)
{
    strarray_t *str = &ctx->propstr;

    if (json_pointer_needsencode(name)) {
        char *tmp = json_pointer_encode(name);
        strarray_push(str, tmp);
        free(tmp);
    } else {
        strarray_push(str, name);
    }
}

static void endprop(context_t *ctx)
{
    strarray_t *str = &ctx->propstr;
    assert(strarray_size(str));
    free(strarray_pop(str));
}

static char* encodeprop(context_t *ctx, const char *name)
{
    struct buf *buf = &ctx->propbuf;
    strarray_t *str = &ctx->propstr;
    int i;

    if (!name && !strarray_size(str)) {
        return NULL;
    }

    if (name) beginprop(ctx, name);

    buf_setcstr(buf, strarray_nth(str, 0));
    for (i = 1; i < strarray_size(str); i++) {
        buf_appendcstr(buf, "/");
        buf_appendcstr(buf, strarray_nth(str, i));
    }

    if (name) endprop(ctx);

    return buf_newcstring(buf);
}

static void invalidprop(context_t *ctx, const char *name)
{
    char *tmp = encodeprop(ctx, name);
    json_object_set_new(ctx->invalid, tmp, json_null());
    free(tmp);
}

static void invalidprop_append(context_t *ctx, json_t *props)
{
    size_t i;
    json_t *val;
    struct buf buf = BUF_INITIALIZER;

    json_array_foreach(props, i, val) {
        const char *raw;
        char *tmp;

        raw = json_string_value(val);
        tmp = encodeprop(ctx, NULL);
        buf_setcstr(&buf, tmp);
        buf_appendcstr(&buf, "/");
        buf_appendcstr(&buf, raw);
        json_object_set_new(ctx->invalid, buf_cstring(&buf), json_null());
        free(tmp);
    }

    buf_free(&buf);
}

static int have_invalid_props(context_t *ctx)
{
    return json_object_size(ctx->invalid) > 0;
}

static size_t invalid_prop_count(context_t *ctx)
{
    return json_object_size(ctx->invalid);
}

static json_t* get_invalid_props(context_t *ctx)
{
    json_t *props = json_pack("[]");
    const char *key;
    json_t *val;

    if (!ctx->invalid)
        return NULL;

    json_object_foreach(ctx->invalid, key, val) {
        json_array_append_new(props, json_string(key));
    }

    if (!json_array_size(props)) {
        json_decref(props);
        props = NULL;
    }

    return props;
}

/* Read the property named name into dst, formatted according to the json
 * unpack format fmt. Report missing or erroneous properties.
 *
 * Return a negative value for a missing or invalid property.
 * Return a positive value if a property was read, zero otherwise. */
static int readprop(context_t *ctx, json_t *from, const char *name,
                    int is_mandatory, const char *fmt, void *dst)
{
    int r = 0;
    json_t *jval = json_object_get(from, name);
    if (!jval && is_mandatory) {
        r = -1;
    } else if (jval) {
        json_error_t err;
        if (json_unpack_ex(jval, &err, 0, fmt, dst)) {
            r = -2;
        } else {
            r = 1;
        }
    }
    if (r < 0) {
        invalidprop(ctx, name);
    }
    return r;
}

static char *mailaddr_from_uri(const char *uri)
{
    if (!uri || strncasecmp(uri, "mailto:", 7)) {
        return NULL;
    }
    uri += 7;
    const char *p = strchr(uri, '?');
    if (!p) return address_canonicalise(uri);

    char *tmp = xstrndup(uri, p - uri);
    char *ret = address_canonicalise(uri);
    free(tmp);
    return ret;
}

static char *normalized_uri(const char *uri)
{
    const char *col = strchr(uri, ':');
    if (!col) return xstrdupnull(uri);

    struct buf buf = BUF_INITIALIZER;
    buf_setmap(&buf, uri, col - uri);
    buf_lcase(&buf);
    buf_appendcstr(&buf, col);
    return buf_release(&buf);
}

static char *mailaddr_to_uri(const char *addr)
{
    struct buf buf = BUF_INITIALIZER;
    buf_setcstr(&buf, "mailto:");
    buf_appendcstr(&buf, addr);
    return buf_release(&buf);
}

static void remove_icalxparam(icalproperty *prop, const char *name)
{
    icalparameter *param, *next;

    for (param = icalproperty_get_first_parameter(prop, ICAL_X_PARAMETER);
         param;
         param = next) {

        next = icalproperty_get_next_parameter(prop, ICAL_X_PARAMETER);
        if (strcasecmp(icalparameter_get_xname(param), name)) {
            continue;
        }
        icalproperty_remove_parameter_by_ref(prop, param);
    }
}


static const char*
get_icalxparam_value(icalproperty *prop, const char *name)
{
    icalparameter *param;

    for (param = icalproperty_get_first_parameter(prop, ICAL_X_PARAMETER);
         param;
         param = icalproperty_get_next_parameter(prop, ICAL_X_PARAMETER)) {

        if (strcasecmp(icalparameter_get_xname(param), name)) {
            continue;
        }
        return icalparameter_get_xvalue(param);
    }

    return NULL;
}

static void
set_icalxparam(icalproperty *prop, const char *name, const char *val, int purge)
{
    icalparameter *param;

    if (purge) remove_icalxparam(prop, name);

    param = icalparameter_new(ICAL_X_PARAMETER);
    icalparameter_set_xname(param, name);
    icalparameter_set_xvalue(param, val);
    icalproperty_add_parameter(prop, param);
}

/* Compare the value of the first occurences of property kind in components
 * a and b. Return 0 if they match or if both do not contain kind. Note that
 * this function does not define an order on property values, so it can't be
 * used for sorting. */
int compare_icalprop(icalcomponent *a, icalcomponent *b,
                     icalproperty_kind kind) {
    icalproperty *pa, *pb;
    icalvalue *va, *vb;

    pa = icalcomponent_get_first_property(a, kind);
    pb = icalcomponent_get_first_property(b, kind);
    if (!pa && !pb) {
        return 0;
    }

    va = icalproperty_get_value(pa);
    vb = icalproperty_get_value(pb);
    enum icalparameter_xliccomparetype cmp = icalvalue_compare(va, vb);
    return cmp != ICAL_XLICCOMPARETYPE_EQUAL;
}

static const char*
get_icalxprop_value(icalcomponent *comp, const char *name)
{
    icalproperty *prop;

    for (prop = icalcomponent_get_first_property(comp, ICAL_X_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_X_PROPERTY)) {

        if (strcasecmp(icalproperty_get_x_name(prop), name)) {
            continue;
        }
        return icalproperty_get_value_as_string(prop);
    }

    return NULL;
}

/* Remove and deallocate any x-properties with name in comp. */
static void remove_icalxprop(icalcomponent *comp, const char *name)
{
    icalproperty *prop, *next;
    icalproperty_kind kind = ICAL_X_PROPERTY;

    for (prop = icalcomponent_get_first_property(comp, kind);
         prop;
         prop = next) {

        next = icalcomponent_get_next_property(comp, kind);

        if (strcasecmp(icalproperty_get_x_name(prop), name))
            continue;

        icalcomponent_remove_property(comp, prop);
        icalproperty_free(prop);
    }
}

static char *xjmapid_from_ical(icalproperty *prop)
{
    const char *id = (char *) get_icalxparam_value(prop, JMAPICAL_XPARAM_ID);
    if (id) return xstrdup(id);
    return sha1key(icalproperty_as_ical_string(prop));
}

static void xjmapid_to_ical(icalproperty *prop, const char *id)
{
    struct buf buf = BUF_INITIALIZER;
    icalparameter *param;

    buf_setcstr(&buf, JMAPICAL_XPARAM_ID);
    buf_appendcstr(&buf, "=");
    buf_appendcstr(&buf, id);
    param = icalparameter_new_from_string(buf_cstring(&buf));
    icalproperty_add_parameter(prop, param);

    buf_free(&buf);
}

static icaltimezone *tz_from_tzid(const char *tzid)
{
    if (!tzid)
        return NULL;

    /* libical doesn't return the UTC singleton for Etc/UTC */
    if (!strcmp(tzid, "Etc/UTC") || !strcmp(tzid, "UTC"))
        return icaltimezone_get_utc_timezone();

    return icaltimezone_get_builtin_timezone(tzid);
}

/* Determine the Olson TZID, if any, of the ical property prop. */
static const char *tzid_from_icalprop(icalproperty *prop, int guess) {
    const char *tzid = NULL;
    icalparameter *param = NULL;

    if (prop) param = icalproperty_get_first_parameter(prop, ICAL_TZID_PARAMETER);
    if (param) tzid = icalparameter_get_tzid(param);
    /* Check if the tzid already corresponds to an Olson name. */
    if (tzid) {
        icaltimezone *tz = tz_from_tzid(tzid);
        if (!tz && guess) {
            /* Try to guess the timezone. */
            icalvalue *val = icalproperty_get_value(prop);
            icaltimetype dt = icalvalue_get_datetime(val);
            tzid = dt.zone ? icaltimezone_get_location((icaltimezone*) dt.zone) : NULL;
            tzid = tzid && tz_from_tzid(tzid) ? tzid : NULL;
        }
    } else {
        icalvalue *val = icalproperty_get_value(prop);
        icaltimetype dt = icalvalue_get_datetime(val);
        if (icaltime_is_valid_time(dt) && icaltime_is_utc(dt)) {
            tzid = "Etc/UTC";
        }
    }
    return tzid;
}

/* Determine the Olson TZID, if any, of the ical property kind in component comp. */
static const char *tzid_from_ical(icalcomponent *comp,
                                  icalproperty_kind kind) {
    icalproperty *prop = icalcomponent_get_first_property(comp, kind);
    if (!prop) {
        return NULL;
    }
    return tzid_from_icalprop(prop, 1/*guess*/);
}

static struct icaltimetype dtstart_from_ical(icalcomponent *comp)
{
    struct icaltimetype dt;
    const char *tzid;

    dt = icalcomponent_get_dtstart(comp);
    if (dt.zone) return dt;

    if ((tzid = tzid_from_ical(comp, ICAL_DTSTART_PROPERTY))) {
        dt.zone = tz_from_tzid(tzid);
    }

    return dt;
}

static struct icaltimetype dtend_from_ical(icalcomponent *comp)
{
    struct icaltimetype dt;
    icalproperty *prop;
    const char *tzid;

    /* Handles DURATION vs DTEND */
    dt = icalcomponent_get_dtend(comp);
    if (dt.zone) return dt;

    prop = icalcomponent_get_first_property(comp, ICAL_DTEND_PROPERTY);
    if (prop) {
        if ((tzid = tzid_from_icalprop(prop, 1))) {
            dt.zone = tz_from_tzid(tzid);
        }
    } else {
        dt.zone = dtstart_from_ical(comp).zone;
    }

    return dt;
}


/* Convert time t to a RFC3339 formatted localdate string. Return the number
 * of bytes written to buf sized size, excluding the terminating null byte. */
static int timet_to_localdate(time_t t, char* buf, size_t size) {
    int n = time_to_rfc3339(t, buf, size);
    if (n && buf[n-1] == 'Z') {
        buf[n-1] = '\0';
        n--;
    }
    return n;
}

/* Convert icaltime to a RFC3339 formatted localdate string.
 * The returned string is owned by the caller or NULL on error.
 */
static char* localdate_from_icaltime_r(icaltimetype icaltime) {
    char *s;
    time_t t;

    s = xzmalloc(RFC3339_DATETIME_MAX);
    if (!s) {
        return NULL;
    }

    t = icaltime_as_timet(icaltime);
    if (!timet_to_localdate(t, s, RFC3339_DATETIME_MAX)) {
        return NULL;
    }
    return s;
}

/* Convert icaltime to a RFC3339 formatted string.
 *
 * The returned string is owned by the caller or NULL on error.
 */
static char* utcdate_from_icaltime_r(icaltimetype icaltime) {
    char *s;
    time_t t;
    int n;

    s = xzmalloc(RFC3339_DATETIME_MAX);
    if (!s) {
        return NULL;
    }

    t = icaltime_as_timet(icaltime);

    n = time_to_rfc3339(t, s, RFC3339_DATETIME_MAX);
    if (!n) {
        free(s);
        return NULL;
    }
    return s;
}

/* Compare int in ascending order. */
static int compare_int(const void *aa, const void *bb)
{
    const int *a = aa, *b = bb;
    return (*a < *b) ? -1 : (*a > *b);
}

/* Return the identity of i. This is a helper for recur_byX. */
static int identity_int(int i) {
    return i;
}

/*
 * Conversion from iCalendar to JMAP
 */

/* Convert at most nmemb entries in the ical recurrence byDay/Month/etc array
 * named byX using conv. Return a new JSON array, sorted in ascending order. */
static json_t* recurrence_byX_fromical(short byX[], size_t nmemb, int (*conv)(int)) {
    json_t *jbd = json_pack("[]");

    size_t i;
    int tmp[nmemb];
    for (i = 0; i < nmemb && byX[i] != ICAL_RECURRENCE_ARRAY_MAX; i++) {
        tmp[i] = conv(byX[i]);
    }

    size_t n = i;
    qsort(tmp, n, sizeof(int), compare_int);
    for (i = 0; i < n; i++) {
        json_array_append_new(jbd, json_pack("i", tmp[i]));
    }

    return jbd;
}

/* Convert the ical recurrence recur to a JMAP recurrenceRule */
static json_t*
recurrence_from_ical(context_t *ctx, icalcomponent *comp)
{
    char *s = NULL;
    size_t i;
    json_t *recur;
    struct buf buf = BUF_INITIALIZER;
    icalproperty *prop;
    struct icalrecurrencetype rrule;
    const char *tzid = ctx->tzid_start;

    prop = icalcomponent_get_first_property(comp, ICAL_RRULE_PROPERTY);
    if (!prop) {
        return json_null();
    }
    rrule = icalproperty_get_rrule(prop);

    recur = json_pack("{}");
    /* frequency */
    s = xstrdup(icalrecur_freq_to_string(rrule.freq));
    s = lcase(s);
    json_object_set_new(recur, "frequency", json_string(s));
    free(s);

    if (rrule.interval > 1) {
        json_object_set_new(recur, "interval", json_pack("i", rrule.interval));
    }

#ifdef HAVE_RSCALE
    /* rscale */
    if (rrule.rscale) {
        s = xstrdup(rrule.rscale);
        s = lcase(s);
        json_object_set_new(recur, "rscale", json_string(s));
        free(s);
    }

    /* skip */
    switch (rrule.skip) {
        case ICAL_SKIP_BACKWARD:
            s = "backward";
            break;
        case ICAL_SKIP_FORWARD:
            s = "forward";
            break;
        case ICAL_SKIP_OMIT:
            /* fall through */
        default:
            s = NULL;
    }
    if (s) json_object_set_new(recur, "skip", json_string(s));
#endif

    /* firstDayOfWeek */
    s = xstrdup(icalrecur_weekday_to_string(rrule.week_start));
    s = lcase(s);
    if (strcmp(s, "mo")) {
        json_object_set_new(recur, "firstDayOfWeek", json_string(s));
    }
    free(s);

    /* byDay */
    json_t *jbd = json_pack("[]");
    for (i = 0; i < ICAL_BY_DAY_SIZE; i++) {
        json_t *jday;
        icalrecurrencetype_weekday weekday;
        int pos;

        if (rrule.by_day[i] == ICAL_RECURRENCE_ARRAY_MAX) {
            break;
        }

        jday = json_pack("{}");
        weekday = icalrecurrencetype_day_day_of_week(rrule.by_day[i]);

        s = xstrdup(icalrecur_weekday_to_string(weekday));
        s = lcase(s);
        json_object_set_new(jday, "day", json_string(s));
        free(s);

        pos = icalrecurrencetype_day_position(rrule.by_day[i]);
        if (pos) {
            json_object_set_new(jday, "nthOfPeriod", json_integer(pos));
        }

        if (json_object_size(jday)) {
            json_array_append_new(jbd, jday);
        } else {
            json_decref(jday);
        }
    }
    if (json_array_size(jbd)) {
        json_object_set_new(recur, "byDay", jbd);
    } else {
        json_decref(jbd);
    }

    /* byMonth */
    json_t *jbm = json_pack("[]");
    for (i = 0; i < ICAL_BY_MONTH_SIZE; i++) {
        short bymonth;

        if (rrule.by_month[i] == ICAL_RECURRENCE_ARRAY_MAX) {
            break;
        }

        bymonth = rrule.by_month[i];
        buf_printf(&buf, "%d", icalrecurrencetype_month_month(bymonth));
        if (icalrecurrencetype_month_is_leap(bymonth)) {
            buf_appendcstr(&buf, "L");
        }
        json_array_append_new(jbm, json_string(buf_cstring(&buf)));
        buf_reset(&buf);

    }
    if (json_array_size(jbm)) {
        json_object_set_new(recur, "byMonth", jbm);
    } else {
        json_decref(jbm);
    }

    if (rrule.by_month_day[0] != ICAL_RECURRENCE_ARRAY_MAX) {
        json_object_set_new(recur, "byDate",
                recurrence_byX_fromical(rrule.by_month_day,
                    ICAL_BY_MONTHDAY_SIZE, &identity_int));
    }
    if (rrule.by_year_day[0] != ICAL_RECURRENCE_ARRAY_MAX) {
        json_object_set_new(recur, "byYearDay",
                recurrence_byX_fromical(rrule.by_year_day,
                    ICAL_BY_YEARDAY_SIZE, &identity_int));
    }
    if (rrule.by_week_no[0] != ICAL_RECURRENCE_ARRAY_MAX) {
        json_object_set_new(recur, "byWeekNo",
                recurrence_byX_fromical(rrule.by_week_no,
                    ICAL_BY_WEEKNO_SIZE, &identity_int));
    }
    if (rrule.by_hour[0] != ICAL_RECURRENCE_ARRAY_MAX) {
        json_object_set_new(recur, "byHour",
                recurrence_byX_fromical(rrule.by_hour,
                    ICAL_BY_HOUR_SIZE, &identity_int));
    }
    if (rrule.by_minute[0] != ICAL_RECURRENCE_ARRAY_MAX) {
        json_object_set_new(recur, "byMinute",
                recurrence_byX_fromical(rrule.by_minute,
                    ICAL_BY_MINUTE_SIZE, &identity_int));
    }
    if (rrule.by_second[0] != ICAL_RECURRENCE_ARRAY_MAX) {
        json_object_set_new(recur, "bySecond",
                recurrence_byX_fromical(rrule.by_second,
                    ICAL_BY_SECOND_SIZE, &identity_int));
    }
    if (rrule.by_set_pos[0] != ICAL_RECURRENCE_ARRAY_MAX) {
        json_object_set_new(recur, "bySetPosition",
                recurrence_byX_fromical(rrule.by_set_pos,
                    ICAL_BY_SETPOS_SIZE, &identity_int));
    }

    if (rrule.count != 0) {
        /* Recur count takes precedence over until. */
        json_object_set_new(recur, "count", json_integer(rrule.count));
    } else if (!icaltime_is_null_time(rrule.until)) {
        icaltimezone *tz = tz_from_tzid(tzid);
        icaltimetype dtloc = icaltime_convert_to_zone(rrule.until, tz);
        char *until = localdate_from_icaltime_r(dtloc);
        if (until == NULL) {
            ctx->err->code = JMAPICAL_ERROR_MEMORY;
            return NULL;
        }
        json_object_set_new(recur, "until", json_string(until));
        free(until);
    }

    if (!json_object_size(recur)) {
        json_decref(recur);
        recur = json_null();
    }

    buf_free(&buf);
    return recur;
}

static json_t*
override_rdate_from_ical(context_t *ctx __attribute__((unused)),
                         icalproperty *prop)
{
    /* returns a JSON object with a single key value pair */
    json_t *override = json_pack("{}");
    json_t *o = json_pack("{}");
    struct icaldatetimeperiodtype rdate = icalproperty_get_rdate(prop);
    icaltimetype id;

    if (!icaltime_is_null_time(rdate.time)) {
        id = rdate.time;
    } else {
        /* PERIOD */
        struct icaldurationtype dur;
        id = rdate.period.start;

        /* Determine duration */
        if (!icaltime_is_null_time(rdate.period.end)) {
            dur = icaltime_subtract(rdate.period.end, id);
        } else {
            dur = rdate.period.duration;
        }

        json_object_set_new(o, "duration",
                json_string(icaldurationtype_as_ical_string(dur)));
    }

    if (!icaltime_is_null_time(id)) {
        char *t = localdate_from_icaltime_r(id);
        json_object_set_new(override, t, o);
        free(t);
    }

    if (!json_object_size(override)) {
        json_decref(override);
        json_decref(o);
        override = NULL;
    }
    return override;
}

static json_t*
override_exdate_from_ical(context_t *ctx, icalproperty *prop)
{
    json_t *override = json_pack("{}");
    icaltimetype id = icalproperty_get_exdate(prop);
    const char *tzid_xdate;

    tzid_xdate = tzid_from_icalprop(prop, 1);
    if (ctx->tzid_start && tzid_xdate && strcmp(ctx->tzid_start, tzid_xdate)) {
        icaltimezone *tz_xdate = tz_from_tzid(tzid_xdate);
        icaltimezone *tz_start = tz_from_tzid(ctx->tzid_start);
        if (tz_xdate && tz_start) {
            if (id.zone) id.zone = tz_xdate;
            id = icaltime_convert_to_zone(id, tz_start);
        }
    }

    if (!icaltime_is_null_time(id)) {
        char *t = localdate_from_icaltime_r(id);
        json_object_set_new(override, t, json_pack("{s:b}", "excluded", 1));
        free(t);
    }

    if (!json_object_size(override)) {
        json_decref(override);
        override = NULL;
    }

    return override;
}

static json_t*
overrides_from_ical(context_t *ctx, icalcomponent *comp, json_t *event)
{
    icalproperty *prop;
    json_t *overrides = json_pack("{}");

    /* RDATE */
    for (prop = icalcomponent_get_first_property(comp, ICAL_RDATE_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_RDATE_PROPERTY)) {

        json_t *override = override_rdate_from_ical(ctx, prop);
        if (override) {
            json_object_update(overrides, override);
            json_decref(override);
        }
    }

    /* EXDATE */
    for (prop = icalcomponent_get_first_property(comp, ICAL_EXDATE_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_EXDATE_PROPERTY)) {

        json_t *override = override_exdate_from_ical(ctx, prop);
        if (override) {
            json_object_update(overrides, override);
            json_decref(override);
        }
    }

    /* VEVENT exceptions */
    json_t *exceptions = json_pack("{}");
    icalcomponent *excomp, *ical;

    ical = icalcomponent_get_parent(comp);
    for (excomp = icalcomponent_get_first_component(ical, ICAL_VEVENT_COMPONENT);
         excomp;
         excomp = icalcomponent_get_next_component(ical, ICAL_VEVENT_COMPONENT)) {

        if (excomp == comp) continue; /* skip toplevel promoted object */

        /* Skip unrelated VEVENTs */
        const char *exuid = icalcomponent_get_uid(excomp);
        if (!exuid || strcmp(exuid, ctx->uid)) continue;

        context_t *myctx;
        json_t *ex, *diff;
        struct icaltimetype recurid;
        char *s;
        const char *exstart;

        /* Convert VEVENT exception to JMAP */
        myctx = context_new(ctx->wantprops, ctx->err, JMAPICAL_READ_MODE);
        myctx->master = comp;
        ex = calendarevent_from_ical(myctx, excomp);
        context_free(myctx);
        if (!ex) {
            continue;
        }
        json_object_del(ex, "updated");
        json_object_del(ex, "created");

        /* Determine recurrence id */
        recurid = icalcomponent_get_recurrenceid(excomp);
        s = localdate_from_icaltime_r(recurid);
        exstart = json_string_value(json_object_get(ex, "start"));
        if (exstart && !strcmp(exstart, s)) {
            json_object_del(ex, "start");
        }

        /* Create override patch */
        diff = jmap_patchobject_create(event, ex);
        json_decref(ex);

        /* Set override at recurrence id */
        json_object_set_new(exceptions, s, diff);
        free(s);
    }

    json_object_update(overrides, exceptions);
    json_decref(exceptions);

    if (!json_object_size(overrides)) {
        json_decref(overrides);
        overrides = json_null();
    }

    return overrides;
}

static int match_uri(const char *uri1, const char *uri2)
{
    const char *col1 = strchr(uri1, ':');
    const char *col2 = strchr(uri2, ':');

    if (col1 == NULL && col2 == NULL) {
        return !strcmp(uri1, uri2);
    }
    else if (col1 && col2 && (col1-uri1) == (col2-uri2)) {
        size_t schemelen = col1-uri1;
        return !strncasecmp(uri1, uri2, schemelen) &&
               !strcmp(uri1+schemelen, uri2+schemelen);
    }
    else return 0;
}

static json_t*
rsvpto_from_ical(icalproperty *prop)
{
    json_t *rsvpTo = json_object();
    struct buf buf = BUF_INITIALIZER;

    /* Read RVSP methods defined in RSVP-URI x-parameters. A RSVP-URI
     * x-parameter value is of the form method:uri. If no method is defined,
     * it's interpreted as the "web" method for legacy reasons. */
    icalparameter *param, *next;
    for (param = icalproperty_get_first_parameter(prop, ICAL_X_PARAMETER);
            param;
            param = next) {

        next = icalproperty_get_next_parameter(prop, ICAL_X_PARAMETER);
        if (strcasecmp(icalparameter_get_xname(param), JMAPICAL_XPARAM_RSVP_URI)) {
            continue;
        }

        const char *val = icalparameter_get_xvalue(param);
        const char *col1 = strchr(val, ':');
        const char *col2 = col1 ? strchr(col1 + 1, ':') : NULL;
        if (!col2) {
            json_object_set_new(rsvpTo, "web", json_string(val));
        } else {
            buf_setmap(&buf, val, col1 - val);
            json_object_set_new(rsvpTo, buf_cstring(&buf), json_string(col1 + 1));
        }
    }

    /* Read URI from property value and check if this URI already is defined.
     * If it isn't, this could be because an iCalendar client updated the
     * property value, but kept the RSVP x-params. */
    const char *caladdress = icalproperty_get_value_as_string(prop);
    int caladdress_is_defined = 0;
    json_t *jval;
    const char *key;
    json_object_foreach(rsvpTo, key, jval) {
        if (match_uri(caladdress, json_string_value(jval))) {
            caladdress_is_defined = 1;
            break;
        }
    }
    if (!caladdress_is_defined) {
        if (!strncasecmp(caladdress, "mailto:", 7))
            json_object_set_new(rsvpTo, "imip", json_string(caladdress));
        else
            json_object_set_new(rsvpTo, "other", json_string(caladdress));
    }

    if (!json_object_size(rsvpTo)) {
        json_decref(rsvpTo);
        rsvpTo = json_null();
    }

    buf_free(&buf);
    return rsvpTo;
}

static json_t *participant_from_ical(icalproperty *prop,
                                     hash_table *attendee_by_uri,
                                     hash_table *id_by_uri,
                                     icalproperty *orga)
{
    json_t *p = json_object();
    icalparameter *param;
    struct buf buf = BUF_INITIALIZER;

    /* FIXME invitedBy */

    /* sendTo */
    json_t *sendTo = rsvpto_from_ical(prop);
    json_object_set_new(p, "sendTo", sendTo ? sendTo : json_null());

    /* email */
    char *email = NULL;
    param = icalproperty_get_first_parameter(prop, ICAL_EMAIL_PARAMETER);
    if (param) {
        email = xstrdupnull(icalparameter_get_value_as_string(param));
    }
    else if (json_object_get(sendTo, "imip")) {
        const char *uri = json_string_value(json_object_get(sendTo, "imip"));
        email = mailaddr_from_uri(uri);
    }
    json_object_set_new(p, "email", email ? json_string(email) : json_null());
    free(email);

    /* name */
    const char *name = NULL;
    param = icalproperty_get_first_parameter(prop, ICAL_CN_PARAMETER);
    if (param) {
        name = icalparameter_get_cn(param);
    }
    json_object_set_new(p, "name", json_string(name ? name : ""));

    /* kind */
    const char *kind = NULL;
    param = icalproperty_get_first_parameter(prop, ICAL_CUTYPE_PARAMETER);
    if (param) {
        icalparameter_cutype cutype = icalparameter_get_cutype(param);
        switch (cutype) {
            case ICAL_CUTYPE_INDIVIDUAL:
                kind = "individual";
                break;
            case ICAL_CUTYPE_GROUP:
                kind = "group";
                break;
            case ICAL_CUTYPE_RESOURCE:
                kind = "resource";
                break;
            case ICAL_CUTYPE_ROOM:
                kind = "location";
                break;
            default:
                kind = "unknown";
        }
    }
    if (kind) {
        json_object_set_new(p, "kind", json_string(kind));
    }

    /* attendance */
    const char *attendance = NULL;
    icalparameter_role ical_role = ICAL_ROLE_REQPARTICIPANT;
    param = icalproperty_get_first_parameter(prop, ICAL_ROLE_PARAMETER);
    if (param) {
        ical_role = icalparameter_get_role(param);
        switch (ical_role) {
            case ICAL_ROLE_REQPARTICIPANT:
                attendance = "required";
                break;
            case ICAL_ROLE_OPTPARTICIPANT:
                attendance = "optional";
                break;
            case ICAL_ROLE_NONPARTICIPANT:
                attendance = "none";
                break;
            case ICAL_ROLE_CHAIR:
                /* fall through */
            default:
                attendance = "required";
        }
    }
    if (attendance) {
        json_object_set_new(p, "attendance", json_string(attendance));
    }

    /* roles */
    json_t *roles = json_object();
    for (param = icalproperty_get_first_parameter(prop, ICAL_X_PARAMETER);
         param;
         param = icalproperty_get_next_parameter(prop, ICAL_X_PARAMETER)) {

        if (strcmp(icalparameter_get_xname(param), JMAPICAL_XPARAM_ROLE))
            continue;

        buf_setcstr(&buf, icalparameter_get_xvalue(param));
        json_object_set_new(roles, buf_lcase(&buf), json_true());
    }
    if (!json_object_get(roles, "owner")) {
        const char *o = icalproperty_get_organizer(orga);
        const char *a = icalproperty_get_attendee(prop);
        if (!strcasecmpsafe(o, a)) {
            json_object_set_new(roles, "owner", json_true());
            json_object_set_new(roles, "attendee", json_true());
        }
    }
    if (ical_role == ICAL_ROLE_CHAIR) {
        json_object_set_new(roles, "chair", json_true());
    }
    if (!json_object_size(roles)) {
        json_object_set_new(roles, "attendee", json_true());
    }
    json_object_set_new(p, "roles", roles);

    /* locationId */
    const char *locid;
    if ((locid = get_icalxparam_value(prop, JMAPICAL_XPARAM_LOCATIONID))) {
        json_object_set_new(p, "locationId", json_string(locid));
    }

    /* participationStatus */
    const char *partstat = NULL;
    short depth = 0;
    icalproperty *partstat_prop = prop;
    while (!partstat) {
        param = icalproperty_get_first_parameter(partstat_prop, ICAL_PARTSTAT_PARAMETER);
        if (!param) break;
        icalparameter_partstat pst = icalparameter_get_partstat(param);
        switch (pst) {
            case ICAL_PARTSTAT_ACCEPTED:
                partstat = "accepted";
                break;
            case ICAL_PARTSTAT_DECLINED:
                partstat = "declined";
                break;
            case ICAL_PARTSTAT_TENTATIVE:
                partstat = "tentative";
                break;
            case ICAL_PARTSTAT_NEEDSACTION:
                partstat = "needs-action";
                break;
            case ICAL_PARTSTAT_DELEGATED:
                /* Follow the delegate chain */
                param = icalproperty_get_first_parameter(prop, ICAL_DELEGATEDTO_PARAMETER);
                if (param) {
                    const char *to = icalparameter_get_delegatedto(param);
                    if (!to) continue;
                    char *uri = normalized_uri(to);
                    partstat_prop = hash_lookup(uri, attendee_by_uri);
                    free(uri);
                    if (partstat_prop) {
                        /* Determine PARTSTAT from delegate. */
                        if (++depth > 64) {
                            /* This is a pathological case: libical does
                             * not check for infinite DELEGATE chains, so we
                             * make sure not to fall in an endless loop. */
                            partstat = "none";
                        }
                        continue;
                    }
                }
                /* fallthrough */
            default:
                partstat = "none";
        }
    }
    if (partstat && strcmp(partstat,  "none")) {
        json_object_set_new(p, "participationStatus", json_string(partstat));
    }

    /* expectReply */
    param = icalproperty_get_first_parameter(prop, ICAL_RSVP_PARAMETER);
    if (param) {
        icalparameter_rsvp val = icalparameter_get_rsvp(param);
        json_object_set_new(p, "expectReply",
                json_boolean(val == ICAL_RSVP_TRUE));
    }

    /* delegatedTo */
    json_t *delegatedTo = json_object();
    for (param = icalproperty_get_first_parameter(prop, ICAL_DELEGATEDTO_PARAMETER);
         param;
         param = icalproperty_get_next_parameter(prop, ICAL_DELEGATEDTO_PARAMETER)) {

        char *uri = normalized_uri(icalparameter_get_delegatedto(param));
        const char *to_id = hash_lookup(uri, id_by_uri);
        free(uri);
        if (to_id) json_object_set_new(delegatedTo, to_id, json_true());
    }
    if (json_object_size(delegatedTo)) {
        json_object_set_new(p, "delegatedTo", delegatedTo);
    }
    else {
        json_decref(delegatedTo);
    }

    /* delegatedFrom */
    json_t *delegatedFrom = json_object();
    for (param = icalproperty_get_first_parameter(prop, ICAL_DELEGATEDFROM_PARAMETER);
         param;
         param = icalproperty_get_next_parameter(prop, ICAL_DELEGATEDFROM_PARAMETER)) {

        char *uri = normalized_uri(icalparameter_get_delegatedfrom(param));
        const char *from_id = hash_lookup(uri, id_by_uri);
        free(uri);
        if (from_id) json_object_set_new(delegatedFrom, from_id, json_true());
    }
    if (json_object_size(delegatedFrom)) {
        json_object_set_new(p, "delegatedFrom", delegatedFrom);
    }
    else {
        json_decref(delegatedFrom);
    }

    /* memberof */
    json_t *memberOf = json_object();
    for (param = icalproperty_get_first_parameter(prop, ICAL_MEMBER_PARAMETER);
         param;
         param = icalproperty_get_next_parameter(prop, ICAL_MEMBER_PARAMETER)) {

        char *uri = normalized_uri(icalparameter_get_member(param));
        char *id = xstrdupnull(hash_lookup(uri, id_by_uri));
        if (!id) id = sha1key(uri);
        json_object_set_new(memberOf, id, json_true());
        free(id);
        free(uri);
    }
    if (json_object_size(memberOf)) {
        json_object_set_new(p, "memberOf", memberOf);
    } else {
        json_decref(memberOf);
    }

    /* linkIds */
    json_t *linkIds = json_object();
    for (param = icalproperty_get_first_parameter(prop, ICAL_X_PARAMETER);
         param;
         param = icalproperty_get_next_parameter(prop, ICAL_X_PARAMETER)) {

        if (strcmp(icalparameter_get_xname(param), JMAPICAL_XPARAM_LINKID))
            continue;

        buf_setcstr(&buf, icalparameter_get_xvalue(param));
        json_object_set_new(linkIds, buf_lcase(&buf), json_true());
    }
    if (json_object_size(linkIds)) {
        json_object_set_new(p, "linkIds", linkIds);
    }
    else {
        json_decref(linkIds);
    } 

    /* scheduleSequence */
    const char *xval = get_icalxparam_value(prop, JMAPICAL_XPARAM_SEQUENCE);
    if (xval) {
        bit64 res;
        if (parsenum(xval, &xval, strlen(xval), &res) == 0) {
            json_object_set_new(p, "scheduleSequence", json_integer(res));
        }
    }

    /* scheduleUpdated */
    if ((xval = get_icalxparam_value(prop, JMAPICAL_XPARAM_DTSTAMP))) {
        icaltimetype dtstamp = icaltime_from_string(xval);
        if (!icaltime_is_null_time(dtstamp) && !dtstamp.is_date &&
                dtstamp.zone == icaltimezone_get_utc_timezone()) {
            char *tmp = utcdate_from_icaltime_r(dtstamp);
            json_object_set_new(p, "scheduleUpdated", json_string(tmp));
            free(tmp);
        }
    }

    buf_free(&buf);
    return p;
}

static json_t*
participant_from_icalorganizer(icalproperty *orga)
{
    json_t *jorga = json_object();

    /* name */
    icalparameter *param;
    const char *name = NULL;
    if ((param = icalproperty_get_first_parameter(orga, ICAL_CN_PARAMETER))) {
        name = icalparameter_get_cn(param);
    }
    json_object_set_new(jorga, "name", json_string(name ? name : ""));

    /* roles */
    json_object_set_new(jorga, "roles", json_pack("{s:b}", "owner", 1));

    /* sendTo */
    /* email */
    const char *caladdress = icalproperty_get_value_as_string(orga);
    if (!strncasecmp(caladdress, "mailto:", 7)) {
        json_object_set_new(jorga, "sendTo", json_pack("{s:s}", "imip", caladdress));
        char *email = mailaddr_from_uri(caladdress);
        json_object_set_new(jorga, "email", json_string(email));
        free(email);
    }
    else {
        json_object_set_new(jorga, "sendTo", json_pack("{s:s}", "other", caladdress));
        json_object_set_new(jorga, "email", json_null());
    }

    return jorga;
}

/* Convert the ical ORGANIZER/ATTENDEEs in comp to CalendarEvent participants */
static json_t*
participants_from_ical(context_t *ctx __attribute__((unused)),
                       icalcomponent *comp)
{
    struct hash_table attendee_by_uri = HASH_TABLE_INITIALIZER;
    struct hash_table id_by_uri = HASH_TABLE_INITIALIZER;
    icalproperty *prop;
    json_t *participants = json_object();

    /* Collect all attendees in a map to lookup delegates and their ids. */
    construct_hash_table(&attendee_by_uri, 32, 0);
    construct_hash_table(&id_by_uri, 32, 0);
    for (prop = icalcomponent_get_first_property(comp, ICAL_ATTENDEE_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_ATTENDEE_PROPERTY)) {

        /* Map normalized URI to ATTENDEE */
        char *uri = normalized_uri(icalproperty_get_value_as_string(prop));
        hash_insert(uri, prop, &attendee_by_uri);

        /* Map mailto:URI to ID */
        char *id = xstrdupnull(get_icalxparam_value(prop, JMAPICAL_XPARAM_ID));
        if (!id) id = sha1key(uri);
        hash_insert(uri, id, &id_by_uri);
        free(uri);
    }
    if (!hash_numrecords(&attendee_by_uri)) {
        goto done;
    }


    /* Map ATTENDEE to JSCalendar */
    icalproperty *orga = icalcomponent_get_first_property(comp, ICAL_ORGANIZER_PROPERTY);
    for (prop = icalcomponent_get_first_property(comp, ICAL_ATTENDEE_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_ATTENDEE_PROPERTY)) {

        char *uri = normalized_uri(icalproperty_get_value_as_string(prop));
        const char *id = hash_lookup(uri, &id_by_uri);
        json_t *p = participant_from_ical(prop, &attendee_by_uri, &id_by_uri, orga);
        json_object_set_new(participants, id, p);
        free(uri);
    }

    if (orga) {
        const char *caladdress = icalproperty_get_value_as_string(orga);
        char *uri = normalized_uri(caladdress);
        if (!hash_lookup(uri, &attendee_by_uri)) {
            /* Add a default participant for the organizer. */
            char *id = xstrdupnull(get_icalxparam_value(orga, JMAPICAL_XPARAM_ID));
            if (!id) id = sha1key(uri);
            json_t *jorga = participant_from_icalorganizer(orga);
            json_object_set_new(participants, id, jorga);
            free(id);
        }
        free(uri);
    }

done:
    if (!json_object_size(participants)) {
        json_decref(participants);
        participants = json_null();
    }
    free_hash_table(&attendee_by_uri, NULL);
    free_hash_table(&id_by_uri, free);
    return participants;
}

static json_t*
link_from_ical(context_t *ctx __attribute__((unused)), icalproperty *prop)
{
    /* href */
    const char *href = NULL;
    if (icalproperty_isa(prop) == ICAL_ATTACH_PROPERTY) {
        icalattach *attach = icalproperty_get_attach(prop);
        /* Ignore ATTACH properties with value BINARY. */
        if (!attach || !icalattach_get_is_url(attach)) {
            return NULL;
        }
        href = icalattach_get_url(attach);
    }
    else if (icalproperty_isa(prop) == ICAL_URL_PROPERTY) {
        href = icalproperty_get_value_as_string(prop);
    }
    if (!href || *href == '\0') return NULL;

    json_t *link = json_pack("{s:s}", "href", href);
    icalparameter *param = NULL;
    const char *s;

    /* cid */
    if ((s = get_icalxparam_value(prop, JMAPICAL_XPARAM_CID))) {
        json_object_set_new(link, "cid", json_string(s));
    }

    /* type */
    param = icalproperty_get_first_parameter(prop, ICAL_FMTTYPE_PARAMETER);
    if (param && ((s = icalparameter_get_fmttype(param)))) {
        json_object_set_new(link, "type", json_string(s));
    }

    /* title - reuse the same x-param as Apple does for their locations  */
    if ((s = get_icalxparam_value(prop, JMAPICAL_XPARAM_TITLE))) {
        json_object_set_new(link, "title", json_string(s));
    }

    /* size */
    json_int_t size = -1;
    param = icalproperty_get_size_parameter(prop);
    if (param) {
        if ((s = icalparameter_get_size(param))) {
            char *ptr;
            size = strtol(s, &ptr, 10);
            json_object_set_new(link, "size",
                    ptr && *ptr == '\0' ? json_integer(size) : json_null());
        }
    }

    /* rel */
    const char *rel = get_icalxparam_value(prop, JMAPICAL_XPARAM_REL);
    if (!rel)
        rel = icalproperty_isa(prop) == ICAL_URL_PROPERTY ? "describedby" :
                                                            "enclosure";
    json_object_set_new(link, "rel", json_string(rel));

    /* display */
    if ((s = get_icalxparam_value(prop, JMAPICAL_XPARAM_DISPLAY))) {
        json_object_set_new(link, "display", json_string(s));
    }


    return link;
}

static json_t*
links_from_ical(context_t *ctx, icalcomponent *comp)
{
    icalproperty* prop;
    json_t *ret = json_pack("{}");

    /* Read iCalendar ATTACH properties */
    for (prop = icalcomponent_get_first_property(comp, ICAL_ATTACH_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_ATTACH_PROPERTY)) {

        char *id = xstrdupnull(get_icalxparam_value(prop, JMAPICAL_XPARAM_ID));
        if (!id) id = sha1key(icalproperty_get_value_as_string(prop));
        beginprop_key(ctx, "links", id);
        json_t *link = link_from_ical(ctx, prop);
        if (link) json_object_set_new(ret, id, link);
        endprop(ctx);
        free(id);
    }

    /* Read iCalendar URL property. Should only be one. */
    for (prop = icalcomponent_get_first_property(comp, ICAL_URL_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_URL_PROPERTY)) {

        char *id = xstrdupnull(get_icalxparam_value(prop, JMAPICAL_XPARAM_ID));
        if (!id) id = sha1key(icalproperty_get_value_as_string(prop));
        beginprop_key(ctx, "links", id);
        json_t *link = link_from_ical(ctx, prop);
        if (link) json_object_set_new(ret, id, link);
        endprop(ctx);
        free(id);
    }

    if (!json_object_size(ret)) {
        json_decref(ret);
        ret = json_null();
    }

    return ret;
}

/* Convert the VALARMS in the VEVENT comp to CalendarEvent alerts.
 * Adds any ATTACH properties found in VALARM components to the
 * event 'links' property. */
static json_t*
alerts_from_ical(context_t *ctx, icalcomponent *comp)
{
    json_t* alerts = json_pack("{}");
    icalcomponent* alarm;
    hash_table snoozes;
    ptrarray_t alarms = PTRARRAY_INITIALIZER;

    construct_hash_table(&snoozes, 32, 0);

    /* Split VALARMS into regular alerst and their snoozing VALARMS */
    for (alarm = icalcomponent_get_first_component(comp, ICAL_VALARM_COMPONENT);
         alarm;
         alarm = icalcomponent_get_next_component(comp, ICAL_VALARM_COMPONENT)) {

        icalparameter *param = NULL;
        const char *uid = NULL;

        /* Ignore alarms with NONE action. */
        icalproperty *prop = icalcomponent_get_first_property(alarm, ICAL_ACTION_PROPERTY);
        if (prop) {
            icalvalue *val = icalproperty_get_value(prop);
            if (val && !strcasecmp(icalvalue_as_ical_string(val), "NONE")) {
                continue;
            }
        }

        /* Check for RELATED-TO property... */
        prop = icalcomponent_get_first_property(alarm, ICAL_RELATEDTO_PROPERTY);
        if (!prop) {
            ptrarray_push(&alarms, alarm);
            continue;
        }
        /* .. that has a UID value... */
        uid = icalproperty_get_value_as_string(prop);
        if (!uid || !strlen(uid)) {
            ptrarray_push(&alarms, alarm);
            continue;
        }
        /* ... and it's RELTYPE is set to SNOOZE */
        param = icalproperty_get_first_parameter(prop, ICAL_RELTYPE_PARAMETER);
        if (!param || strcasecmp(icalparameter_get_xvalue(param), "SNOOZE")) {
            ptrarray_push(&alarms, alarm);
            continue;
        }

        /* Must be a SNOOZE alarm */
        hash_insert(uid, alarm, &snoozes);
    }

    while ((alarm = (icalcomponent*) ptrarray_pop(&alarms))) {
        icalproperty* prop;
        icalparameter *param;

        json_t *alert = json_object();

        /* alert id */
        char *id = (char *) icalcomponent_get_uid(alarm);
        if (!id) {
            id = sha1key(icalcomponent_as_ical_string(alarm));
        } else {
            id = xstrdup(id);
        }
        beginprop_key(ctx, "alerts", id);

        /* Determine TRIGGER and RELATED parameter */
        struct icaltriggertype trigger = {
            icaltime_null_time(), icaldurationtype_null_duration()
        };
        icalparameter_related related = ICAL_RELATED_START;
        prop = icalcomponent_get_first_property(alarm, ICAL_TRIGGER_PROPERTY);
        if (prop) {
            trigger = icalproperty_get_trigger(prop);
            param = icalproperty_get_first_parameter(prop, ICAL_RELATED_PARAMETER);
            if (param) {
                related = icalparameter_get_related(param);
                if (related != ICAL_RELATED_START && related != ICAL_RELATED_END) {
                    endprop(ctx);
                    free(id);
                    continue;
                }
            }
        }

        /* Determine duration between alarm and start/end */
        struct icaldurationtype duration;
        if (!icaldurationtype_is_null_duration(trigger.duration) ||
             icaltime_is_null_time(trigger.time)) {
            duration = trigger.duration;
        } else {
            icaltimetype ttrg, tref;
            icaltimezone *utc = icaltimezone_get_utc_timezone();

            ttrg = icaltime_convert_to_zone(trigger.time, utc);
            if (related == ICAL_RELATED_START) {
                tref = icaltime_convert_to_zone(dtstart_from_ical(comp), utc);
            } else {
                tref = icaltime_convert_to_zone(dtend_from_ical(comp), utc);
            }
            duration = icaltime_subtract(ttrg, tref);
        }

        /*  action */
        const char *action = "display";
        prop = icalcomponent_get_first_property(alarm, ICAL_ACTION_PROPERTY);
        if (prop && icalproperty_get_action(prop) == ICAL_ACTION_EMAIL) {
            action = "email";
        }
        json_object_set_new(alert, "action", json_string(action));

        /* relativeTo */
        const char *relative_to = "before-start";
        if (duration.is_neg) {
            relative_to = related == ICAL_RELATED_START ?
                "before-start" : "before-end";
        } else {
            relative_to = related == ICAL_RELATED_START ?
                "after-start" : "after-end";
        }
        json_object_set_new(alert, "relativeTo", json_string(relative_to));

        /* offset*/
        duration.is_neg = 0;
        char *offset = icaldurationtype_as_ical_string_r(duration);
        json_object_set_new(alert, "offset", json_string(offset));
        json_object_set_new(alerts, id, alert);
        free(offset);

        /* acknowledged */
        if ((prop = icalcomponent_get_acknowledged_property(alarm))) {
            icaltimetype t = icalproperty_get_acknowledged(prop);
            if (icaltime_is_valid_time(t)) {
                char *val = utcdate_from_icaltime_r(t);
                json_object_set_new(alert, "acknowledged", json_string(val));
                free(val);
            }
        }

        /* snoozed */
        icalcomponent *snooze;
        const char *uid;
        if ((uid = icalcomponent_get_uid(alarm)) &&
            (snooze = hash_lookup(uid, &snoozes)) &&
            (prop = icalcomponent_get_first_property(snooze,
                                        ICAL_TRIGGER_PROPERTY))) {
            icaltimetype t = icalproperty_get_trigger(prop).time;
            if (!icaltime_is_null_time(t) && icaltime_is_valid_time(t)) {
                char *val = utcdate_from_icaltime_r(t);
                json_object_set_new(alert, "snoozed", json_string(val));
                free(val);
            }
        }

        free(id);
    }

    if (!json_object_size(alerts)) {
        json_decref(alerts);
        alerts = json_null();
    }

    ptrarray_fini(&alarms);
    free_hash_table(&snoozes, NULL);
    return alerts;
}



/* Convert a VEVENT ical component to CalendarEvent keywords */
static json_t*
keywords_from_ical(context_t *ctx __attribute__((unused)), icalcomponent *comp)
{
    icalproperty* prop;
    json_t *ret = json_object();

    for (prop = icalcomponent_get_first_property(comp, ICAL_CATEGORIES_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_CATEGORIES_PROPERTY)) {
        json_object_set_new(ret, icalproperty_get_categories(prop), json_true());
    }
    if (!json_object_size(ret)) {
        json_decref(ret);
        ret = json_null();
    }

    return ret;
}

/* Convert a VEVENT ical component to CalendarEvent relatedTo */
static json_t*
relatedto_from_ical(context_t *ctx __attribute__((unused)), icalcomponent *comp)
{
    icalproperty* prop;
    json_t *ret = json_pack("{}");

    for (prop = icalcomponent_get_first_property(comp, ICAL_RELATEDTO_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_RELATEDTO_PROPERTY)) {

        const char *uid = icalproperty_get_value_as_string(prop);
        if (!uid || !strlen(uid)) continue;

        icalparameter *param = NULL;
        json_t *relation = json_object();
        for (param = icalproperty_get_first_parameter(prop, ICAL_RELTYPE_PARAMETER);
             param;
             param = icalproperty_get_next_parameter(prop, ICAL_RELTYPE_PARAMETER)) {

            const char *reltype = icalparameter_get_xvalue(param);
            if (reltype && *reltype) {
                char *s = lcase(xstrdup(reltype));
                json_object_set_new(relation, s, json_true());
                free(s);
            }
        }

        if (!json_object_size(relation)) {
            json_decref(relation);
            relation = json_null();
        }

        json_object_set_new(ret, uid, json_pack("{s:o}", "relation", relation));
    }

    if (!json_object_size(ret)) {
        json_decref(ret);
        ret = json_null();
    }

    return ret;
}

static json_t* location_from_ical(context_t *ctx __attribute__((unused)),
                                  icalproperty *prop, json_t *links)
{
    icalparameter *param;
    json_t *loc = json_object();

    /* name */
    const char *name = icalvalue_get_text(icalproperty_get_value(prop));
    json_object_set_new(loc, "name", json_string(name ? name : ""));

    /* rel */
    const char *rel = get_icalxparam_value(prop, JMAPICAL_XPARAM_REL);
    if (!rel) rel = "unknown";
    json_object_set_new(loc, "rel", json_string(rel));

    /* description */
    const char *desc = get_icalxparam_value(prop, JMAPICAL_XPARAM_DESCRIPTION);
    json_object_set_new(loc, "description", desc ? json_string(desc) : json_null());

    /* timeZone */
    const char *tzid = get_icalxparam_value(prop, JMAPICAL_XPARAM_TZID);
    json_object_set_new(loc, "timeZone", tzid ? json_string(tzid) : json_null());

    /* coordinates */
    const char *coord = get_icalxparam_value(prop, JMAPICAL_XPARAM_GEO);
    json_object_set_new(loc, "coordinates", coord ? json_string(coord) : json_null());

    /* linkIds (including altrep) */
    json_t *linkids = json_object();
    for (param = icalproperty_get_first_parameter(prop, ICAL_X_PARAMETER);
         param;
         param = icalproperty_get_next_parameter(prop, ICAL_X_PARAMETER)) {

        if (strcasecmp(icalparameter_get_xname(param), JMAPICAL_XPARAM_LINKID)) {
            continue;
        }
        const char *s = icalparameter_get_xvalue(param);
        if (!s) continue;
        json_object_set_new(linkids, s, json_true());
    }
    const char *altrep = NULL;
    param = icalproperty_get_first_parameter(prop, ICAL_ALTREP_PARAMETER);
    if (param) altrep = icalparameter_get_altrep(param);
    if (altrep) {
        char *tmp = sha1key(altrep);
        json_object_set_new(links, tmp, json_pack("{s:s}", "href", altrep));
        json_object_set_new(linkids, tmp, json_true());
        free(tmp);
    }
    if (!json_object_size(linkids)) {
        json_decref(linkids);
        linkids = json_null();
    }
    json_object_set_new(loc, "linkIds", linkids);

    return loc;
}

static json_t *coordinates_from_ical(icalproperty *prop)
{
    /* Use verbatim coordinate string, rather than the parsed ical value */
    const char *p, *val = icalproperty_get_value_as_string(prop);
    struct buf buf = BUF_INITIALIZER;
    json_t *c;

    p = strchr(val, ';');
    if (!p) return NULL;

    buf_setcstr(&buf, "geo:");
    buf_appendmap(&buf, val, p-val);
    buf_appendcstr(&buf, ",");
    val = p + 1;
    buf_appendcstr(&buf, val);

    c = json_string(buf_cstring(&buf));
    buf_free(&buf);
    return c;
}

static json_t*
locations_from_ical(context_t *ctx, icalcomponent *comp, json_t *links)
{
    icalproperty* prop;
    json_t *loc, *locations = json_pack("{}");
    char *id;

    /* Handle end locations */
    const char *tzidstart = tzid_from_ical(comp, ICAL_DTSTART_PROPERTY);
    const char *tzidend = tzid_from_ical(comp, ICAL_DTEND_PROPERTY);
    if (tzidstart && tzidend && strcmp(tzidstart, tzidend)) {
        prop = icalcomponent_get_first_property(comp, ICAL_DTEND_PROPERTY);
        id = xjmapid_from_ical(prop);
        loc = json_pack("{s:s s:s}", "timeZone", tzidend, "rel", "end");
        json_object_set_new(locations, id, loc);
        free(id);
    }

    /* LOCATION */
    if ((prop = icalcomponent_get_first_property(comp, ICAL_LOCATION_PROPERTY))) {
        id = xjmapid_from_ical(prop);
        beginprop_key(ctx, "locations", id);
        if ((loc = location_from_ical(ctx, prop, links))) {
            json_object_set_new(locations, id, loc);
        }
        endprop(ctx);
        free(id);
    }

    /* GEO */
    if ((prop = icalcomponent_get_first_property(comp, ICAL_GEO_PROPERTY))) {
        json_t *coord = coordinates_from_ical(prop);
        if (coord) {
            loc = json_pack("{s:o}", "coordinates", coord);
            id = xjmapid_from_ical(prop);
            json_object_set_new(locations, id, loc);
            free(id);
        }
    }

    /* Lookup X-property locations */
    for (prop = icalcomponent_get_first_property(comp, ICAL_X_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_X_PROPERTY)) {

        const char *name = icalproperty_get_property_name(prop);

        /* X-APPLE-STRUCTURED-LOCATION */
        /* FIXME Most probably,
         * a X-APPLE-STRUCTURED-LOCATION may occur only once and
         * always comes with a LOCATION. But who knows for sure? */
        if (!strcmp(name, "X-APPLE-STRUCTURED-LOCATION")) {
            const char *title, *uri;
            icalvalue *val;

            val = icalproperty_get_value(prop);
            if (icalvalue_isa(val) != ICAL_URI_VALUE) continue;

            uri = icalvalue_as_ical_string(val);
            if (strncmp(uri, "geo:", 4)) continue;

            loc = json_pack("{s:s}", "coordinates", uri);
            if ((title = get_icalxparam_value(prop, JMAPICAL_XPARAM_TITLE))) {
                json_object_set_new(loc, "name", json_string(title));
            }

            id = xjmapid_from_ical(prop);
            json_object_set_new(locations, id, loc);
            free(id);
            continue;
        }

        if (strcmp(name, JMAPICAL_XPROP_LOCATION)) {
            continue;
        }

        /* X-JMAP-LOCATION */
        id = xjmapid_from_ical(prop);
        beginprop_key(ctx, "locations", id);
        loc = location_from_ical(ctx, prop, links);
        if (loc) json_object_set_new(locations, id, loc);
        free(id);
        endprop(ctx);
    }

    if (!json_object_size(locations)) {
        json_decref(locations);
        locations = json_null();
    }

    return locations;
}

static json_t*
virtuallocations_from_ical(context_t *ctx, icalcomponent *comp)
{
    icalproperty* prop;
    json_t *locations = json_pack("{}");

    for (prop = icalcomponent_get_first_property(comp, ICAL_CONFERENCE_PROPERTY);
         prop;
         prop = icalcomponent_get_next_property(comp, ICAL_CONFERENCE_PROPERTY)) {

        char *id = xjmapid_from_ical(prop);
        beginprop_key(ctx, "locations", id);

        json_t *loc = json_object();

        const char *uri = icalproperty_get_value_as_string(prop);
        if (uri) json_object_set_new(loc, "uri", json_string(uri));

        const char *name = NULL;
        icalparameter *param = icalproperty_get_first_parameter(prop, ICAL_LABEL_PARAMETER);
        if (param) name = icalparameter_get_label(param);
        if (name) json_object_set_new(loc, "name", json_string(name));

        const char *desc = get_icalxparam_value(prop, JMAPICAL_XPARAM_DESCRIPTION);
        if (desc) json_object_set_new(loc, "description", json_string(desc));

        if (uri) json_object_set_new(locations, id, loc);

        endprop(ctx);
        free(id);
    }

    if (!json_object_size(locations)) {
        json_decref(locations);
        locations = json_null();
    }

    return locations;
}

static json_t* duration_from_ical(icalcomponent *comp)
{
    struct icaltimetype dtstart, dtend;
    char *val = NULL;

    dtstart = dtstart_from_ical(comp);
    dtend = dtend_from_ical(comp);

    if (!icaltime_is_null_time(dtend)) {
        time_t tstart, tend;
        struct icaldurationtype dur;

        tstart = icaltime_as_timet_with_zone(dtstart, dtstart.zone);
        tend = icaltime_as_timet_with_zone(dtend, dtend.zone);
        dur = icaldurationtype_from_int((int)(tend - tstart));

        if (!icaldurationtype_is_bad_duration(dur) && !dur.is_neg) {
            val = icaldurationtype_as_ical_string_r(dur);
        }
    }

    json_t *ret = json_string(val ? val : "PT0S");
    if (val) free(val);
    return ret;
}

static json_t*
locale_from_ical(context_t *ctx __attribute__((unused)), icalcomponent *comp)
{
    icalproperty *sum, *dsc;
    icalparameter *param = NULL;
    const char *lang = NULL;

    sum = icalcomponent_get_first_property(comp, ICAL_SUMMARY_PROPERTY);
    dsc = icalcomponent_get_first_property(comp, ICAL_DESCRIPTION_PROPERTY);

    if (sum) {
        param = icalproperty_get_first_parameter(sum, ICAL_LANGUAGE_PARAMETER);
    }
    if (!param && dsc) {
        param = icalproperty_get_first_parameter(dsc, ICAL_LANGUAGE_PARAMETER);
    }
    if (param) {
        lang = icalparameter_get_language(param);
    }

    return lang ? json_string(lang) : json_null();
}

/* Convert the libical VEVENT comp to a CalendarEvent 
 *
 * parent: if not NULL, treat comp as a VEVENT exception
 * props:  if not NULL, only convert properties named as keys
 */
static json_t*
calendarevent_from_ical(context_t *ctx, icalcomponent *comp)
{
    icalproperty* prop;
    json_t *event, *wantprops;
    int is_exc = ctx->master != NULL;

    wantprops = NULL;
    if (ctx->wantprops && wantprop(ctx, "recurrenceOverrides") && !is_exc) {
        /* Fetch all properties if recurrenceOverrides are requested,
         * otherwise we might return incomplete override patches */
        wantprops = ctx->wantprops;
        ctx->wantprops = NULL;
    }

    event = json_pack("{s:s}", "@type", "jsevent");

    /* Always determine the event's start timezone. */
    ctx->tzid_start = tzid_from_ical(comp, ICAL_DTSTART_PROPERTY);

    /* Always determine isAllDay to set start, end and timezone fields. */
    ctx->is_allday = icaltime_is_date(icalcomponent_get_dtstart(comp));
    if (ctx->is_allday && ctx->tzid_start) {
        /* bogus iCalendar data */
        ctx->tzid_start = NULL;
    }

    /* isAllDay */
    if (wantprop(ctx, "isAllDay") && !is_exc) {
        json_object_set_new(event, "isAllDay", json_boolean(ctx->is_allday));
    }

    /* uid */
    const char *uid = icalcomponent_get_uid(comp);
    if (uid && !is_exc) {
        json_object_set_new(event, "uid", json_string(uid));
    }
    if (!ctx->uid) ctx->uid = uid;

    /* relatedTo */
    if (wantprop(ctx, "relatedTo") && !is_exc) {
        json_object_set_new(event, "relatedTo", relatedto_from_ical(ctx, comp));
    }

    /* prodId */
    if (wantprop(ctx, "prodId") && !is_exc) {
        icalcomponent *ical = icalcomponent_get_parent(comp);
        const char *prodid = NULL;
        prop = icalcomponent_get_first_property(ical, ICAL_PRODID_PROPERTY);
        if (prop) prodid = icalproperty_get_prodid(prop);
        json_object_set_new(event, "prodId",
                prodid ? json_string(prodid) : json_null());
    }

    /* created */
    if (wantprop(ctx, "created")) {
        json_t *val = json_null();
        prop = icalcomponent_get_first_property(comp, ICAL_CREATED_PROPERTY);
        if (prop) {
            char *t = utcdate_from_icaltime_r(icalproperty_get_created(prop));
            if (t) {
                val = json_string(t);
                free(t);
            }
        }
        json_object_set_new(event, "created", val);
    }

    /* updated */
    if (wantprop(ctx, "updated")) {
        json_t *val = json_null();
        prop = icalcomponent_get_first_property(comp, ICAL_DTSTAMP_PROPERTY);
        if (prop) {
            char *t = utcdate_from_icaltime_r(icalproperty_get_dtstamp(prop));
            if (t) {
                val = json_string(t);
                free(t);
            }
        }
        json_object_set_new(event, "updated", val);
    }

    /* sequence */
    if (wantprop(ctx, "sequence")) {
        json_object_set_new(event, "sequence",
                json_integer(icalcomponent_get_sequence(comp)));
    }

    /* priority */
    if (wantprop(ctx, "priority")) {
        prop = icalcomponent_get_first_property(comp, ICAL_PRIORITY_PROPERTY);
        if (prop) {
            json_object_set_new(event, "priority",
                    json_integer(icalproperty_get_priority(prop)));
        }
    }

    /* title */
    if (wantprop(ctx, "title")) {
        prop = icalcomponent_get_first_property(comp, ICAL_SUMMARY_PROPERTY);
        if (prop) {
            json_object_set_new(event, "title",
                                json_string(icalproperty_get_summary(prop)));
        } else {
            json_object_set_new(event, "title", json_string(""));
        }
        if (!wantprop(ctx, "title")) {
            json_object_del(event, "title");
        }
    }

    /* description */
    if (wantprop(ctx, "description") || wantprop(ctx, "descriptionContentType")) {
        const char *desc = "";
        prop = icalcomponent_get_first_property(comp, ICAL_DESCRIPTION_PROPERTY);
        if (prop) {
            desc = icalproperty_get_description(prop);
            if (!desc) desc = "";
        }
        if (wantprop(ctx, "description")) {
            json_object_set_new(event, "description", json_string(desc));
        }
        if (wantprop(ctx, "descriptionContentType")) {
            json_object_set_new(event, "descriptionContentType", json_string("text/plain"));
        }
    }

    /* method */
    if (wantprop(ctx, "method")) {
        icalcomponent *ical = icalcomponent_get_parent(comp);
        if (ical) {
            icalproperty_method icalmethod = icalcomponent_get_method(ical);
            if (icalmethod != ICAL_METHOD_NONE) {
                char *method = xstrdupsafe(icalenum_method_to_string(icalmethod));
                lcase(method);
                json_object_set_new(event, "method", json_string(method));
                free(method);
            }
        }
    }

    /* color */
    if (wantprop(ctx, "color")) {
        prop = icalcomponent_get_first_property(comp, ICAL_COLOR_PROPERTY);
        if (prop) {
            json_object_set_new(event, "color",
                    json_string(icalproperty_get_color(prop)));
        }
    }

    /* keywords */
    if (wantprop(ctx, "keywords")) {
        json_object_set_new(event, "keywords", keywords_from_ical(ctx, comp));
    }

    /* links */
    if (wantprop(ctx, "links")) {
        json_object_set_new(event, "links", links_from_ical(ctx, comp));
        if (!wantprop(ctx, "links")) {
            json_object_del(event, "links");
        }
    }

    /* locale */
    if (wantprop(ctx, "locale")) {
        json_object_set_new(event, "locale", locale_from_ical(ctx, comp));
    }

    /* locations */
    if (wantprop(ctx, "locations")) {
        json_t *links = json_object();
        json_object_set_new(event, "locations", locations_from_ical(ctx, comp, links));
        if (json_object_size(links)) {
            if (JNOTNULL(json_object_get(event, "links"))) {
                json_object_update(json_object_get(event, "links"), links);
            } else {
                json_object_set(event, "links", links);
            }
        }
        json_decref(links);
    }

    /* virtualLocations */
    if (wantprop(ctx, "virtualLocations")) {
        json_object_set_new(event, "virtualLocations", virtuallocations_from_ical(ctx, comp));
    }

    /* start */
    if (wantprop(ctx, "start")) {
        struct icaltimetype dt = icalcomponent_get_dtstart(comp);
        char *s = localdate_from_icaltime_r(dt);
        json_object_set_new(event, "start", json_string(s));
        free(s);
    }

    /* timeZone */
    if (wantprop(ctx, "timeZone")) {
        json_object_set_new(event, "timeZone",
                ctx->tzid_start && !ctx->is_allday ?
                json_string(ctx->tzid_start) : json_null());
    }

    /* duration */
    if (wantprop(ctx, "duration")) {
        json_object_set_new(event, "duration", duration_from_ical(comp));
    }

    /* recurrenceRule */
    if (wantprop(ctx, "recurrenceRule") && !is_exc) {
        json_object_set_new(event, "recurrenceRule",
                            recurrence_from_ical(ctx, comp));
    }

    /* status */
    if (wantprop(ctx, "status")) {
        const char *status;
        switch (icalcomponent_get_status(comp)) {
            case ICAL_STATUS_TENTATIVE:
                status = "tentative";
                break;
            case ICAL_STATUS_CONFIRMED:
                status = "confirmed";
                break;
            case ICAL_STATUS_CANCELLED:
                status = "cancelled";
                break;
            default:
                status = NULL;
        }
        if (status) json_object_set_new(event, "status", json_string(status));
    }

    /* freeBusyStatus */
    if (wantprop(ctx, "freeBusyStatus")) {
        const char *fbs = "busy";
        if ((prop = icalcomponent_get_first_property(comp,
                                                     ICAL_TRANSP_PROPERTY))) {
            if (icalproperty_get_transp(prop) == ICAL_TRANSP_TRANSPARENT) {
                fbs = "free";
            }
        }
        json_object_set_new(event, "freeBusyStatus", json_string(fbs));
    }

    /* privacy */
    if (wantprop(ctx, "privacy")) {
        const char *prv = "public";
        if ((prop = icalcomponent_get_first_property(comp, ICAL_CLASS_PROPERTY))) {
            switch (icalproperty_get_class(prop)) {
                case ICAL_CLASS_CONFIDENTIAL:
                    prv = "secret";
                    break;
                case ICAL_CLASS_PRIVATE:
                    prv = "private";
                    break;
                default:
                    prv = "public";
            }
        }
        json_object_set_new(event, "privacy", json_string(prv));
    }

    /* replyTo */
    if (wantprop(ctx, "replyTo") && !is_exc) {
        if ((prop = icalcomponent_get_first_property(comp, ICAL_ORGANIZER_PROPERTY))) {
            json_object_set_new(event, "replyTo",rsvpto_from_ical(prop));
        }
    }

    /* participants */
    if (wantprop(ctx, "participants")) {
        json_object_set_new(event, "participants", participants_from_ical(ctx, comp));
    }

    /* useDefaultAlerts */
    if (wantprop(ctx, "useDefaultAlerts")) {
        const char *v = get_icalxprop_value(comp, JMAPICAL_XPROP_USEDEFALERTS);
        if (v && !strcasecmp(v, "true")) {
            json_object_set_new(event, "useDefaultAlerts", json_true());
        }
    }

    /* alerts */
    if (wantprop(ctx, "alerts")) {
        json_object_set_new(event, "alerts", alerts_from_ical(ctx, comp));
        if (!wantprop(ctx, "alerts")) {
            json_object_del(event, "alerts");
        }
    }

    /* recurrenceOverrides - must be last to generate patches */
    if (wantprop(ctx, "recurrenceOverrides") && !is_exc) {
        json_object_set_new(event, "recurrenceOverrides",
                            overrides_from_ical(ctx, comp, event));
    }

    if (wantprops) {
        /* Remove all properties that weren't requested by the caller. */
        json_t *tmp = json_pack("{}");
        void *iter = json_object_iter(wantprops);
        while (iter)
        {
            const char *key = json_object_iter_key(iter);
            json_object_set(tmp, key, json_object_get(event, key));
            iter = json_object_iter_next(wantprops, iter);
        }
        json_decref(event);
        event = tmp;
    }
    ctx->wantprops = wantprops;

    return event;
}

json_t*
jmapical_tojmap_all(icalcomponent *ical, json_t *props, jmapical_err_t *err)
{
    icalcomponent* comp;

    /* Locate all main VEVENTs. */
    ptrarray_t todo = PTRARRAY_INITIALIZER;
    icalcomponent *firstcomp =
        icalcomponent_get_first_component(ical, ICAL_VEVENT_COMPONENT);
    for (comp = firstcomp;
         comp;
         comp = icalcomponent_get_next_component(ical, ICAL_VEVENT_COMPONENT)) {

        icalproperty *recurid = icalcomponent_get_first_property(comp, ICAL_RECURRENCEID_PROPERTY);
        if (recurid) continue;

        if (icalcomponent_get_uid(comp) == NULL) continue;

        ptrarray_append(&todo, comp);
    }
    /* magic promote to toplevel for the first item */
    if (firstcomp && !ptrarray_size(&todo)) {
        ptrarray_append(&todo, firstcomp);
    }
    else if (!ptrarray_size(&todo)) {
        return json_array();
    }

    // FIXME merge this with tthe jmapical_tojmap fucntion

    /* Convert the VEVENTs to JMAP. */
    json_t *events = json_array();
    while ((comp = ptrarray_pop(&todo))) {
        context_t *ctx = context_new(props, err, JMAPICAL_READ_MODE);
        json_t *obj = calendarevent_from_ical(ctx, comp);
        context_free(ctx);
        if (obj) json_array_append_new(events, obj);
    }

    ptrarray_fini(&todo);
    return events;
}

json_t*
jmapical_tojmap(icalcomponent *ical, json_t *props, jmapical_err_t *err)
{
    json_t *jsevents = jmapical_tojmap_all(ical, props, err);
    json_t *ret = NULL;
    if (json_array_size(jsevents)) {
        ret = json_incref(json_array_get(jsevents, 0));
    }
    json_decref(jsevents);
    return ret;
}

/*
 * Convert to iCalendar from JMAP
 */

/* defined in http_tzdist */
extern void icalcomponent_add_required_timezones(icalcomponent *ical);

/* Remove and deallocate any properties of kind in comp. */
static void remove_icalprop(icalcomponent *comp, icalproperty_kind kind)
{
    icalproperty *prop, *next;

    for (prop = icalcomponent_get_first_property(comp, kind);
         prop;
         prop = next) {

        next = icalcomponent_get_next_property(comp, kind);
        icalcomponent_remove_property(comp, prop);
        icalproperty_free(prop);
    }
}

/* Convert the JMAP local datetime in buf to tm time.
   Return non-zero on success. */
static int localdate_to_tm(const char *buf, struct tm *tm) {
    /* Initialize tm. We don't know about daylight savings time here. */
    memset(tm, 0, sizeof(struct tm));
    tm->tm_isdst = -1;

    /* Parse LocalDate. */
    const char *p = strptime(buf, "%Y-%m-%dT%H:%M:%S", tm);
    if (!p || *p) {
        return 0;
    }
    return 1;
}

/* Convert the JMAP local datetime formatted buf into ical datetime dt
 * using timezone tz. Return non-zero on success. */
static int localdate_to_icaltime(const char *buf,
                                 icaltimetype *dt,
                                 icaltimezone *tz,
                                 int is_allday) {
    struct tm tm;
    int r;
    char *s = NULL;
    icaltimetype tmp;
    int is_utc;
    size_t n;

    r = localdate_to_tm(buf, &tm);
    if (!r) return 0;

    if (is_allday && (tm.tm_sec || tm.tm_min || tm.tm_hour)) {
        return 0;
    }

    is_utc = tz == icaltimezone_get_utc_timezone();

    /* Can't use icaltime_from_timet_with_zone since it tries to convert
     * t from UTC into tz. Let's feed ical a DATETIME string, instead. */
    s = xcalloc(19, sizeof(char));
    n = strftime(s, 18, "%Y%m%dT%H%M%S", &tm);
    if (is_utc) {
        s[n]='Z';
    }
    tmp = icaltime_from_string(s);
    free(s);
    if (icaltime_is_null_time(tmp)) {
        return 0;
    }
    tmp.zone = tz;
    tmp.is_date = is_allday && tz == NULL;
    *dt = tmp;
    return 1;
}

static int utcdate_to_icaltime(const char *src,
                               icaltimetype *dt)
{
    struct buf buf = BUF_INITIALIZER;
    size_t len = strlen(src);
    int r;
    icaltimezone *utc = icaltimezone_get_utc_timezone();

    if (!len || src[len-1] != 'Z') {
        return 0;
    }

    buf_setmap(&buf, src, len-1);
    r = localdate_to_icaltime(buf_cstring(&buf), dt, utc, 0);
    buf_free(&buf);
    return r;
}

/* Add or overwrite the datetime property kind in comp. If tz is not NULL, set
 * the TZID parameter on the property. Also take care to purge conflicting
 * datetime properties such as DTEND and DURATION. */
static icalproperty *dtprop_to_ical(icalcomponent *comp,
                                    icaltimetype dt,
                                    icaltimezone *tz,
                                    int purge,
                                    enum icalproperty_kind kind) {
    icalproperty *prop;

    /* Purge existing property. */
    if (purge) {
        remove_icalprop(comp, kind);
    }

    /* Resolve DTEND/DURATION conflicts. */
    if (kind == ICAL_DTEND_PROPERTY) {
        remove_icalprop(comp, ICAL_DURATION_PROPERTY);
    } else if (kind == ICAL_DURATION_PROPERTY) {
        remove_icalprop(comp, ICAL_DTEND_PROPERTY);
    }

    /* backwards compatible way to set date or datetime */
    icalvalue *val =
        dt.is_date ? icalvalue_new_date(dt) : icalvalue_new_datetime(dt);
    assert(val);  // no way to return errors from here

    /* Set the new property. */
    prop = icalproperty_new(kind);
    icalproperty_set_value(prop, val);
    if (tz && !icaltime_is_utc(dt)) {
        icalparameter *param =
            icalproperty_get_first_parameter(prop, ICAL_TZID_PARAMETER);
        const char *tzid = icaltimezone_get_location(tz);
        if (param) {
            icalparameter_set_tzid(param, tzid);
        } else {
            icalproperty_add_parameter(prop,icalparameter_new_tzid(tzid));
        }
    }
    icalcomponent_add_property(comp, prop);
    return prop;
}

static int location_is_endtimezone(json_t *loc)
{
    const char *rel = json_string_value(json_object_get(loc, "rel"));
    if (!rel) return 0;
    return json_object_get(loc, "timeZone") && !strcmp(rel, "end");
}

/* Update the start and end properties of VEVENT comp, as defined by
 * the JMAP calendarevent event. */
static void
startend_to_ical(context_t *ctx, icalcomponent *comp, json_t *event)
{
    int pe;
    const char *val;

    /* Determine current timezone */
    const char *tzid = tzid_from_ical(comp, ICAL_DTSTART_PROPERTY);
    if (tzid) {
        ctx->tzstart_old = tz_from_tzid(tzid);
    } else {
        ctx->tzstart_old = NULL;
    }

    /* Read new timezone */
    if (!json_is_null(json_object_get(event, "timeZone"))) {
        pe = readprop(ctx, event, "timeZone", !ctx->is_allday, "s", &val);
        if (pe > 0) {
            /* Lookup the new timezone. */
            ctx->tzstart = tz_from_tzid(val);
            if (!ctx->tzstart) {
                invalidprop(ctx, "timeZone");
            }
        } else if (!pe) {
            ctx->tzstart = ctx->tzstart_old;
        }
    } else {
        ctx->tzstart = NULL;
    }
    ctx->tzstart_old = ctx->tzstart;

    /* Determine current end timezone */
    tzid = tzid_from_ical(comp, ICAL_DTEND_PROPERTY);
    if (tzid) {
        ctx->tzend_old = tz_from_tzid(tzid);
    } else {
        ctx->tzend_old = ctx->tzstart_old;
    }

    /* Read new end timezone */
    const char *endzoneid = NULL;
    json_t *locations = json_object_get(event, "locations");
    if (locations && !json_is_null(locations)) {
        json_t *loc;
        const char *id;

        /* Pick the first location with timeZone and rel=end */
        json_object_foreach(json_object_get(event, "locations"), id, loc) {
            json_t *timeZone;

            if (!location_is_endtimezone(loc)) {
                continue;
            }
            endzoneid = id;

            /* Prepare prefix for error reporting */
            beginprop_key(ctx, "locations", id);

            timeZone = json_object_get(loc, "timeZone");
            if (!json_is_null(timeZone)) {
                tzid = json_string_value(json_object_get(loc, "timeZone"));
                if (tzid) {
                    ctx->tzend = tz_from_tzid(tzid);
                } else {
                    invalidprop(ctx, "timeZone");
                }
            } else {
                /* The end timeZone is set to floating time */
                ctx->tzend = NULL;
            }

            /* Make sure that both timezones are either floating time or not */
            if ((ctx->tzstart == NULL) != (ctx->tzend == NULL)) {
                invalidprop(ctx, "timeZone");
            }
            /* allDay requires floating time */
            if (ctx->is_allday && ctx->tzend) {
                invalidprop(ctx, "timeZone");
            }

            endprop(ctx);
            break;
        }
    } else if (json_is_null(locations)) {
        ctx->tzend = NULL;
    } else {
        ctx->tzend = ctx->tzend_old;
    }
    ctx->tzend_old = endzoneid ? ctx->tzend : ctx->tzstart;
    if (!endzoneid) {
        ctx->tzend = ctx->tzend_old;
    }

    /* Read new duration */
    struct icaldurationtype dur = icaldurationtype_null_duration();
    pe = readprop(ctx, event, "duration", 0, "s", &val);
    if (pe > 0) {
        dur = icaldurationtype_from_string(val);
        if (icaldurationtype_is_bad_duration(dur)) {
            invalidprop(ctx, "duration");
        }
    }
    if (ctx->is_allday) {
        if (!icaldurationtype_is_bad_duration(dur) && (dur.hours || dur.minutes || dur.seconds)) {
            invalidprop(ctx, "duration");
        }
    }

    /* Determine current start */
    struct icaltimetype dtstart_old = dtstart_from_ical(comp);

    /* Read new start */
    struct icaltimetype dtstart = dtstart_old;
    pe = readprop(ctx, event, "start", 1, "s", &val);
    if (pe > 0) {
        if (!localdate_to_icaltime(val, &dtstart,
                                   ctx->tzstart, ctx->is_allday)) {
            invalidprop(ctx, "start");
        }
    } else {
        dtstart = dtstart_old;
    }

    /* Bail out for property errors */
    if (have_invalid_props(ctx))
        return;

    /* Either all timezones float or none */
    assert((ctx->tzstart != NULL) == (ctx->tzend != NULL));

    /* Purge and rebuild start and end */
    remove_icalprop(comp, ICAL_DTSTART_PROPERTY);
    remove_icalprop(comp, ICAL_DTEND_PROPERTY);
    remove_icalprop(comp, ICAL_DURATION_PROPERTY);

    dtprop_to_ical(comp, dtstart, ctx->tzstart, 1, ICAL_DTSTART_PROPERTY);
    if (ctx->tzstart != ctx->tzend) {
        /* Add DTEND */
        icaltimetype dtend;
        icalproperty *prop;

        dtend = icaltime_add(dtstart, dur);
        dtend = icaltime_convert_to_zone(dtend, ctx->tzend);
        prop = dtprop_to_ical(comp, dtend, ctx->tzend, 1, ICAL_DTEND_PROPERTY);
        xjmapid_to_ical(prop, endzoneid);
    } else {
        /* Add DURATION */
        icalcomponent_set_duration(comp, dur);
    }
}

static void
participant_roles_to_ical(context_t *ctx,
                          icalproperty *prop,
                          json_t *roles,
                          icalparameter_role ical_role,
                          int is_replyto)
{
    if (!json_object_size(roles)) {
        invalidprop(ctx, "roles");
        return;
    }

    const char *key;
    json_t *jval;
    json_object_foreach(roles, key, jval) {
        if (jval != json_true()) {
            beginprop_key(ctx, "roles", key);
            invalidprop(ctx, NULL);
            endprop(ctx);
        }
    }

    int has_owner = json_object_get(roles, "owner") == json_true();
    int has_chair = json_object_get(roles, "chair") == json_true();
    int has_attendee = json_object_get(roles, "attendee") == json_true();
    size_t xroles_count = json_object_size(roles);

    /* Try to map roles to iCalendar without falling back to X-ROLE */
    if (has_chair && ical_role == ICAL_ROLE_REQPARTICIPANT) {
        /* Can use iCalendar ROLE=CHAIR parameter */
        xroles_count--;
    }
    if (has_owner && is_replyto) {
        /* This is the ORGANIZER or its ATTENDEE, which is implicit "owner" */
        xroles_count--;
    }
    if (has_attendee) {
        /* Default role for ATTENDEE without X-ROLE is "attendee" */
        xroles_count--;
    }
    if (xroles_count == 0) {
        /* No need to set X-ROLE parameters on this ATTENDEE */
        if (has_chair) {
            icalparameter *param = icalparameter_new_role(ICAL_ROLE_CHAIR);
            icalproperty_add_parameter(prop, param);
        }
    }
    else {
        /* Map roles to X-ROLE */
        json_object_foreach(roles, key, jval) {
            /* Try to use standard CHAIR role */
            if (!strcasecmp(key, "CHAIR") && ical_role == ICAL_ROLE_REQPARTICIPANT) {
                icalparameter *param = icalparameter_new_role(ICAL_ROLE_CHAIR);
                icalproperty_add_parameter(prop, param);
            } else {
                set_icalxparam(prop, JMAPICAL_XPARAM_ROLE, key, 0);
            }
        }
    }
}

static int is_valid_rsvpmethod(const char *s)
{
    if (!s) return 0;
    size_t i;
    for (i = 0; s[i]; i++) {
        if (!isascii(s[i]) || !isalpha(s[i])) {
            return 0;
        }
    }
    return i > 0;
}

static int
participant_equals(json_t *jpart1, json_t *jpart2)
{
    /* Special-case sendTo URI values */
    json_t *jsendTo1 = json_object_get(jpart1, "sendTo");
    json_t *jsendTo2 = json_object_get(jpart2, "sendTo");
    if (jsendTo1 == NULL || jsendTo1 == json_null()) {
        json_t *jemail = json_object_get(jpart1, "email");
        if (json_is_string(jemail)) {
            char *tmp = strconcat("mailto:", json_string_value(jemail), NULL);
            json_object_set_new(jpart1, "sendTo", json_pack("{s:s}", "imip", tmp));
            free(tmp);
        }
        jsendTo1 = json_object_get(jpart1, "sendTo");
    }
    if (jsendTo2 == NULL || jsendTo2 == json_null()) {
        json_t *jemail = json_object_get(jpart2, "email");
        if (json_is_string(jemail)) {
            char *tmp = strconcat("mailto:", json_string_value(jemail), NULL);
            json_object_set_new(jpart2, "sendTo", json_pack("{s:s}", "imip", tmp));
            free(tmp);
        }
        jsendTo2 = json_object_get(jpart2, "sendTo");
    }
    if (json_object_size(jsendTo1) != json_object_size(jsendTo2)) return 0;
    if (JNOTNULL(jsendTo1)) {
        json_t *juri1;
        const char *method;
        json_object_foreach(jsendTo1, method, juri1) {
            json_t *juri2 = json_object_get(jsendTo2, method);
            if (!juri2) return 0;
            const char *uri1 = json_string_value(juri1);
            const char *uri2 = json_string_value(juri2);
            if (!uri1 || !uri2 || !match_uri(uri1, uri2)) return 0;
        }
    }

    json_t *jval1 = json_copy(jpart1);
    json_t *jval2 = json_copy(jpart2);
    json_object_del(jval1, "sendTo");
    json_object_del(jval2, "sendTo");

    /* Remove default values */
    if (!strcmpsafe(json_string_value(json_object_get(jval1, "name")), ""))
        json_object_del(jval1, "name");
    if (!strcmpsafe(json_string_value(json_object_get(jval2, "name")), ""))
        json_object_del(jval2, "name");

    if (!strcmpsafe(json_string_value(json_object_get(jval1, "participationStatus")), "needs-action"))
        json_object_del(jval1, "participationStatus");
    if (!strcmpsafe(json_string_value(json_object_get(jval2, "participationStatus")), "needs-action"))
        json_object_del(jval2, "participationStatus");

    if (!strcmpsafe(json_string_value(json_object_get(jval1, "attendance")), "required"))
        json_object_del(jval1, "attendance");
    if (!strcmpsafe(json_string_value(json_object_get(jval2, "attendance")), "required"))
        json_object_del(jval2, "attendance");

    if (!json_boolean_value(json_object_get(jval1, "expectReply")))
        json_object_del(jval1, "expectReply");
    if (!json_boolean_value(json_object_get(jval2, "expectReply")))
        json_object_del(jval2, "expectReply");

    if (json_integer_value(json_object_get(jval1, "scheduleSequence")) == 0)
        json_object_del(jval1, "scheduleSequence");
    if (json_integer_value(json_object_get(jval2, "scheduleSequence")) == 0)
        json_object_del(jval2, "scheduleSequence");

    /* Unify JSON null to NULL */
    json_t *jprop;
    const char *key;
    void *tmp;
    json_object_foreach_safe(jval1, tmp, key, jprop) {
        if (json_is_null(jprop)) json_object_del(jval1, key);
    }
    json_object_foreach_safe(jval2, tmp, key, jprop) {
        if (json_is_null(jprop)) json_object_del(jval2, key);
    }

    int is_equal = json_equal(jval1, jval2);
    json_decref(jval1);
    json_decref(jval2);
    return is_equal;
}



static void
participant_to_ical(context_t *ctx,
                    icalcomponent *comp,
                    const char *id,
                    json_t *jpart,
                    json_t *participants,
                    json_t *links,
                    const char *orga_uri,
                    hash_table *caladdress_by_participant_id)
{
    const char *caladdress = hash_lookup(id, caladdress_by_participant_id);
    icalproperty *prop = icalproperty_new_attendee(caladdress);
    set_icalxparam(prop, JMAPICAL_XPARAM_ID, id, 1);

    icalproperty *orga = icalcomponent_get_first_property(comp, ICAL_ORGANIZER_PROPERTY);
    int is_orga = match_uri(caladdress, orga_uri);
    if (is_orga) set_icalxparam(orga, JMAPICAL_XPARAM_ID, id, 1);

    /* name */
    json_t *jname = json_object_get(jpart, "name");
    if (json_is_string(jname)) {
        const char *name = json_string_value(jname);
        icalproperty_add_parameter(prop, icalparameter_new_cn(name));
        if (is_orga) {
            icalproperty_add_parameter(orga, icalparameter_new_cn(name));
        }
    }
    else if (JNOTNULL(jname)) {
        invalidprop(ctx, "name");
    }

    /* sendTo */
    json_t *sendTo = json_object_get(jpart, "sendTo");
    if (json_object_size(sendTo)) {
        beginprop(ctx, "sendTo");
        struct buf buf = BUF_INITIALIZER;

        /* Only set RSVP URI x-params if not trivial */
        int set_rsvp_uris = 0;
        if (json_object_size(sendTo) > 1) {
            set_rsvp_uris = 1;
        }
        else {
            const char *method = json_object_iter_key(json_object_iter(sendTo));
            set_rsvp_uris = strcmp(method, "imip") && strcmp(method, "other");
        }

        const char *key;
        json_t *jval;
        /* Process RSVP URIs */
        json_object_foreach(sendTo, key, jval) {
            if (!is_valid_rsvpmethod(key) || !json_is_string(jval)) {
                invalidprop(ctx, key);
                continue;
            }
            if (!set_rsvp_uris) continue;

            buf_setcstr(&buf, key);
            buf_putc(&buf, ':');
            buf_appendcstr(&buf, json_string_value(jval));
            set_icalxparam(prop, JMAPICAL_XPARAM_RSVP_URI, buf_cstring(&buf), 0);
        }

        buf_free(&buf);
        endprop(ctx);
    }
    else if (JNOTNULL(sendTo)) {
        invalidprop(ctx, "sendTo");
    }

    /* email */
    json_t *jemail = json_object_get(jpart, "email");
    if (json_is_string(jemail)) {
        const char *uri = icalproperty_get_value_as_string(prop);
        const char *email = json_string_value(jemail);
        if (!match_uri(uri, email)) {
            icalproperty_add_parameter(prop, icalparameter_new_email(email));
            if (is_orga) {
                icalproperty_add_parameter(orga, icalparameter_new_email(email));
            }
        }
    }
    else if (JNOTNULL(jemail)) {
        invalidprop(ctx, "email");
    }

    /* kind */
    json_t *kind = json_object_get(jpart, "kind");
    if (json_is_string(kind)) {
        icalparameter *param = NULL;
        char *tmp = ucase(xstrdup(json_string_value(kind)));
        icalparameter_cutype cu;
        if (!strcmp(tmp, "LOCATION"))
            cu = ICAL_CUTYPE_ROOM;
        else
            cu = icalparameter_string_to_enum(tmp);
        switch (cu) {
            case ICAL_CUTYPE_INDIVIDUAL:
            case ICAL_CUTYPE_GROUP:
            case ICAL_CUTYPE_RESOURCE:
            case ICAL_CUTYPE_ROOM:
                param = icalparameter_new_cutype(cu);
                icalproperty_add_parameter(prop, param);
                break;
            default:
                /* ignore */ ;
        }
        free(tmp);
    }
    else if (JNOTNULL(kind)) {
        invalidprop(ctx, "kind");
    }

    /* attendance */
    icalparameter_role ical_role = ICAL_ROLE_REQPARTICIPANT;
    json_t *attendance = json_object_get(jpart, "attendance");
    if (json_is_string(attendance)) {
        const char *s = json_string_value(attendance);
        if (!strcasecmp(s, "required")) {
            ical_role = ICAL_ROLE_REQPARTICIPANT;
        }
        else if (!strcasecmp(s, "optional")) {
            ical_role = ICAL_ROLE_OPTPARTICIPANT;
        }
        else if (!strcasecmp(s, "none")) {
            ical_role = ICAL_ROLE_NONPARTICIPANT;
        }
        if (ical_role != ICAL_ROLE_REQPARTICIPANT) {
            icalproperty_add_parameter(prop, icalparameter_new_role(ical_role));
        }
    }
    else if (JNOTNULL(attendance)) {
        invalidprop(ctx, "attendance");
    }

    /* roles */
    json_t *roles = json_object_get(jpart, "roles");
    if (json_object_size(roles)) {
        participant_roles_to_ical(ctx, prop, roles, ical_role, is_orga);
    }
    else if (roles) {
        invalidprop(ctx, "roles");
    }

    /* locationId */
    json_t *locationId = json_object_get(jpart, "locationId");
    if (json_is_string(locationId)) {
        const char *s = json_string_value(locationId);
        set_icalxparam(prop, JMAPICAL_XPARAM_LOCATIONID, s, 1);
    }
    else if (JNOTNULL(locationId)) {
        invalidprop(ctx, "locationId");
    }

    /* participationStatus */
    icalparameter_partstat ps = ICAL_PARTSTAT_NONE;
    json_t *participationStatus = json_object_get(jpart, "participationStatus");
    if (json_is_string(participationStatus)) {
        char *tmp = ucase(xstrdup(json_string_value(participationStatus)));
        ps = icalparameter_string_to_enum(tmp);
        switch (ps) {
            case ICAL_PARTSTAT_NEEDSACTION:
            case ICAL_PARTSTAT_ACCEPTED:
            case ICAL_PARTSTAT_DECLINED:
            case ICAL_PARTSTAT_TENTATIVE:
                break;
            default:
                invalidprop(ctx, "participationStatus");
                ps = ICAL_PARTSTAT_NONE;
        }
        free(tmp);
    }
    else if (JNOTNULL(participationStatus)) {
        invalidprop(ctx, "participationStatus");
    }
    if (ps != ICAL_PARTSTAT_NONE) {
        icalproperty_add_parameter(prop, icalparameter_new_partstat(ps));
    }

    /* expectReply */
    json_t *expectReply = json_object_get(jpart, "expectReply");
    if (json_is_boolean(expectReply)) {
        icalparameter *param = NULL;
        if (expectReply == json_true()) {
            param = icalparameter_new_rsvp(ICAL_RSVP_TRUE);
            if (ps == ICAL_PARTSTAT_NONE) {
                icalproperty_add_parameter(prop,
                        icalparameter_new_partstat(ICAL_PARTSTAT_NEEDSACTION));
            }
        }
        else {
            param = icalparameter_new_rsvp(ICAL_RSVP_FALSE);
        }
        icalproperty_add_parameter(prop, param);
    }
    else if (JNOTNULL(expectReply)) {
        invalidprop(ctx, "expectReply");
    }

    /* delegatedTo */
    json_t *delegatedTo = json_object_get(jpart, "delegatedTo");
    if (json_object_size(delegatedTo)) {
        const char *id;
        json_t *jval;
        json_object_foreach(delegatedTo, id, jval) {
            json_t *delegatee = json_object_get(participants, id);
            if (is_valid_jmapid(id) && delegatee && jval == json_true()) {
                const char *uri = hash_lookup(id, caladdress_by_participant_id);
                if (uri) {
                    icalproperty_add_parameter(prop, icalparameter_new_delegatedto(uri));
                }
            }
            else {
                beginprop_key(ctx, "delegatedTo", id);
                invalidprop(ctx, NULL);
                endprop(ctx);
            }
        }
    }
    else if (JNOTNULL(delegatedTo)) {
        invalidprop(ctx, "delegatedTo");
    }

    /* delegatedFrom */
    json_t *delegatedFrom = json_object_get(jpart, "delegatedFrom");
    if (json_object_size(delegatedFrom)) {
        const char *id;
        json_t *jval;
        json_object_foreach(delegatedFrom, id, jval) {
            json_t *delegator = json_object_get(participants, id);
            if (is_valid_jmapid(id) && delegator && jval == json_true()) {
                const char *uri = hash_lookup(id, caladdress_by_participant_id);
                if (uri) {
                    icalproperty_add_parameter(prop, icalparameter_new_delegatedfrom(uri));
                }
            }
            else {
                beginprop_key(ctx, "delegatedFrom", id);
                invalidprop(ctx, NULL);
                endprop(ctx);
            }
        }
    }
    else if (JNOTNULL(delegatedFrom)) {
        invalidprop(ctx, "delegatedFrom");
    }

    /* memberOf */
    json_t *memberOf = json_object_get(jpart, "memberOf");
    if (json_object_size(memberOf)) {
        const char *id;
        json_t *jval;
        json_object_foreach(memberOf, id, jval) {
            json_t *group = json_object_get(participants, id);
            if (is_valid_jmapid(id) && group && jval == json_true()) {
                const char *uri = hash_lookup(id, caladdress_by_participant_id);
                if (uri) {
                    icalproperty_add_parameter(prop, icalparameter_new_member(uri));
                }
            }
            else {
                beginprop_key(ctx, "memberOf", id);
                invalidprop(ctx, NULL);
                endprop(ctx);
            }
        }
    }
    else if (JNOTNULL(memberOf)) {
        invalidprop(ctx, "memberOf");
    }

    /* linkIds */
    json_t *linkIds = json_object_get(jpart, "linkIds");
    if (json_object_size(linkIds)) {
        const char *id;
        json_t *jval;
        json_object_foreach(linkIds, id, jval) {
            if (!is_valid_jmapid(id) || !json_object_get(links, id) || jval != json_true()) {
                beginprop_key(ctx, "linkIds", id);
                invalidprop(ctx, NULL);
                endprop(ctx);
                continue;
            }
            set_icalxparam(prop, JMAPICAL_XPARAM_LINKID, id, 0);
        }
    }
    else if (JNOTNULL(linkIds)) {
        invalidprop(ctx, "linkIds");
    }

    /* scheduleSequence */
    json_t *scheduleSequence = json_object_get(jpart, "scheduleSequence");
    if (json_is_integer(scheduleSequence) && json_integer_value(scheduleSequence) >= 0) {
        struct buf buf = BUF_INITIALIZER;
        buf_printf(&buf, "%lld", json_integer_value(scheduleSequence));
        set_icalxparam(prop, JMAPICAL_XPARAM_SEQUENCE, buf_cstring(&buf), 0);
        buf_free(&buf);
    }
    else if (JNOTNULL(scheduleSequence)) {
        invalidprop(ctx, "scheduleSequence");
    }

    /* scheduleUpdated */
    json_t *scheduleUpdated = json_object_get(jpart, "scheduleUpdated");
    if (json_is_string(scheduleUpdated)) {
        const char *s = json_string_value(scheduleUpdated);
        icaltimetype dtstamp;
        if (utcdate_to_icaltime(s, &dtstamp)) {
            char *tmp = icaltime_as_ical_string_r(dtstamp);
            set_icalxparam(prop, JMAPICAL_XPARAM_DTSTAMP, tmp, 0);
            free(tmp);
        }
        else {
            invalidprop(ctx, "scheduleSequence");
        }
    }
    else if (JNOTNULL(scheduleUpdated)) {
        invalidprop(ctx, "scheduleSequence");
    }

    if (is_orga) {
        /* We might get away by not creating an ATTENDEE, if the
         * participant is owner of the event and all its JSCalendar
         * properties can be mapped to the ORGANIZER property. */
        json_t *jorga = participant_from_icalorganizer(orga);
        if (participant_equals(jorga, jpart)) {
            icalproperty_free(prop);
            prop = NULL;
        }
        json_decref(jorga);
        if (!prop) return;
    }

    icalcomponent_add_property(comp, prop);
}

/* Create or update the ORGANIZER and ATTENDEEs in the VEVENT component comp as
 * defined by the participants and replyTo property. */
static void
participants_to_ical(context_t *ctx, icalcomponent *comp, json_t *event)
{
    /* Purge existing ATTENDEEs and ORGANIZER */
    remove_icalprop(comp, ICAL_ATTENDEE_PROPERTY);
    remove_icalprop(comp, ICAL_ORGANIZER_PROPERTY);

    json_t *jval = NULL;
    const char *key = NULL;

    /* If participants are set, replyTo must be set */
    json_t *replyTo = json_object_get(event, "replyTo");
    if (JNOTNULL(replyTo) && !json_object_size(replyTo)) {
        invalidprop(ctx, "replyTo");
    }
    json_t *participants = json_object_get(event, "participants");
    if (JNOTNULL(participants) && !json_object_size(participants)) {
        invalidprop(ctx, "participants");
    }
    if (JNOTNULL(replyTo) != JNOTNULL(participants)) {
        invalidprop(ctx, "replyTo");
        invalidprop(ctx, "participants");
        return;
    }
    else if (!JNOTNULL(replyTo)) return;

    /* OK, there's both replyTo and participants set. */

    /* Parse replyTo */
    beginprop(ctx, "replyTo");
    json_object_foreach(replyTo, key, jval) {
        if (!is_valid_rsvpmethod(key) || !json_is_string(jval)) {
            invalidprop(ctx, key);
            continue;
        }
    }
    endprop(ctx);

    /* Map participant ids to their iCalendar CALADDRESS */
    hash_table caladdress_by_participant_id = HASH_TABLE_INITIALIZER;
    construct_hash_table(&caladdress_by_participant_id, json_object_size(participants)+1, 0);
    json_object_foreach(participants, key, jval) {
        if (!is_valid_jmapid(key)) continue;
        char *caladdress = NULL;
        json_t *sendTo = json_object_get(jval, "sendTo");
        if (json_object_get(sendTo, "imip")) {
            caladdress = xstrdup(json_string_value(json_object_get(sendTo, "imip")));
        }
        else if (json_object_get(sendTo, "other")) {
            caladdress = xstrdup(json_string_value(json_object_get(sendTo, "imip")));
        }
        else if (json_object_size(sendTo)) {
            const char *anymethod = json_object_iter_key(json_object_iter(sendTo));
            caladdress = xstrdup(json_string_value(json_object_get(sendTo, anymethod)));
        }
        else if (json_object_get(jval, "email")) {
            caladdress = mailaddr_to_uri(json_string_value(json_object_get(jval, "email")));
        }
        if (!caladdress) continue; /* reported later as error */
        hash_insert(key, caladdress, &caladdress_by_participant_id);
    }

    /* Pick the ORGANIZER URI */
    const char *orga_method = NULL;
    if (json_object_get(replyTo, "imip")) {
        orga_method = "imip";
    }
    else if (json_object_get(replyTo, "other")) {
        orga_method = "other";
    }
    else {
        orga_method = json_object_iter_key(json_object_iter(replyTo));
    }
    const char *orga_uri = json_string_value(json_object_get(replyTo, orga_method));

    /* Create the ORGANIZER property */
    icalproperty *orga = icalproperty_new_organizer(orga_uri);
    /* Keep track of the RSVP URIs and their method */
    if (json_object_size(replyTo) > 1 || (strcmp(orga_method, "imip") && strcmp(orga_method, "other"))) {
        struct buf buf = BUF_INITIALIZER;
        json_object_foreach(replyTo, key, jval) {
            buf_setcstr(&buf, key);
            buf_putc(&buf, ':');
            buf_appendcstr(&buf, json_string_value(jval));
            set_icalxparam(orga, JMAPICAL_XPARAM_RSVP_URI, buf_cstring(&buf), 0);
        }
        buf_free(&buf);
    }
    icalcomponent_add_property(comp, orga);


    /* Process participants */
    json_t *links = json_object_get(event, "links");
    json_object_foreach(participants, key, jval) {
        beginprop_key(ctx, "participants", key);
        if (!is_valid_jmapid(key)) {
            invalidprop(ctx, NULL);
            endprop(ctx);
            continue;
        }

        const char *caladdress = hash_lookup(key, &caladdress_by_participant_id);
        if (!caladdress) {
            invalidprop(ctx, "sendTo");
            invalidprop(ctx, "email");
            endprop(ctx);
            continue;
        }

        /* Map participant to iCalendar */
        participant_to_ical(ctx, comp, key, jval, participants, links,
                            orga_uri, &caladdress_by_participant_id);
        endprop(ctx);
    }

    free_hash_table(&caladdress_by_participant_id, free);
}

static int is_valid_regrel(const char *rel)
{
    // RFC 8288, section 3.3, reg-rel-type:
    const char *p = rel;
    while ((('a' <= *p) && (*p <= 'z')) ||
           (('0' <= *p) && (*p <= '9')) ||
           ((*p == '.') && p > rel) ||
           ((*p == '-') && p > rel)) {
        p++;
    }
    return *p == '\0' && p > rel;
}

static void
links_to_ical(context_t *ctx, icalcomponent *comp, json_t *links, const char *propname)
{
    icalproperty *prop;
    struct buf buf = BUF_INITIALIZER;

    /* Purge existing attachments */
    remove_icalprop(comp, ICAL_ATTACH_PROPERTY);
    remove_icalprop(comp, ICAL_URL_PROPERTY);

    const char *id;
    json_t *link;
    json_object_foreach(links, id, link) {
        int pe;
        const char *href = NULL;
        const char *type = NULL;
        const char *title = NULL;
        const char *rel = NULL;
        const char *cid = NULL;
        const char *display = NULL;
        json_int_t size = -1;

        beginprop_key(ctx, propname, id);

        if (!is_valid_jmapid(id)) {
            invalidprop(ctx, NULL);
            endprop(ctx);
            continue;
        }

        pe = readprop(ctx, link, "href", 1, "s", &href);
        if (pe > 0) {
            if (!strlen(href)) {
                invalidprop(ctx, "href");
                href = NULL;
            }
        }
        if (JNOTNULL(json_object_get(link, "type"))) {
            readprop(ctx, link, "type", 0, "s", &type);
        }
        if (JNOTNULL(json_object_get(link, "title"))) {
            readprop(ctx, link, "title", 0, "s", &title);
        }
        if (JNOTNULL(json_object_get(link, "cid"))) {
            readprop(ctx, link, "cid", 0, "s", &cid);
        }
        if (JNOTNULL(json_object_get(link, "display"))) {
            readprop(ctx, link, "display", 0, "s", &display);
        }
        if (JNOTNULL(json_object_get(link, "size"))) {
            pe = readprop(ctx, link, "size", 0, "I", &size);
            if (pe > 0 && size < 0) {
                invalidprop(ctx, "size");
            }
        }
        readprop(ctx, link, "rel", 0, "s", &rel);
        if (rel && !is_valid_regrel(rel)) {
            invalidprop(ctx, "rel");
        }

        if (href && !have_invalid_props(ctx)) {

            /* Build iCalendar property */
            if (!strcmpsafe(rel, "describedby") &&
                !icalcomponent_get_first_property(comp, ICAL_URL_PROPERTY) &&
                json_object_size(link) == 2) {

                prop = icalproperty_new(ICAL_URL_PROPERTY);
                icalproperty_set_value(prop, icalvalue_new_uri(href));
            }
            else {
                icalattach *icalatt = icalattach_new_from_url(href);
                prop = icalproperty_new_attach(icalatt);
                icalattach_unref(icalatt);
            }

            /* type */
            if (type) {
                icalproperty_add_parameter(prop,
                        icalparameter_new_fmttype(type));
            }

            /* title */
            if (title) {
                set_icalxparam(prop, JMAPICAL_XPARAM_TITLE, title, 1);
            }

            /* cid */
            if (cid) set_icalxparam(prop, JMAPICAL_XPARAM_CID, cid, 1);

            /* size */
            if (size >= 0) {
                buf_printf(&buf, "%"JSON_INTEGER_FORMAT, size);
                icalproperty_add_parameter(prop,
                        icalparameter_new_size(buf_cstring(&buf)));
                buf_reset(&buf);
            }

            /* rel */
            if (rel && strcmp(rel, "enclosure"))
                set_icalxparam(prop, JMAPICAL_XPARAM_REL, rel, 1);

            /* Set custom id */
            set_icalxparam(prop, JMAPICAL_XPARAM_ID, id, 1);

            /* display */
            if (display) set_icalxparam(prop, JMAPICAL_XPARAM_DISPLAY, display, 1);

            /* Add ATTACH property. */
            icalcomponent_add_property(comp, prop);
        }
        endprop(ctx);
        buf_free(&buf);
    }
}

static void
description_to_ical(context_t *ctx __attribute__((unused)), icalcomponent *comp,
                    const char *desc, const char *content_type)
{
    remove_icalprop(comp, ICAL_DESCRIPTION_PROPERTY);

    /* FIXME
     * We'd like to support HTML descriptions, but with iCalendar being
     * our storage format there really isn't a good way to deal with
     * that. We can't rely on iCalendar clients correctly handling the
     * ALTREP parameters on DESCRIPTION, and we don't want to make the
     * CalDAV PUT code deal with comparing old vs new descriptions to
     * try figuring out what the client did.
     * This should become more sane to handle if we start using
     * JSCalendar for storage.
     */
    if (content_type && strcasecmp(content_type, "TEXT/PLAIN")) {
        invalidprop(ctx, "descriptionContentType");
    }

    icalcomponent_set_description(comp, desc);
}

/* Create or update the VALARMs in the VEVENT component comp as defined by the
 * JMAP alerts. */
static void
alerts_to_ical(context_t *ctx, icalcomponent *comp, json_t *alerts)
{
    icalcomponent *alarm, *next;

    /* Purge all VALARMs. */
    for (alarm = icalcomponent_get_first_component(comp, ICAL_VALARM_COMPONENT);
         alarm;
         alarm = next) {
        next = icalcomponent_get_next_component(comp, ICAL_VALARM_COMPONENT);
        icalcomponent_remove_component(comp, alarm);
        icalcomponent_free(alarm);
    }

    if (!JNOTNULL(alerts)) {
        return;
    }

    int pe;
    const char *id;
    json_t *alert;
    json_object_foreach(alerts, id, alert) {
        const char *s;
        icalproperty *prop;
        icalparameter *param;

        beginprop_key(ctx, "alerts", id);

        if (!is_valid_jmapid(id)) {
            invalidprop(ctx, NULL);
            endprop(ctx);
            continue;
        }

        alarm = icalcomponent_new_valarm();
        icalcomponent_set_uid(alarm, id);

        /* offset */
        struct icaltriggertype trigger = {
            icaltime_null_time(), icaldurationtype_null_duration()
        };
        pe = readprop(ctx, alert, "offset", 1, "s", &s);
        if (pe > 0) {
            trigger.duration = icaldurationtype_from_string(s);
            if (icaldurationtype_is_bad_duration(trigger.duration)) {
                invalidprop(ctx, "offset");
            }
        }

        /* relativeTo */
        icalparameter_related rel = ICAL_RELATED_START;
        trigger.duration.is_neg = 1;
        pe = readprop(ctx, alert, "relativeTo", 0, "s", &s);
        if (pe > 0) {
            if (!strcmp(s, "before-start")) {
                rel = ICAL_RELATED_START;
            } else if (!strcmp(s, "after-start")) {
                rel = ICAL_RELATED_START;
                trigger.duration.is_neg = 0;
            } else if (!strcmp(s, "before-end")) {
                rel = ICAL_RELATED_END;
            } else if (!strcmp(s, "after-end")) {
                rel = ICAL_RELATED_END;
                trigger.duration.is_neg = 0;
            } else {
                invalidprop(ctx, "relativeTo");
            }
        }

        /* Add TRIGGER */
        prop = icalproperty_new_trigger(trigger);
        param = icalparameter_new_related(rel);
        icalproperty_add_parameter(prop, param);
        icalcomponent_add_property(alarm, prop);

        /* snoozed */
        pe = readprop(ctx, alert, "snoozed", 0, "s", &s);
        if (pe > 0) {
            struct icaltriggertype snooze_trigger = {
                icaltime_null_time(), icaldurationtype_null_duration()
            };
            if (utcdate_to_icaltime(s, &snooze_trigger.time)) {
                icalcomponent *snooze = icalcomponent_new_valarm();

                /* Add RELATED-TO */
                remove_icalprop(snooze, ICAL_UID_PROPERTY);
                prop = icalproperty_new_relatedto(id);
                param = icalparameter_new(ICAL_RELTYPE_PARAMETER);
                icalparameter_set_xvalue(param, "SNOOZE");
                icalproperty_add_parameter(prop, param);
                icalcomponent_add_property(snooze, prop);

                /* Add TRIGGER */
                prop = icalproperty_new_trigger(snooze_trigger);
                icalcomponent_add_property(snooze, prop);
                icalcomponent_add_component(comp, snooze);
            } else {
                invalidprop(ctx, "snoozed");
            }
        }

        /* acknowledged */
        pe = readprop(ctx, alert, "acknowledged", 0, "s", &s);
        if (pe > 0) {
            icaltimetype t;
            if (utcdate_to_icaltime(s, &t)) {
                prop = icalproperty_new_acknowledged(t);
                icalcomponent_add_property(alarm, prop);
            } else {
                invalidprop(ctx, "acknowledged");
            }
        }

        /* action */
        icalproperty_action action = ICAL_ACTION_DISPLAY;
        pe = readprop(ctx, alert, "action", 0, "s", &s);
        if (pe > 0) {
            if (!strcmp(s, "email")) {
                action = ICAL_ACTION_EMAIL;
            } else if (!strcmp(s, "display")) {
                action = ICAL_ACTION_DISPLAY;
            } else {
                invalidprop(ctx, "action");
            }
        }
        prop = icalproperty_new_action(action);
        icalcomponent_add_property(alarm, prop);

        if (action == ICAL_ACTION_EMAIL) {
            /* ATTENDEE */
            const char *annotname = DAV_ANNOT_NS "<" XML_NS_CALDAV ">calendar-user-address-set";
            char *mailboxname = caldav_mboxname(httpd_userid, NULL);
            struct buf buf = BUF_INITIALIZER;
            int r = annotatemore_lookupmask(mailboxname, annotname, httpd_userid, &buf);

            char *recipient = NULL;
            if (!r && buf.len > 7 && !strncasecmp(buf_cstring(&buf), "mailto:", 7)) {
                recipient = buf_release(&buf);
            } else {
                recipient = strconcat("mailto:", httpd_userid, NULL);
            }
            icalcomponent_add_property(alarm, icalproperty_new_attendee(recipient));
            free(recipient);

            buf_free(&buf);
            free(mailboxname);

            /* SUMMARY */
            const char *summary = icalcomponent_get_summary(comp);
            if (!summary) summary = "Your event alert";
            icalcomponent_add_property(alarm, icalproperty_new_summary(summary));
        }

        /* DESCRIPTION is required for both email and display */
        const char *description = icalcomponent_get_description(comp);
        if (!description) description = "";
        icalcomponent_add_property(alarm, icalproperty_new_description(description));

        icalcomponent_add_component(comp, alarm);
        endprop(ctx);
    }
}

static void int_to_ical(struct buf *buf, int val) {
    buf_printf(buf, "%d", val);
}

/* Convert and print the JMAP byX recurrence value to ical into buf, otherwise
 * report the erroneous fieldName as invalid. If lower or upper is not NULL,
 * make sure that every byX value is within these bounds. */
static void recurrence_byX_to_ical(context_t *ctx,
                                   json_t *byX,
                                   struct buf *buf,
                                   const char *tag,
                                   int *lower,
                                   int *upper,
                                   int allowZero,
                                   const char *fieldName,
                                   void(*conv)(struct buf*, int)) {

    /* Make sure there is at least one entry. */
    if (!json_array_size(byX)) {
        invalidprop(ctx, fieldName);
        return;
    }

    /* Convert the array. */
    buf_printf(buf, ";%s=", tag);
    size_t i;
    for (i = 0; i < json_array_size(byX); i++) {
        int val;
        int err = json_unpack(json_array_get(byX, i), "i", &val);
        if (!err && !allowZero && !val) {
            err = 1;
        }
        if (!err && ((lower && val < *lower) || (upper && val > *upper))) {
            err = 2;
        }
        if (err) {
            beginprop_idx(ctx, fieldName, i);
            invalidprop(ctx, NULL);
            endprop(ctx);
            continue;
        }
        /* Prepend leading comma, if not first parameter value. */
        if (i) {
            buf_printf(buf, "%c", ',');
        }
        /* Convert the byX value to ical. */
        conv(buf, val);
    }
}

/* Create or overwrite the RRULE in the VEVENT component comp as defined by the
 * JMAP recurrence. */
static void
recurrence_to_ical(context_t *ctx, icalcomponent *comp, json_t *recur)
{
    struct buf buf = BUF_INITIALIZER;
    int pe, lower, upper;
    icalproperty *prop, *next;

    /* Purge existing RRULE. */
    for (prop = icalcomponent_get_first_property(comp, ICAL_RRULE_PROPERTY);
         prop;
         prop = next) {
        next = icalcomponent_get_next_property(comp, ICAL_RRULE_PROPERTY);
        icalcomponent_remove_property(comp, prop);
        icalproperty_free(prop);
    }

    if (!JNOTNULL(recur)) {
        return;
    }

    beginprop(ctx, "recurrenceRule");

    /* frequency */
    char *freq;
    pe = readprop(ctx, recur, "frequency", 1, "s", &freq);
    if (pe > 0) {
        char *s = xstrdup(freq);
        s = lcase(s);
        buf_printf(&buf, "FREQ=%s", s);
        free(s);
    }

    /* interval */
    int interval = 1;
    pe = readprop(ctx, recur, "interval", 0, "i", &interval);
    if (pe > 0) {
        if (interval > 1) {
            buf_printf(&buf, ";INTERVAL=%d", interval);
        } else if (interval < 1) {
            invalidprop(ctx, "interval");
        }
    }

    /* skip */
    char *skip = NULL;
    pe = readprop(ctx, recur, "skip", 0, "s", &skip);
    if (pe > 0 && strlen(skip)) {
        skip = xstrdup(skip);
        skip = ucase(skip);
        buf_printf(&buf, ";SKIP=%s", skip);
        free(skip);
    } else if (pe > 0) {
        invalidprop(ctx, "skip");
    }

    /* rscale */
    char *rscale = NULL;
    pe = readprop(ctx, recur, "rscale", skip != NULL, "s", &rscale);
    if (pe > 0 && strlen(rscale)) {
        rscale = xstrdup(rscale);
        rscale = ucase(rscale);
        buf_printf(&buf, ";RSCALE=%s", rscale);
        free(rscale);
    } else if (pe > 0) {
        invalidprop(ctx, "rscale");
    }

    /* firstDayOfWeek */
    const char *firstday = NULL;
    pe = readprop(ctx, recur, "firstDayOfWeek", 0, "s", &firstday);
    if (pe > 0) {
        char *tmp = xstrdup(firstday);
        tmp = ucase(tmp);
        if (icalrecur_string_to_weekday(tmp) != ICAL_NO_WEEKDAY) {
            buf_printf(&buf, ";WKST=%s", tmp);
        } else {
            invalidprop(ctx, "firstDayOfWeek");
        }
        free(tmp);
    }

    /* byDay */
    json_t *byday = json_object_get(recur, "byDay");
    if (json_array_size(byday) > 0) {
        size_t i;
        json_t *bd;

        buf_appendcstr(&buf, ";BYDAY=");

        json_array_foreach(byday, i, bd) {
            const char *s;
            char *day = NULL;
            json_int_t nth;

            beginprop_idx(ctx, "byDay", i);

            /* day */
            pe = readprop(ctx, bd, "day", 1, "s", &s);
            if (pe > 0) {
                day = xstrdup(s);
                day = ucase(day);
                if (icalrecur_string_to_weekday(day) == ICAL_NO_WEEKDAY) {
                    invalidprop(ctx, "day");
                }
            }

            /* nthOfPeriod */
            nth = 0;
            pe = readprop(ctx, bd, "nthOfPeriod", 0, "I", &nth);
            if (pe > 0 && !nth) {
                invalidprop(ctx, "nthOfPeriod");
            }

            /* Bail out for property errors */
            if (have_invalid_props(ctx)) {
                if (day) free(day);
                endprop(ctx);
                continue;
            }

            /* Append day */
            if (i > 0) {
                buf_appendcstr(&buf, ",");
            }
            if (nth) {
                buf_printf(&buf, "%+"JSON_INTEGER_FORMAT, nth);
            }
            buf_appendcstr(&buf, day);
            free(day);

            endprop(ctx);
        }
    } else if (byday) {
        invalidprop(ctx, "byDay");
    }

    /* byDate */
    json_t *bydate = NULL;
    lower = -31;
    upper = 31;
    pe = readprop(ctx, recur, "byDate", 0, "o", &bydate);
    if (pe > 0) {
        recurrence_byX_to_ical(ctx, bydate, &buf, "BYDATE",
                &lower, &upper, 0 /* allowZero */,
                "byDate", int_to_ical);
    }

    /* byMonth */
    json_t *bymonth = NULL;
    pe = readprop(ctx, recur, "byMonth", 0, "o", &bymonth);
    if (pe > 0) {
        if (json_array_size(bymonth) > 0) {
            size_t i;
            json_t *jval;
            buf_printf(&buf, ";BYMONTH=");
            json_array_foreach(bymonth, i, jval) {
                const char *s = json_string_value(jval);
                if (!s) {
                    beginprop_idx(ctx,"byMonth", i);
                    invalidprop(ctx, NULL);
                    endprop(ctx);
                    continue;
                }
                int val;
                char leap = 0, dummy = 0;
                int matched = sscanf(s, "%2d%c%c", &val, &leap, &dummy);
                if (matched < 1 || matched > 2 || (leap && leap != 'L') || val < 1) {
                    beginprop_idx(ctx,"byMonth", i);
                    invalidprop(ctx, NULL);
                    endprop(ctx);
                    continue;
                }
                if (i) buf_putc(&buf, ',');
                buf_printf(&buf, "%d", val);
                if (leap) buf_putc(&buf, 'L');
            }
        }
    }

    /* byYearDay */
    json_t *byyearday = NULL;
    lower = -366;
    upper = 366;
    pe = readprop(ctx, recur, "byYearDay", 0, "o", &byyearday);
    if (pe > 0) {
        recurrence_byX_to_ical(ctx, byyearday, &buf, "BYYEARDAY",
                &lower, &upper, 0 /* allowZero */,
                "byYearDay", int_to_ical);
    }


    /* byWeekNo */
    json_t *byweekno = NULL;
    lower = -53;
    upper = 53;
    pe = readprop(ctx, recur, "byWeekNo", 0, "o", &byweekno);
    if (pe > 0) {
        recurrence_byX_to_ical(ctx, byweekno, &buf, "BYWEEKNO",
                &lower, &upper, 0 /* allowZero */,
                "byWeekNo", int_to_ical);
    }

    /* byHour */
    json_t *byhour = NULL;
    lower = 0;
    upper = 23;
    pe = readprop(ctx, recur, "byHour", 0, "o", &byhour);
    if (pe > 0) {
        recurrence_byX_to_ical(ctx, byhour, &buf, "BYHOUR",
                &lower, &upper, 1 /* allowZero */,
                "byHour", int_to_ical);
    }

    /* byMinute */
    json_t *byminute = NULL;
    lower = 0;
    upper = 59;
    pe = readprop(ctx, recur, "byMinute", 0, "o", &byminute);
    if (pe > 0) {
        recurrence_byX_to_ical(ctx, byminute, &buf, "BYMINUTE",
                &lower, &upper, 1 /* allowZero */,
                "byMinute", int_to_ical);
    }

    /* bySecond */
    json_t *bysecond = NULL;
    lower = 0;
    upper = 59;
    pe = readprop(ctx, recur, "bySecond", 0, "o", &bysecond);
    if (pe > 0) {
        recurrence_byX_to_ical(ctx, bysecond, &buf, "BYSECOND",
                &lower, &upper, 1 /* allowZero */,
                "bySecond", int_to_ical);
    }

    /* bySetPosition */
    json_t *bysetpos = NULL;
    lower = 0;
    upper = 59;
    pe = readprop(ctx, recur, "bySetPosition", 0, "o", &bysetpos);
    if (pe > 0) {
        recurrence_byX_to_ical(ctx, bysetpos, &buf, "BYSETPOS",
                &lower, &upper, 1 /* allowZero */,
                "bySetPos", int_to_ical);
    }

    if (json_object_get(recur, "count") && json_object_get(recur, "until")) {
        invalidprop(ctx, "count");
        invalidprop(ctx, "until");
    }

    /* count */
    int count;
    pe = readprop(ctx, recur, "count", 0, "i", &count);
    if (pe > 0) {
        if (count > 0 && !json_object_get(recur, "until")) {
            buf_printf(&buf, ";COUNT=%d", count);
        } else {
            invalidprop(ctx, "count");
        }
    }

    /* until */
    const char *until;
    pe = readprop(ctx, recur, "until", 0, "s", &until);
    if (pe > 0) {
        icaltimetype dtloc;

        if (localdate_to_icaltime(until, &dtloc, ctx->tzstart, ctx->is_allday)) {
            icaltimezone *utc = icaltimezone_get_utc_timezone();
            icaltimetype dt = icaltime_convert_to_zone(dtloc, utc);
            buf_printf(&buf, ";UNTIL=%s", icaltime_as_ical_string(dt));
        } else {
            invalidprop(ctx, "until");
        }
    }

    if (!have_invalid_props(ctx)) {
        /* Add RRULE to component */
        struct icalrecurrencetype rt =
            icalrecurrencetype_from_string(buf_cstring(&buf));
        if (rt.freq != ICAL_NO_RECURRENCE) {
            icalcomponent_add_property(comp, icalproperty_new_rrule(rt));
        } else {
            /* Messed up the RRULE value. That's an error. */
            ctx->err->code = JMAPICAL_ERROR_UNKNOWN;
            invalidprop(ctx, NULL);
        }
    }

    endprop(ctx);
    buf_free(&buf);
}

/* Create or overwrite JMAP keywords in comp */
static void
keywords_to_ical(context_t *ctx, icalcomponent *comp, json_t *keywords)
{
    icalproperty *prop, *next;

    // FIXME should support patch here

    /* Purge existing keywords from component */
    for (prop = icalcomponent_get_first_property(comp, ICAL_CATEGORIES_PROPERTY);
         prop;
         prop = next) {

        next = icalcomponent_get_next_property(comp, ICAL_CATEGORIES_PROPERTY);
        icalcomponent_remove_property(comp, prop);
        icalproperty_free(prop);
    }

    /* Add keywords */
    json_t *jval;
    const char *keyword;
    json_object_foreach(keywords, keyword, jval) {
        if (jval != json_true()) {
            beginprop(ctx, "keywords");
            invalidprop(ctx, keyword);
            endprop(ctx);
            continue;
        }
        // FIXME known bug: libical doesn't properly
        // handle multi-values separated by comma,
        // if a single entry contains a comma.
        prop = icalproperty_new_categories(keyword);
        icalcomponent_add_property(comp, prop);
    }
}

/* Create or overwrite JMAP relatedTo in comp */
static void
relatedto_to_ical(context_t *ctx, icalcomponent *comp, json_t *relatedTo)
{
    icalproperty *prop, *next;
    icalparameter *param;

    /* Purge existing relatedTo properties from component */
    for (prop = icalcomponent_get_first_property(comp, ICAL_RELATEDTO_PROPERTY);
         prop;
         prop = next) {

        next = icalcomponent_get_next_property(comp, ICAL_RELATEDTO_PROPERTY);
        icalcomponent_remove_property(comp, prop);
        icalproperty_free(prop);
    }
    if (relatedTo == NULL || relatedTo == json_null()) return;

    /* Add relatedTo */
    const char *uid = NULL;
    json_t *relationObj = NULL;
    json_object_foreach(relatedTo, uid, relationObj) {
        json_t *relation = json_object_get(relationObj, "relation");
        beginprop_key(ctx, "relatedTo", uid);
        if (json_object_size(relation)) {
            prop = icalproperty_new_relatedto(uid);
            json_t *jval;
            const char *reltype;
            json_object_foreach(relation, reltype, jval) {
                if (jval == json_true()) {
                    char *s = ucase(xstrdup(reltype));
                    param = icalparameter_new(ICAL_RELTYPE_PARAMETER);
                    icalparameter_set_xvalue(param, s);
                    icalproperty_add_parameter(prop, param);
                    free(s);
                }
                else {
                    beginprop_key(ctx, "relation", reltype);
                    invalidprop(ctx, NULL);
                    endprop(ctx);
                }
            }
            icalcomponent_add_property(comp, prop);
        }
        else if (relation == NULL || relation == json_null()) {
            icalcomponent_add_property(comp, icalproperty_new_relatedto(uid));
        }
        else if (!json_is_object(relation)) {
            invalidprop(ctx, "relation");
        }
        else if (!json_object_size(relationObj)) {
            invalidprop(ctx, NULL);
        }
        endprop(ctx);
    }
}

static int
validate_location(context_t *ctx, json_t *loc, json_t *links)
{
    size_t invalid_cnt = invalid_prop_count(ctx);
    json_t *jval;

    /* At least one property other than rel MUST be set */
    if (json_object_size(loc) == 0 ||
        (json_object_size(loc) == 1 && json_object_get(loc, "rel"))) {
        invalidprop(ctx, NULL);
        return 0;
    }

    jval = json_object_get(loc, "name");
    if (JNOTNULL(jval) && !json_is_string(jval))
        invalidprop(ctx, "name");

    jval = json_object_get(loc, "description");
    if (JNOTNULL(jval) && !json_is_string(jval))
        invalidprop(ctx, "description");

    jval = json_object_get(loc, "rel");
    if (JNOTNULL(jval) && !json_is_string(jval))
        invalidprop(ctx, "rel");

    jval = json_object_get(loc, "coordinates");
    if (JNOTNULL(jval) && !json_is_string(jval))
        invalidprop(ctx, "coordinates");

    jval = json_object_get(loc, "timeZone");
    if (json_is_string(jval)) {
        if (!tz_from_tzid(json_string_value(jval)))
            invalidprop(ctx, "timeZone");
    }
    else if (JNOTNULL(jval)) {
        invalidprop(ctx, "timeZone");
    }

    /* linkIds */
    const char *id;
    json_t *linkids = json_object_get(loc, "linkIds");
    if (JNOTNULL(linkids) && json_is_object(linkids)) {
        json_object_foreach(linkids, id, jval) {
            if (!is_valid_jmapid(id) || !json_object_get(links, id) || jval != json_true()) {
                beginprop_key(ctx, "linkIds", id);
                invalidprop(ctx, NULL);
                endprop(ctx);
            }
        }
    }
    else if (JNOTNULL(linkids)) {
        invalidprop(ctx, "linkIds");
    }

    /* Location is invalid, if any invalid property has been added */
    return invalid_prop_count(ctx) == invalid_cnt;
}

static void
location_to_ical(context_t *ctx __attribute__((unused)), icalcomponent *comp, const char *id, json_t *loc)
{
    const char *name = json_string_value(json_object_get(loc, "name"));
    const char *rel = json_string_value(json_object_get(loc, "rel"));

    /* Gracefully handle bogus values */
    if (rel && !strcmp(rel, "unknown")) rel = NULL;

    /* Determine which property kind to use for this location.
     * Always try to create at least one LOCATION, even if CONFERENCE
     * would be more appropriate, to gracefully handle legacy clients. */
    icalproperty *prop;
    if (!icalcomponent_get_first_property(comp, ICAL_LOCATION_PROPERTY)) {
        prop = icalproperty_new(ICAL_LOCATION_PROPERTY);
    } else {
        prop = icalproperty_new(ICAL_X_PROPERTY);
        icalproperty_set_x_name(prop, JMAPICAL_XPROP_LOCATION);
    }

    /* Keep user-supplied location id */
    xjmapid_to_ical(prop, id);

    /* name, rel */
    icalvalue *val = icalvalue_new_from_string(ICAL_TEXT_VALUE, name);
    icalproperty_set_value(prop, val);
    if (rel) set_icalxparam(prop, JMAPICAL_XPARAM_REL, rel, 0);

    /* description, timeZone, coordinates */
    const char *s = json_string_value(json_object_get(loc, "description"));
    if (s) set_icalxparam(prop, JMAPICAL_XPARAM_DESCRIPTION, s, 0);
    s = json_string_value(json_object_get(loc, "timeZone"));
    if (s) set_icalxparam(prop, JMAPICAL_XPARAM_TZID, s, 0);
    s = json_string_value(json_object_get(loc, "coordinates"));
    if (s) set_icalxparam(prop, JMAPICAL_XPARAM_GEO, s, 0);

    /* linkIds */
    json_t *jval;
    const char *key;
    json_object_foreach(json_object_get(loc, "linkIds"), key, jval) {
        set_icalxparam(prop, JMAPICAL_XPARAM_LINKID, key, 0);
    }

    icalcomponent_add_property(comp, prop);
}

/* Create or overwrite the JMAP locations in comp */
static void
locations_to_ical(context_t *ctx, icalcomponent *comp, json_t *locations, json_t *links)
{
    json_t *loc;
    const char *id;

    /* Purge existing locations */
    remove_icalprop(comp, ICAL_LOCATION_PROPERTY);
    remove_icalprop(comp, ICAL_GEO_PROPERTY);
    remove_icalxprop(comp, JMAPICAL_XPROP_LOCATION);
    remove_icalxprop(comp, "X-APPLE-STRUCTURED-LOCATION");

    /* Bail out if no location needs to be set */
    if (!JNOTNULL(locations)) {
        return;
    }

    /* Add locations */
    json_object_foreach(locations, id, loc) {
        beginprop_key(ctx, "locations", id);

        /* Validate the location id */
        if (!is_valid_jmapid(id)) {
            invalidprop(ctx, NULL);
            endprop(ctx);
            continue;
        }

        /* Ignore end timeZone locations */
        if (location_is_endtimezone(loc)) {
            endprop(ctx);
            continue;
        }
        /* Validate location */
        if (!validate_location(ctx, loc, links)) {
            endprop(ctx);
            continue;
        }

        /* Add location */
        location_to_ical(ctx, comp, id, loc);
        endprop(ctx);
    }
}

/* Create or overwrite the JMAP virtualLocations in comp */
static void
virtuallocations_to_ical(context_t *ctx, icalcomponent *comp, json_t *locations)
{
    json_t *loc;
    const char *id;

    remove_icalprop(comp, ICAL_CONFERENCE_PROPERTY);
    if (!JNOTNULL(locations)) {
        return;
    }

    json_object_foreach(locations, id, loc) {
        beginprop_key(ctx, "virtualLocations", id);

        /* Validate the location id */
        if (!is_valid_jmapid(id)) {
            invalidprop(ctx, NULL);
            endprop(ctx);
            continue;
        }

        icalproperty *prop = icalproperty_new(ICAL_CONFERENCE_PROPERTY);
        xjmapid_to_ical(prop, id);

        /* uri */
        json_t *juri = json_object_get(loc, "uri");
        if (json_is_string(juri)) {
            const char *uri = json_string_value(juri);
            icalvalue *val = icalvalue_new_from_string(ICAL_URI_VALUE, uri);
            icalproperty_set_value(prop, val);
        }
        else {
            invalidprop(ctx, "uri");
        }

        /* name */
        json_t *jname = json_object_get(loc, "name");
        if (json_is_string(juri)) {
            const char *name = json_string_value(jname);
            icalproperty_add_parameter(prop, icalparameter_new_label(name));
        }
        else {
            invalidprop(ctx, "uri");
        }


        /* description */
        json_t *jdescription = json_object_get(loc, "description");
        if (json_is_string(jdescription)) {
            const char *desc = json_string_value(jdescription);
            set_icalxparam(prop, JMAPICAL_XPARAM_DESCRIPTION, desc, 0);
        }
        else if (JNOTNULL(jdescription)) {
            invalidprop(ctx, "description");
        }

        icalcomponent_add_property(comp, prop);

        endprop(ctx);
    }
}

static void set_language_icalprop(icalcomponent *comp, icalproperty_kind kind,
                                  const char *lang)
{
    icalproperty *prop;
    icalparameter *param;

    prop = icalcomponent_get_first_property(comp, kind);
    if (!prop) return;

    icalproperty_remove_parameter_by_kind(prop, ICAL_LANGUAGE_PARAMETER);
    if (!lang) return;

    param = icalparameter_new(ICAL_LANGUAGE_PARAMETER);
    icalparameter_set_language(param, lang);
    icalproperty_add_parameter(prop, param);
}

static void
overrides_to_ical(context_t *ctx, icalcomponent *comp, json_t *overrides)
{
    json_t *override, *master;
    const char *id;
    icalcomponent *excomp, *next, *ical;
    context_t *fromctx;
    hash_table recurs = HASH_TABLE_INITIALIZER;
    int n;

    /* Purge EXDATE, RDATE */
    remove_icalprop(comp, ICAL_RDATE_PROPERTY);
    remove_icalprop(comp, ICAL_EXDATE_PROPERTY);

    /* Move VEVENT exceptions to a cache */
    ical = icalcomponent_get_parent(comp);
    n = icalcomponent_count_components(ical, ICAL_VEVENT_COMPONENT);
    construct_hash_table(&recurs, n + 1, 0);
    for (excomp = icalcomponent_get_first_component(ical, ICAL_VEVENT_COMPONENT);
         excomp;
         excomp = next) {

        icaltimetype recurid;
        char *t;

        next = icalcomponent_get_next_component(ical, ICAL_VEVENT_COMPONENT);
        if (excomp == comp) continue;

        /* Index VEVENT by its LocalDate recurrence id */
        icalcomponent_remove_component(ical, excomp);
        recurid = icalcomponent_get_recurrenceid(excomp);
        t = localdate_from_icaltime_r(recurid);
        hash_insert(t, excomp, &recurs);
        free(t);
    }

    if (json_is_null(overrides)) {
        free_hash_table(&recurs, (void (*)(void *))icalcomponent_free);
        return;
    }

    /* Convert current master event to JMAP */
    fromctx = context_new(NULL, NULL, JMAPICAL_READ_MODE);
    master = calendarevent_from_ical(fromctx, comp);
    if (!master) {
        if (ctx->err) {
            ctx->err->code = JMAPICAL_ERROR_UNKNOWN;
            context_free(fromctx);
            return;
        }
    }
    json_object_del(master, "recurrenceRule");
    json_object_del(master, "recurrenceOverrides");

    json_object_foreach(overrides, id, override) {
        icaltimetype start;

        beginprop_key(ctx, "recurrenceOverrides", id);

        if (!localdate_to_icaltime(id, &start, ctx->tzstart, ctx->is_allday)) {
            invalidprop(ctx, NULL);
            endprop(ctx);
            continue;
        }

        json_t *excluded = json_object_get(override, "excluded");
        if (excluded) {
            if (json_object_size(override) == 1 && excluded == json_true()) {
                /* Add EXDATE */
                dtprop_to_ical(comp, start, ctx->tzstart, 0, ICAL_EXDATE_PROPERTY);
            }
            else {
                invalidprop(ctx, id);
                endprop(ctx);
                continue;
            }
        } else if (!json_object_size(override)) {
            /* Add RDATE */
            dtprop_to_ical(comp, start, ctx->tzstart, 0, ICAL_RDATE_PROPERTY);
        } else {
            /* Add VEVENT exception */
            context_t *toctx;
            json_t *ex, *val;
            const char *key;
            int ignore = 0;

            /* JMAP spec: "A pointer MUST NOT start with one of the following
             * prefixes; any patch with a such a key MUST be ignored" */
            json_object_foreach(override, key, val) {
                if (!strcmp(key, "uid") ||
                    !strcmp(key, "relatedTo") ||
                    !strcmp(key, "prodId") ||
                    !strcmp(key, "isAllDay") ||
                    !strcmp(key, "recurrenceRule") ||
                    !strcmp(key, "recurrenceOverrides") ||
                    !strcmp(key, "replyTo") ||
                    !strcmp(key, "participantId")) {

                    ignore = 1;
                }
            }
            if (ignore)
                continue;

            /* If the override doesn't have a custom start date, use
             * the LocalDate in the recurrenceOverrides object key */
            if (!json_object_get(override, "start")) {
                json_object_set_new(override, "start", json_string(id));
            }

            /* Create overridden event from patch and master event */
            if (!(ex = jmap_patchobject_apply(master, override))) {
                invalidprop(ctx, NULL);
                endprop(ctx);
                continue;
            }

            /* Lookup or create the VEVENT for this override */
            if ((excomp = hash_del(id, &recurs)) == NULL) {
                excomp = icalcomponent_new_clone(comp);
                remove_icalprop(excomp, ICAL_RDATE_PROPERTY);
                remove_icalprop(excomp, ICAL_EXDATE_PROPERTY);
                remove_icalprop(excomp, ICAL_RRULE_PROPERTY);
            }
            dtprop_to_ical(excomp, start,
                           ctx->tzstart, 1, ICAL_RECURRENCEID_PROPERTY);

            /* Convert the override event to iCalendar */
            toctx = context_new(NULL, ctx->err, ctx->mode | JMAPICAL_EXC_MODE);
            calendarevent_to_ical(toctx, excomp, ex);
            if (have_invalid_props(toctx)) {
                json_t *invalid = get_invalid_props(toctx);
                invalidprop_append(ctx, invalid);
                json_decref(invalid);
            }
            context_free(toctx);

            /* Add the exception */
            icalcomponent_add_component(ical, excomp);
            json_decref(ex);
        }

        endprop(ctx);
    }

    free_hash_table(&recurs, (void (*)(void *))icalcomponent_free);
    context_free(fromctx);
    json_decref(master);
}

/* Create or overwrite the iCalendar properties in VEVENT comp based on the
 * properties the JMAP calendar event. This writes a *complete* jsevent and
 * does not implement patch object semantics.
 *
 * Collect all required timezone ids in ctx. 
 */
static void
calendarevent_to_ical(context_t *ctx, icalcomponent *comp, json_t *event)
{
    int pe; /* parse error */
    const char *val = NULL;
    icalproperty *prop = NULL;
    int is_exc = ctx->mode & JMAPICAL_EXC_MODE;
    json_t *overrides = NULL;

    /* Do not preserve any current contents */
    json_incref(event);

    icaltimezone *utc = icaltimezone_get_utc_timezone();
    icaltimetype now = icaltime_current_time_with_zone(utc);

    json_t *excluded = json_object_get(event, "excluded");
    if (excluded && excluded != json_false()) {
        invalidprop(ctx, "excluded");
    }

    /* uid */
    icalcomponent_set_uid(comp, ctx->uid);

    json_t *jtype = json_object_get(event, "@type");
    if (JNOTNULL(jtype) && json_is_string(jtype)) {
        if (strcmp(json_string_value(jtype), "jsevent")) {
            invalidprop(ctx, "@type");
        }
    }
    else if (JNOTNULL(jtype)) {
        invalidprop(ctx, "@type");
    }

    /* isAllDay */
    readprop(ctx, event, "isAllDay", 1, "b", &ctx->is_allday);

    /* start, duration, timeZone */
    startend_to_ical(ctx, comp, event);

    /* relatedTo */
    json_t *relatedTo = NULL;
    pe = readprop(ctx, event, "relatedTo", 0, "o", &relatedTo);
    if (pe > 0) {
        if (json_is_null(relatedTo) || json_object_size(relatedTo)) {
            relatedto_to_ical(ctx, comp, relatedTo);
        } else {
            invalidprop(ctx, "relatedTo");
        }
    }

    /* prodId */
    if (!is_exc) {
        val = NULL;
        if (!json_is_null(json_object_get(event, "prodId"))) {
            pe = readprop(ctx, event, "prodId", 0, "s", &val);
            struct buf buf = BUF_INITIALIZER;
            if (!val) {
                /* Use same product id like jcal.c */
                buf_setcstr(&buf, "-//CyrusJMAP.org/Cyrus ");
                buf_appendcstr(&buf, CYRUS_VERSION);
                buf_appendcstr(&buf, "//EN");
                val = buf_cstring(&buf);
            }
            /* Set PRODID in the VCALENDAR */
            icalcomponent *ical = icalcomponent_get_parent(comp);
            remove_icalprop(ical, ICAL_PRODID_PROPERTY);
            prop = icalproperty_new_prodid(val);
            icalcomponent_add_property(ical, prop);
            buf_free(&buf);
        }
    }

    /* created */
    dtprop_to_ical(comp, now, utc, 1, ICAL_CREATED_PROPERTY);

    /* updated */
    dtprop_to_ical(comp, now, utc, 1, ICAL_DTSTAMP_PROPERTY);

    /* sequence */
    icalcomponent_set_sequence(comp, 0);

    json_t *jprio = json_object_get(event, "priority");
    if (json_integer_value(jprio) >= 0 || json_integer_value(jprio) <= 9) {
        prop = icalproperty_new_priority(json_integer_value(jprio));
        icalcomponent_add_property(comp, prop);
    } else if (JNOTNULL(jprio)) {
        invalidprop(ctx, "priority");
    }

    /* title */
    pe = readprop(ctx, event, "title", 1, "s", &val);
    if (pe > 0) {
        icalcomponent_set_summary(comp, val);
    }

    /* description and descriptionContentType */
    const char *desc = NULL;
    const char *desc_content_type = NULL;
    pe = readprop(ctx, event, "descriptionContentType", 0, "s", &desc_content_type);
    pe = readprop(ctx, event, "description", 0, "s", &desc);
    if (pe > 0 && strlen(desc)) {
        description_to_ical(ctx, comp, desc, desc_content_type);
    }

    const char *method = NULL;
    pe = readprop(ctx, event, "method", 0, "s", &method);
    if (pe > 0) {
        icalproperty_method icalmethod = icalenum_string_to_method(method);
        if (icalmethod != ICAL_METHOD_NONE) {
            icalcomponent *ical = icalcomponent_get_parent(comp);
            icalcomponent_set_method(ical, icalmethod);
        }
        else {
            invalidprop(ctx, "method");
        }
    }

    /* color */
    pe = readprop(ctx, event, "color", 0, "s", &val);
    if (pe > 0 && strlen(val)) {
        prop = icalproperty_new_color(val);
        icalcomponent_add_property(comp, prop);
    }

    /* keywords */
    json_t *keywords = NULL;
    pe = readprop(ctx, event, "keywords", 0, "o", &keywords);
    if (pe > 0) {
        if (json_is_null(keywords) || json_is_object(keywords)) {
            keywords_to_ical(ctx, comp, keywords);
        } else {
            invalidprop(ctx, "keywords");
        }
    }

    /* links */
    json_t *links = NULL;
    pe = readprop(ctx, event, "links", 0, "o", &links);
    if (pe > 0) {
        if (json_is_null(links) || json_object_size(links)) {
            links_to_ical(ctx, comp, links, "links");
        } else {
            invalidprop(ctx, "links");
        }
    }

    /* locale */
    if (!json_is_null(json_object_get(event, "locale"))) {
        pe = readprop(ctx, event, "locale", 0, "s", &val);
        if (pe > 0) {
            set_language_icalprop(comp, ICAL_SUMMARY_PROPERTY, NULL);
            set_language_icalprop(comp, ICAL_DESCRIPTION_PROPERTY, NULL);
            if (strlen(val)) {
                set_language_icalprop(comp, ICAL_SUMMARY_PROPERTY, val);
            }
        }
    } else {
        set_language_icalprop(comp, ICAL_SUMMARY_PROPERTY, NULL);
        set_language_icalprop(comp, ICAL_DESCRIPTION_PROPERTY, NULL);
    }

    /* locations */
    json_t *locations = NULL;
    pe = readprop(ctx, event, "locations", 0, "o", &locations);
    if (pe > 0) {
        if (json_is_null(locations) || json_object_size(locations)) {
            json_t *links = json_object_get(event, "links");
            locations_to_ical(ctx, comp, locations, links);
        } else {
            invalidprop(ctx, "locations");
        }
    }

    /* virtualLocations */
    json_t *virtualLocations = NULL;
    pe = readprop(ctx, event, "virtualLocations", 0, "o", &virtualLocations);
    if (pe > 0) {
        if (json_is_null(virtualLocations) || json_object_size(virtualLocations)) {
            virtuallocations_to_ical(ctx, comp, virtualLocations);
        } else {
            invalidprop(ctx, "virtualLocations");
        }
    }

    /* recurrenceRule */
    json_t *recurrence = NULL;
    pe = readprop(ctx, event, "recurrenceRule", 0, "o", &recurrence);
    if (pe > 0 && !is_exc) {
        recurrence_to_ical(ctx, comp, recurrence);
    }

    /* status */
    enum icalproperty_status status = ICAL_STATUS_NONE;
    pe = readprop(ctx, event, "status", 0, "s", &val);
    if (pe > 0) {
        if (!strcmp(val, "confirmed")) {
            status = ICAL_STATUS_CONFIRMED;
        } else if (!strcmp(val, "cancelled")) {
            status = ICAL_STATUS_CANCELLED;
        } else if (!strcmp(val, "tentative")) {
            status = ICAL_STATUS_TENTATIVE;
        } else {
            invalidprop(ctx, "status");
        }
    } else if (!pe) {
        status = ICAL_STATUS_CONFIRMED;
    }
    if (status != ICAL_STATUS_NONE) {
        remove_icalprop(comp, ICAL_STATUS_PROPERTY);
        icalcomponent_set_status(comp, status);
    }

    /* freeBusyStatus */
    pe = readprop(ctx, event, "freeBusyStatus", 0, "s", &val);
    if (pe > 0) {
        enum icalproperty_transp v = ICAL_TRANSP_NONE;
        if (!strcmp(val, "free")) {
            v = ICAL_TRANSP_TRANSPARENT;
        } else if (!strcmp(val, "busy")) {
            v = ICAL_TRANSP_OPAQUE;
        } else {
            invalidprop(ctx, "freeBusyStatus");
        }
        if (v != ICAL_TRANSP_NONE) {
            prop = icalcomponent_get_first_property(comp, ICAL_TRANSP_PROPERTY);
            if (prop) {
                icalproperty_set_transp(prop, v);
            } else {
                icalcomponent_add_property(comp, icalproperty_new_transp(v));
            }
        }
    }

    /* privacy */
    pe = readprop(ctx, event, "privacy", 0, "s", &val);
    if (pe > 0) {
        enum icalproperty_class v = ICAL_CLASS_NONE;
        if (!strcmp(val, "public")) {
            v = ICAL_CLASS_PUBLIC;
        } else if (!strcmp(val, "private")) {
            v = ICAL_CLASS_PRIVATE;
        } else if (!strcmp(val, "secret")) {
            v = ICAL_CLASS_CONFIDENTIAL;
        } else {
            invalidprop(ctx, "privacy");
        }
        if (v != ICAL_CLASS_NONE) {
            prop = icalcomponent_get_first_property(comp, ICAL_CLASS_PROPERTY);
            if (prop) {
                icalproperty_set_class(prop, v);
            } else {
                icalcomponent_add_property(comp, icalproperty_new_class(v));
            }
        }
    }

    /* replyTo and participants */
    participants_to_ical(ctx, comp, event);

    /* participantId: readonly */

    /* useDefaultAlerts */
    int default_alerts;
    pe = readprop(ctx, event, "useDefaultAlerts", 0, "b", &default_alerts);
    if (pe > 0) {
        remove_icalxprop(comp, JMAPICAL_XPROP_USEDEFALERTS);
        if (default_alerts) {
            icalvalue *val = icalvalue_new_boolean(1);
            prop = icalproperty_new(ICAL_X_PROPERTY);
            icalproperty_set_x_name(prop, JMAPICAL_XPROP_USEDEFALERTS);
            icalproperty_set_value(prop, val);
            icalcomponent_add_property(comp, prop);
        }
    }

    /* alerts */
    json_t *alerts = NULL;
    pe = readprop(ctx, event, "alerts", 0, "o", &alerts);
    if (pe > 0) {
        if (json_is_null(alerts) || json_object_size(alerts)) {
            alerts_to_ical(ctx, comp, alerts);
        } else {
            invalidprop(ctx, "alerts");
        }
    }

    /* recurrenceOverrides - must be last to apply patches */
    pe = readprop(ctx, event, "recurrenceOverrides", 0, "o", &overrides);
    if (pe > 0 && !is_exc) {
        overrides_to_ical(ctx, comp, overrides);
    }

    /* Bail out for property errors */
    if (have_invalid_props(ctx)) {
        json_decref(event);
        return;
    }

    /* Check JMAP specification conditions on the generated iCalendar file, so 
     * this also doubles as a sanity check. Note that we *could* report a
     * property here as invalid, which had only been set by the client in a
     * previous request. */

    /* Either both organizer and attendees are null, or neither are. */
    if ((icalcomponent_get_first_property(comp, ICAL_ORGANIZER_PROPERTY) == NULL) !=
        (icalcomponent_get_first_property(comp, ICAL_ATTENDEE_PROPERTY) == NULL)) {
        invalidprop(ctx, "replyTo");
        invalidprop(ctx, "participants");
    }
    json_decref(event);
}

icalcomponent*
jmapical_toical(json_t *obj, jmapical_err_t *err)
{
    icalcomponent *ical = NULL;
    icalcomponent *comp = NULL;
    context_t *ctx = NULL;

    /* Create a new VCALENDAR. */
    ical = icalcomponent_new_vcalendar();
    icalcomponent_add_property(ical, icalproperty_new_version("2.0"));
    icalcomponent_add_property(ical, icalproperty_new_calscale("GREGORIAN"));

    /* Create a new VEVENT. */
    icaltimezone *utc = icaltimezone_get_utc_timezone();
    struct icaltimetype now =
        icaltime_from_timet_with_zone(time(NULL), 0, utc);
    comp = icalcomponent_new_vevent();
    icalcomponent_set_sequence(comp, 0);
    icalcomponent_set_dtstamp(comp, now);
    icalcomponent_add_property(comp, icalproperty_new_created(now));
    icalcomponent_add_component(ical, comp);

    /* Convert the JMAP calendar event to ical. */
    ctx = context_new(NULL, err, JMAPICAL_WRITE_MODE);
    ctx->uid = json_string_value(json_object_get(obj, "uid"));
    if (!ctx->uid) {
        if (err) err->code = JMAPICAL_ERROR_UID;
        if (ical) icalcomponent_free(ical);
        ical = NULL;
        goto done;
    }
    calendarevent_to_ical(ctx, comp, obj);
    icalcomponent_add_required_timezones(ical);

    /* Bubble up any property errors. */
    if (have_invalid_props(ctx) && err) {
        err->code = JMAPICAL_ERROR_PROPS;
        err->props = get_invalid_props(ctx);
        if (ical) icalcomponent_free(ical);
        ical = NULL;
    }

    /* Free erroneous ical data */
    if (ctx->err && ctx->err->code) {
        if (ical) icalcomponent_free(ical);
        ical = NULL;
    }

done:
    context_free(ctx);
    return ical;
}

const char *
jmapical_strerror(int err)
{
    switch (err) {
        case 0:
            return "jmapical: success";
        case JMAPICAL_ERROR_CALLBACK:
            return "jmapical: callback error";
        case JMAPICAL_ERROR_MEMORY:
            return "jmapical: no memory";
        case JMAPICAL_ERROR_ICAL:
            return "jmapical: iCalendar error";
        case JMAPICAL_ERROR_PROPS:
            return "jmapical: property error";
        case JMAPICAL_ERROR_UID:
            return "jmapical: iCalendar uid error";
        default:
            return "jmapical: unknown error";
    }
}

/*
 * Construct a jevent string for an iCalendar component.
 */
EXPORTED struct buf *icalcomponent_as_jevent_string(icalcomponent *ical)
{
    struct buf *ret;
    json_t *jcal;
    size_t flags = JSON_PRESERVE_ORDER;
    char *buf;

    if (!ical) return NULL;

    jcal = jmapical_tojmap(ical, NULL, NULL);

    flags |= (config_httpprettytelemetry ? JSON_INDENT(2) : JSON_COMPACT);
    buf = json_dumps(jcal, flags);

    json_decref(jcal);

    ret = buf_new();
    buf_initm(ret, buf, strlen(buf));

    return ret;
}

EXPORTED icalcomponent *jevent_string_as_icalcomponent(const struct buf *buf)
{
    json_t *obj;
    json_error_t jerr;
    icalcomponent *ical;
    const char *str = buf_cstring(buf);

    if (!str) return NULL;

    obj = json_loads(str, 0, &jerr);
    if (!obj) {
        syslog(LOG_WARNING, "json parse error: '%s'", jerr.text);
        return NULL;
    }

    ical = jmapical_toical(obj, NULL);

    json_decref(obj);

    return ical;
}

