/*
 * queue_display - ACAP for Axis Display Speaker
 *
 * Subscribes to MQTT topics (raw socket, no external library) and drives
 * the Speaker Display Notification API via libcurl.
 *
 * Settings are stored via axparameter; the Axis device web UI shows them
 * automatically via the manifest paramConfig declaration.
 *
 * Dependencies (all from the Axis ACAP Native SDK):
 *   glib-2.0, gio-2.0, axparameter, libcurl
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <syslog.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <axsdk/axparameter.h>
#include <curl/curl.h>

#define APP_NAME "queue_display"
#define LOG(fmt, ...)      do { syslog(LOG_INFO,    fmt, ##__VA_ARGS__); printf(fmt "\n",        ##__VA_ARGS__); } while(0)
#define LOG_WARN(fmt, ...) do { syslog(LOG_WARNING, fmt, ##__VA_ARGS__); printf("WARN: " fmt "\n", ##__VA_ARGS__); } while(0)

/* ========== Settings ========== */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_mqtt_host[256]   = "127.0.0.1";
static int  g_mqtt_port        = 1883;
static char g_mqtt_user[256]   = "";
static char g_mqtt_pass[256]   = "";
static char g_queue_topic[256] = "restaurant/queue";
static char g_ready_topic[256] = "restaurant/ready";
static int  g_ready_ms         = 8000;
static int  g_reconnect        = 0;

static AXParameter *g_param = NULL;

static void param_str(const char *n, char *dst, size_t sz) {
    gchar *v = NULL;
    if (ax_parameter_get(g_param, n, &v, NULL) && v) { g_strlcpy(dst, v, sz); g_free(v); }
}
static void param_int(const char *n, int *dst) {
    gchar *v = NULL;
    if (ax_parameter_get(g_param, n, &v, NULL) && v) { *dst = atoi(v); g_free(v); }
}

static void on_param(const gchar *name, const gchar *value, gpointer ud) {
    pthread_mutex_lock(&g_mu);
    if      (!strcmp(name, "MqttHost"))        g_strlcpy(g_mqtt_host, value, sizeof(g_mqtt_host));
    else if (!strcmp(name, "MqttPort"))        g_mqtt_port = atoi(value);
    else if (!strcmp(name, "MqttUsername"))    g_strlcpy(g_mqtt_user, value, sizeof(g_mqtt_user));
    else if (!strcmp(name, "MqttPassword"))    g_strlcpy(g_mqtt_pass, value, sizeof(g_mqtt_pass));
    else if (!strcmp(name, "QueueTopic"))      g_strlcpy(g_queue_topic, value, sizeof(g_queue_topic));
    else if (!strcmp(name, "ReadyTopic"))      g_strlcpy(g_ready_topic, value, sizeof(g_ready_topic));
    else if (!strcmp(name, "ReadyDurationMs")) g_ready_ms = atoi(value);
    g_reconnect = 1;
    pthread_mutex_unlock(&g_mu);
    /* Avoid logging the password value */
    if (!strcmp(name, "MqttPassword"))
        LOG("Param: %s = ***", name);
    else
        LOG("Param: %s = %s", name, value);
}

static void settings_init(void) {
    GError *err = NULL;
    g_param = ax_parameter_new(APP_NAME, &err);
    if (!g_param) { LOG_WARN("ax_parameter_new: %s", err ? err->message : "?"); if (err) g_error_free(err); return; }
    param_str("MqttHost",        g_mqtt_host,   sizeof(g_mqtt_host));
    param_int("MqttPort",        &g_mqtt_port);
    param_str("MqttUsername",    g_mqtt_user,   sizeof(g_mqtt_user));
    param_str("MqttPassword",    g_mqtt_pass,   sizeof(g_mqtt_pass));
    param_str("QueueTopic",      g_queue_topic, sizeof(g_queue_topic));
    param_str("ReadyTopic",      g_ready_topic, sizeof(g_ready_topic));
    param_int("ReadyDurationMs", &g_ready_ms);
    const char *names[] = { "MqttHost","MqttPort","MqttUsername","MqttPassword","QueueTopic","ReadyTopic","ReadyDurationMs",NULL };
    for (int i = 0; names[i]; i++)
        ax_parameter_register_callback(g_param, names[i], on_param, NULL, NULL);
}

/* ========== VAPIX display ========== */

static char g_creds[256] = "";

static void vapix_get_credentials(void) {
    GError *err = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
    if (!conn) { LOG_WARN("D-Bus: %s", err ? err->message : "?"); if (err) g_error_free(err); return; }
    GVariant *res = g_dbus_connection_call_sync(conn,
        "com.axis.HTTPConf1", "/com/axis/HTTPConf1/VAPIXServiceAccounts1",
        "com.axis.HTTPConf1.VAPIXServiceAccounts1", "GetCredentials",
        g_variant_new("(s)", APP_NAME), G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    g_object_unref(conn);
    if (!res) { LOG_WARN("GetCredentials: %s", err ? err->message : "?"); if (err) g_error_free(err); return; }
    const char *s = NULL;
    g_variant_get(res, "(&s)", &s);
    if (s) g_strlcpy(g_creds, s, sizeof(g_creds));
    g_variant_unref(res);
}

static size_t curl_sink(void *p, size_t s, size_t n, void *ud) { return s * n; }

static void display_post(const char *json) {
    if (!g_creds[0]) { LOG_WARN("display_post: no credentials"); return; }
    CURL *curl = curl_easy_init();
    if (!curl) return;
    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL,           "http://127.0.0.12/config/rest/speaker-display-notification/v1/simple");
    curl_easy_setopt(curl, CURLOPT_USERPWD,       g_creds);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH,      CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_sink);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,      1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       5L);
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) LOG_WARN("display_post: %s", curl_easy_strerror(rc));
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
}

/* ========== Display logic ========== */

static int  g_showing_ready  = 0;
static char g_queue_str[512] = "";
static char g_ready_str[512] = "";
static guint g_revert_src    = 0;

static void numbers_from_json(const char *json, char *out, size_t sz) {
    out[0] = '\0';
    const char *p = strchr(json, '[');
    if (!p) return;
    p++;
    char tmp[512] = "";
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (*p == ']' || !*p) break;
        char *end; long n = strtol(p, &end, 10);
        if (end == p) break;
        char entry[16]; snprintf(entry, sizeof(entry), "#%ld  ", n);
        g_strlcat(tmp, entry, sizeof(tmp));
        p = end;
    }
    size_t l = strlen(tmp);
    while (l > 0 && tmp[l-1] == ' ') tmp[--l] = '\0';
    g_strlcpy(out, tmp, sz);
}

static void show_queue(void);

static gboolean revert_cb(gpointer ud) {
    g_showing_ready = 0; g_revert_src = 0;
    show_queue(); return G_SOURCE_REMOVE;
}

static void show_queue(void) {
    char msg[600], json[1024];
    snprintf(msg, sizeof(msg), "PREPARING:  %s", g_queue_str[0] ? g_queue_str : "(none)");
    snprintf(json, sizeof(json),
        "{\"data\":{\"message\":\"%s\","
        "\"textColor\":\"#FFFFFF\",\"backgroundColor\":\"#1A1A2E\","
        "\"textSize\":\"large\",\"scrollDirection\":\"fromRightToLeft\","
        "\"scrollSpeed\":4}}",
        msg);
    display_post(json);
    LOG("show_queue: %s", g_queue_str);
}

static void show_ready(void) {
    pthread_mutex_lock(&g_mu); int dur = g_ready_ms; pthread_mutex_unlock(&g_mu);
    char msg[600], json[1024];
    snprintf(msg, sizeof(msg), "ORDER READY:  %s", g_ready_str);
    snprintf(json, sizeof(json),
        "{\"data\":{\"message\":\"%s\","
        "\"textColor\":\"#000000\",\"backgroundColor\":\"#00CC44\","
        "\"textSize\":\"large\",\"scrollDirection\":\"fromRightToLeft\","
        "\"scrollSpeed\":5,\"duration\":{\"type\":\"time\",\"value\":%d}}}",
        msg, dur);
    display_post(json);
    LOG("show_ready: %s", g_ready_str);
    if (g_revert_src) { g_source_remove(g_revert_src); g_revert_src = 0; }
    g_revert_src = g_timeout_add(dur, revert_cb, NULL);
}

static void on_message(const char *topic, const char *payload) {
    char qt[256], rt[256];
    pthread_mutex_lock(&g_mu);
    g_strlcpy(qt, g_queue_topic, sizeof(qt));
    g_strlcpy(rt, g_ready_topic, sizeof(rt));
    pthread_mutex_unlock(&g_mu);

    if (!strcmp(topic, qt)) {
        numbers_from_json(payload, g_queue_str, sizeof(g_queue_str));
        if (!g_showing_ready) show_queue();
    } else if (!strcmp(topic, rt)) {
        numbers_from_json(payload, g_ready_str, sizeof(g_ready_str));
        if (g_ready_str[0]) {
            g_showing_ready = 1; show_ready();
        } else if (g_showing_ready) {
            g_showing_ready = 0;
            if (g_revert_src) { g_source_remove(g_revert_src); g_revert_src = 0; }
            show_queue();
        }
    }
}

/* ========== Minimal MQTT 3.1.1 subscriber (raw POSIX sockets) ========== */

typedef struct { char *topic; char *payload; } Msg;
static volatile int g_running = 1;

static gboolean dispatch(gpointer ud) {
    Msg *m = ud; on_message(m->topic, m->payload);
    g_free(m->topic); g_free(m->payload); g_free(m);
    return G_SOURCE_REMOVE;
}

static void w16(uint8_t *b, uint16_t v) { b[0] = v >> 8; b[1] = v & 0xFF; }

static int encode_rl(uint8_t *b, uint32_t n) {
    int i = 0;
    do { b[i] = n & 0x7F; n >>= 7; if (n) b[i] |= 0x80; i++; } while (n);
    return i;
}
static int read_rl(int fd, uint32_t *out) {
    uint32_t v = 0; int sh = 0; uint8_t b;
    do { if (recv(fd, &b, 1, MSG_WAITALL) != 1) return -1; v |= (uint32_t)(b & 0x7F) << sh; sh += 7; } while ((b & 0x80) && sh < 28);
    *out = v; return 0;
}
static int recv_exact(int fd, void *buf, uint32_t len) {
    uint8_t *p = buf;
    while (len > 0) { int n = recv(fd, p, len, MSG_WAITALL); if (n <= 0) return -1; p += n; len -= n; }
    return 0;
}
static int send_all(int fd, const void *buf, int len) {
    const uint8_t *p = buf;
    while (len > 0) { int n = send(fd, p, len, MSG_NOSIGNAL); if (n <= 0) return -1; p += n; len -= n; }
    return 0;
}
static int skip_bytes(int fd, uint32_t n) {
    uint8_t tmp[256];
    while (n > 0) { uint32_t c = n < sizeof(tmp) ? n : (uint32_t)sizeof(tmp); if (recv_exact(fd, tmp, c) < 0) return -1; n -= c; }
    return 0;
}

static int mqtt_connect_pkt(int fd, const char *cid, const char *user, const char *pass) {
    uint16_t cid_len  = (uint16_t)strlen(cid);
    uint16_t user_len = user && user[0] ? (uint16_t)strlen(user) : 0;
    uint16_t pass_len = pass && pass[0] ? (uint16_t)strlen(pass) : 0;
    /* connect flags: CleanSession=1, plus Username/Password bits if set */
    uint8_t flags = 0x02;
    if (user_len) flags |= 0x80;
    if (pass_len) flags |= 0x40;
    uint32_t rem = 10 + 2 + cid_len
                 + (user_len ? 2 + user_len : 0)
                 + (pass_len ? 2 + pass_len : 0);
    uint8_t buf[768]; int pos = 0;
    buf[pos++] = 0x10; pos += encode_rl(buf + pos, rem);
    w16(buf + pos, 4); pos += 2; memcpy(buf + pos, "MQTT", 4); pos += 4;
    buf[pos++] = 4; buf[pos++] = flags;
    w16(buf + pos, 60); pos += 2;
    w16(buf + pos, cid_len); pos += 2; memcpy(buf + pos, cid, cid_len); pos += cid_len;
    if (user_len) { w16(buf + pos, user_len); pos += 2; memcpy(buf + pos, user, user_len); pos += user_len; }
    if (pass_len) { w16(buf + pos, pass_len); pos += 2; memcpy(buf + pos, pass, pass_len); pos += pass_len; }
    return send_all(fd, buf, pos);
}
static int mqtt_subscribe_pkt(int fd, const char *topic, uint16_t pid) {
    uint16_t tlen = (uint16_t)strlen(topic);
    uint32_t rem = 2 + 2 + tlen + 1;
    uint8_t buf[512]; int pos = 0;
    buf[pos++] = 0x82; pos += encode_rl(buf + pos, rem);
    w16(buf + pos, pid); pos += 2; w16(buf + pos, tlen); pos += 2;
    memcpy(buf + pos, topic, tlen); pos += tlen; buf[pos++] = 0;
    return send_all(fd, buf, pos);
}
static int mqtt_pingreq(int fd) { uint8_t p[2] = {0xC0, 0x00}; return send_all(fd, p, 2); }

static void *mqtt_thread(void *arg) {
    char cid[64]; snprintf(cid, sizeof(cid), "%s-%d", APP_NAME, (int)getpid());

    while (g_running) {
        char host[256]; int port; char qt[256], rt[256]; char user[256], pass[256];
        pthread_mutex_lock(&g_mu);
        g_strlcpy(host, g_mqtt_host, sizeof(host)); port = g_mqtt_port;
        g_strlcpy(user, g_mqtt_user, sizeof(user)); g_strlcpy(pass, g_mqtt_pass, sizeof(pass));
        g_strlcpy(qt, g_queue_topic, sizeof(qt)); g_strlcpy(rt, g_ready_topic, sizeof(rt));
        g_reconnect = 0;
        pthread_mutex_unlock(&g_mu);

        struct addrinfo hints = {0}, *ai;
        hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
        char port_s[8]; snprintf(port_s, sizeof(port_s), "%d", port);
        if (getaddrinfo(host, port_s, &hints, &ai) != 0) {
            LOG_WARN("MQTT: cannot resolve %s", host); sleep(5); continue;
        }
        int fd = socket(ai->ai_family, ai->ai_socktype, 0);
        int ok = (fd >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen) == 0);
        freeaddrinfo(ai);
        if (!ok) { if (fd >= 0) close(fd); LOG_WARN("MQTT: connect: %s", strerror(errno)); sleep(5); continue; }

        struct timeval tv = {65, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (mqtt_connect_pkt(fd, cid, user, pass) < 0) { close(fd); sleep(5); continue; }

        uint8_t hdr; uint32_t rem; uint8_t ca[2];
        if (recv(fd, &hdr, 1, MSG_WAITALL) != 1 || hdr != 0x20 ||
            read_rl(fd, &rem) < 0 || rem < 2 || recv_exact(fd, ca, 2) < 0 || ca[1] != 0) {
            LOG_WARN("MQTT: CONNACK failed"); close(fd); sleep(5); continue;
        }
        LOG("MQTT: connected to %s:%d", host, port);
        mqtt_subscribe_pkt(fd, qt, 1);
        mqtt_subscribe_pkt(fd, rt, 2);

        time_t last_ping = time(NULL);
        int alive = 1;
        while (g_running && alive) {
            pthread_mutex_lock(&g_mu); int changed = g_reconnect; pthread_mutex_unlock(&g_mu);
            if (changed) break;

            if (time(NULL) - last_ping >= 55) {
                if (mqtt_pingreq(fd) < 0) { alive = 0; break; }
                last_ping = time(NULL);
            }

            uint8_t ph; int n = recv(fd, &ph, 1, 0);
            if (n == 0) { alive = 0; break; }
            if (n < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) continue; alive = 0; break; }

            uint32_t plen; if (read_rl(fd, &plen) < 0) { alive = 0; break; }

            if ((ph & 0xF0) == 0x30) { /* PUBLISH QoS 0 */
                if (plen < 2) { skip_bytes(fd, plen); continue; }
                uint8_t tl[2]; if (recv_exact(fd, tl, 2) < 0) { alive = 0; break; }
                uint16_t tlen = ((uint16_t)tl[0] << 8) | tl[1];
                if ((uint32_t)(tlen + 2) > plen) { skip_bytes(fd, plen - 2); continue; }
                char *topic = g_malloc(tlen + 1);
                if (recv_exact(fd, topic, tlen) < 0) { g_free(topic); alive = 0; break; }
                topic[tlen] = '\0';
                uint32_t pay_len = plen - 2 - tlen;
                char *payload = g_malloc(pay_len + 1);
                if (recv_exact(fd, payload, pay_len) < 0) { g_free(topic); g_free(payload); alive = 0; break; }
                payload[pay_len] = '\0';
                Msg *m = g_new(Msg, 1); m->topic = topic; m->payload = payload;
                g_main_context_invoke(NULL, dispatch, m);
            } else {
                if (skip_bytes(fd, plen) < 0) { alive = 0; break; }
            }
        }
        close(fd);
        if (g_running) { LOG("MQTT: disconnected, retrying in 5 s..."); sleep(5); }
    }
    return NULL;
}

/* ========== Main ========== */

static GMainLoop *g_loop = NULL;
static pthread_t  g_tid;

static gboolean on_sigterm(gpointer ud) {
    LOG("SIGTERM - shutting down"); g_running = 0;
    if (g_loop) g_main_loop_quit(g_loop); return G_SOURCE_REMOVE;
}

int main(void) {
    openlog(APP_NAME, LOG_PID | LOG_CONS, LOG_USER);
    LOG("--- %s starting ---", APP_NAME);
    curl_global_init(CURL_GLOBAL_ALL);
    settings_init();
    vapix_get_credentials();
    g_loop = g_main_loop_new(NULL, FALSE);
    GSource *sig = g_unix_signal_source_new(SIGTERM);
    g_source_set_callback(sig, on_sigterm, NULL, NULL);
    g_source_attach(sig, NULL); g_source_unref(sig);
    g_running = 1;
    pthread_create(&g_tid, NULL, mqtt_thread, NULL);
    g_main_loop_run(g_loop);
    g_running = 0;
    pthread_join(g_tid, NULL);
    if (g_param) ax_parameter_free(g_param);
    curl_global_cleanup();
    g_main_loop_unref(g_loop);
    closelog();
    return 0;
}
