#define GST_USE_UNSTABLE_API
#include "pipeline.h"
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/webrtc/webrtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct IceCandidate { char *candidate; int mline; };

struct Pipeline {
    GstElement  *pipeline;
    GstAppSrc   *appsrc;
    GstElement  *webrtcbin;
    GMutex       mutex;
    GCond        sdp_cond;
    char        *offer_json;
    struct IceCandidate *candidates;
    int          num_candidates;
    bool         caps_set;
};

/* WL_SHM_FORMAT → GStreamer video/x-raw format.
 * On little-endian x86 the component order in memory is reversed vs the
 * big-endian Wayland naming: XRGB8888 → bytes [B,G,R,X] → BGRx. */
static const char *wl_fmt_to_gst(uint32_t fmt)
{
    switch (fmt) {
    case 0x00000000: return "BGRA";   /* ARGB8888 */
    case 0x00000001: return "BGRx";   /* XRGB8888 */
    case 0x34325241: return "RGBA";   /* ARGB8888 (DRM) */
    case 0x34324258: return "RGBx";   /* XBGR8888 (DRM) */
    case 0x34324241: return "BGRA";   /* ABGR8888 (DRM) */
    default:         return "BGRx";
    }
}

static void json_escape(const char *src, char *dst, size_t sz)
{
    size_t i = 0, j = 0;
    dst[j++] = '"';
    for (; src[i] && j + 4 < sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if      (c == '"')  { dst[j++] = '\\'; dst[j++] = '"';  }
        else if (c == '\\') { dst[j++] = '\\'; dst[j++] = '\\'; }
        else if (c == '\n') { dst[j++] = '\\'; dst[j++] = 'n';  }
        else if (c == '\r') { dst[j++] = '\\'; dst[j++] = 'r';  }
        else                  dst[j++] = (char)c;
    }
    dst[j++] = '"';
    dst[j]   = '\0';
}

static void build_and_signal_offer(Pipeline *p)
{
    GstWebRTCSessionDescription *desc = NULL;
    g_object_get(p->webrtcbin, "local-description", &desc, NULL);
    if (!desc) return;

    gchar *sdp = gst_sdp_message_as_text(desc->sdp);
    gst_webrtc_session_description_free(desc);

    size_t sdp_sz  = strlen(sdp) * 2 + 64;
    char  *esc_sdp = malloc(sdp_sz);
    json_escape(sdp, esc_sdp, sdp_sz);
    g_free(sdp);

    /* Build candidates array */
    g_mutex_lock(&p->mutex);
    size_t ca_sz = 64 + (size_t)p->num_candidates * 512;
    char  *ca    = malloc(ca_sz);
    size_t ci    = 0;
    ca[ci++] = '[';
    for (int i = 0; i < p->num_candidates; i++) {
        char esc[1024];
        json_escape(p->candidates[i].candidate, esc, sizeof(esc));
        int n = snprintf(ca + ci, ca_sz - ci,
                         "%s{\"candidate\":%s,\"sdpMLineIndex\":%d}",
                         i ? "," : "", esc, p->candidates[i].mline);
        if (n > 0) ci += (size_t)n;
    }
    ca[ci++] = ']'; ca[ci] = '\0';

    size_t jsz  = strlen(esc_sdp) + ci + 64;
    char  *json = malloc(jsz);
    snprintf(json, jsz, "{\"type\":\"offer\",\"sdp\":%s,\"candidates\":%s}",
             esc_sdp, ca);
    free(ca); free(esc_sdp);

    if (!p->offer_json) {
        p->offer_json = json;
        g_cond_signal(&p->sdp_cond);
    } else {
        free(json);
    }
    g_mutex_unlock(&p->mutex);
}

static void on_ice_candidate(GstElement *w G_GNUC_UNUSED, guint mline,
                              gchar *candidate, gpointer ud)
{
    Pipeline *p = ud;
    g_mutex_lock(&p->mutex);
    p->candidates = realloc(p->candidates, (size_t)(p->num_candidates + 1) * sizeof(*p->candidates));
    p->candidates[p->num_candidates].candidate = g_strdup(candidate);
    p->candidates[p->num_candidates].mline     = (int)mline;
    p->num_candidates++;
    g_mutex_unlock(&p->mutex);
}

static void on_ice_gathering_state(GstElement *webrtc,
                                    GParamSpec *ps G_GNUC_UNUSED,
                                    gpointer ud)
{
    GstWebRTCICEGatheringState st;
    g_object_get(webrtc, "ice-gathering-state", &st, NULL);
    if (st == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE)
        build_and_signal_offer(ud);
}

static void on_offer_created(GstPromise *promise, gpointer ud)
{
    Pipeline *p = ud;
    GstStructure const *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = NULL;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);
    if (!offer) return;

    GstPromise *p2 = gst_promise_new();
    g_signal_emit_by_name(p->webrtcbin, "set-local-description", offer, p2);
    gst_promise_interrupt(p2);
    gst_promise_unref(p2);
    gst_webrtc_session_description_free(offer);
}

static void on_negotiation_needed(GstElement *webrtc, gpointer ud)
{
    Pipeline *p = ud;
    GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, p, NULL);
    g_signal_emit_by_name(webrtc, "create-offer", NULL, promise);
}

static const char *pick_video_encoder(bool vp9)
{
    if (vp9) {
        if (gst_element_factory_find("vavp9enc"))    return "vavp9enc";
        if (gst_element_factory_find("vavp9lpenc"))  return "vavp9lpenc";
        if (gst_element_factory_find("vaapivp9enc")) return "vaapivp9enc";
        if (gst_element_factory_find("vp9enc"))      return "vp9enc";
    }
    if (gst_element_factory_find("vah264lpenc"))  return "vah264lpenc";
    if (gst_element_factory_find("vah264enc"))    return "vah264enc";
    if (gst_element_factory_find("vaapih264enc")) return "vaapih264enc";
    if (gst_element_factory_find("openh264enc"))  return "openh264enc";
    if (gst_element_factory_find("x264enc"))      return "x264enc";
    return NULL;
}

Pipeline *pipeline_create(int w, int h, const char *monitor,
                           int bitrate, bool use_vp9)
{
    Pipeline *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    g_mutex_init(&p->mutex);
    g_cond_init(&p->sdp_cond);

    const char *enc = pick_video_encoder(use_vp9);
    if (!enc) {
        fprintf(stderr, "No video encoder found. Install gst-plugins-bad gst-plugins-good.\n");
        free(p); return NULL;
    }

    bool is_vp9 = strstr(enc, "vp9") || strstr(enc, "VP9");

    char enc_params[256] = "";
    if      (strstr(enc, "vah264lpenc") || strstr(enc, "vah264enc"))
        snprintf(enc_params, sizeof(enc_params), "bitrate=%d target-usage=4 key-int-max=60", bitrate);
    else if (strstr(enc, "vaapih264enc"))
        snprintf(enc_params, sizeof(enc_params), "bitrate=%d quality-level=5 keyframe-period=60", bitrate);
    else if (strstr(enc, "openh264enc"))
        snprintf(enc_params, sizeof(enc_params), "bitrate=%d complexity=low", bitrate * 1000);
    else if (strstr(enc, "x264enc"))
        snprintf(enc_params, sizeof(enc_params), "bitrate=%d tune=zerolatency speed-preset=ultrafast", bitrate);
    else if (strstr(enc, "vavp9") || strstr(enc, "vaapivp9"))
        snprintf(enc_params, sizeof(enc_params), "bitrate=%d target-usage=4", bitrate);
    else if (strstr(enc, "vp9enc"))
        snprintf(enc_params, sizeof(enc_params), "target-bitrate=%d cpu-used=8 deadline=1", bitrate * 1000);

    const char *pay      = is_vp9 ? "rtpvp9pay pt=96"  : "rtph264pay config-interval=1 pt=96";
    const char *enc_name = is_vp9 ? "VP9" : "H264";
    const char *parse    = is_vp9 ? "" : "! h264parse ";

    char video[2048];
    snprintf(video, sizeof(video),
             "appsrc name=vsrc format=time is-live=true do-timestamp=true block=false "
             "! videoconvert ! %s %s %s! %s "
             "! application/x-rtp,media=video,encoding-name=%s,payload=96 ! webrtc. ",
             enc, enc_params, parse, pay, enc_name);

    char audio[512] = "";
    if (monitor)
        snprintf(audio, sizeof(audio),
                 "pulsesrc device=%s latency-time=10000 "
                 "! audio/x-raw,rate=48000,channels=2 "
                 "! audioconvert ! audioresample "
                 "! opusenc bitrate=64000 audio-type=restricted-lowdelay "
                 "! rtpopuspay pt=97 "
                 "! application/x-rtp,media=audio,encoding-name=OPUS,payload=97 ! webrtc. ",
                 monitor);

    char full[4096];
    snprintf(full, sizeof(full),
             "%s webrtcbin name=webrtc bundle-policy=max-bundle "
             "stun-server=stun://stun.l.google.com:19302 %s",
             video, audio);

    GError *err = NULL;
    p->pipeline = gst_parse_launch(full, &err);
    if (!p->pipeline || err) {
        if (err) { fprintf(stderr, "Pipeline error: %s\n", err->message); g_error_free(err); }
        pipeline_destroy(p); return NULL;
    }

    p->appsrc = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(p->pipeline), "vsrc"));
    p->webrtcbin  = gst_bin_get_by_name(GST_BIN(p->pipeline), "webrtc");

    g_signal_connect(p->webrtcbin, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), p);
    g_signal_connect(p->webrtcbin, "on-ice-candidate",      G_CALLBACK(on_ice_candidate),      p);
    g_signal_connect(p->webrtcbin, "notify::ice-gathering-state",
                     G_CALLBACK(on_ice_gathering_state), p);

    (void)w; (void)h;
    return p;
}

void pipeline_push_frame(Pipeline *p, const void *pixels,
                          int width, int height, int stride,
                          uint32_t wl_format, uint64_t pts_ns)
{
    (void)pts_ns;
    if (!p->caps_set) {
        GstCaps *caps = gst_caps_new_simple("video/x-raw",
            "format",    G_TYPE_STRING,   wl_fmt_to_gst(wl_format),
            "width",     G_TYPE_INT,      width,
            "height",    G_TYPE_INT,      height,
            "framerate", GST_TYPE_FRACTION, 0, 1,
            NULL);
        gst_app_src_set_caps(p->appsrc, caps);
        gst_caps_unref(caps);
        p->caps_set = true;
    }

    gsize row  = (gsize)width * 4;
    gsize total = row * (gsize)height;
    GstBuffer *buf = gst_buffer_new_allocate(NULL, total, NULL);
    GstMapInfo map;
    gst_buffer_map(buf, &map, GST_MAP_WRITE);
    if ((gsize)stride == row) {
        memcpy(map.data, pixels, total);
    } else {
        const uint8_t *src = pixels;
        uint8_t *dst = map.data;
        for (int r = 0; r < height; r++, dst += row, src += (gsize)stride)
            memcpy(dst, src, row);
    }
    gst_buffer_unmap(buf, &map);
    gst_app_src_push_buffer(p->appsrc, buf);
}

void pipeline_start(Pipeline *p)
{
    gst_element_set_state(p->pipeline, GST_STATE_PLAYING);
}

char *pipeline_get_offer_json(Pipeline *p)
{
    g_mutex_lock(&p->mutex);
    while (!p->offer_json) g_cond_wait(&p->sdp_cond, &p->mutex);
    char *json = g_strdup(p->offer_json);
    g_mutex_unlock(&p->mutex);
    return json;
}

void pipeline_set_remote_sdp(Pipeline *p, const char *sdp_answer)
{
    GstSDPMessage *sdp = NULL;
    if (gst_sdp_message_new(&sdp) != GST_SDP_OK ||
        gst_sdp_message_parse_buffer((const guint8 *)sdp_answer,
                                      (guint)strlen(sdp_answer), sdp) != GST_SDP_OK) {
        fprintf(stderr, "Failed to parse SDP answer\n");
        if (sdp) gst_sdp_message_free(sdp);
        return;
    }
    GstWebRTCSessionDescription *desc =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
    GstPromise *promise = gst_promise_new();
    g_signal_emit_by_name(p->webrtcbin, "set-remote-description", desc, promise);
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);
    gst_webrtc_session_description_free(desc);
}

void pipeline_add_candidate(Pipeline *p, const char *candidate, int mline)
{
    g_signal_emit_by_name(p->webrtcbin, "add-ice-candidate", (guint)mline, candidate);
}

void pipeline_destroy(Pipeline *p)
{
    if (!p) return;
    if (p->pipeline) {
        gst_element_set_state(p->pipeline, GST_STATE_NULL);
        gst_object_unref(p->pipeline);
    }
    if (p->appsrc) gst_object_unref(p->appsrc);
    if (p->webrtcbin)  gst_object_unref(p->webrtcbin);
    free(p->offer_json);
    for (int i = 0; i < p->num_candidates; i++) g_free(p->candidates[i].candidate);
    free(p->candidates);
    g_mutex_clear(&p->mutex);
    g_cond_clear(&p->sdp_cond);
    free(p);
}
