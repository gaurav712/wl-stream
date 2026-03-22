#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct Pipeline Pipeline;

/*
 * Create a GStreamer pipeline.
 *   w, h        — video dimensions (from capture)
 *   monitor     — PulseAudio sink monitor device string (may be NULL for no audio)
 *   bitrate     — video bitrate in kbps
 *   use_vp9     — true → vavp9enc; false → vah264lpenc (with openh264/x264 fallback)
 */
Pipeline *pipeline_create(int w, int h, const char *monitor,
                           int bitrate, bool use_vp9);

/* Push a captured frame.  pixels must stay valid until this returns.
 * wl_format is the WL_SHM_FORMAT_* value from the compositor. */
void pipeline_push_frame(Pipeline *p, const void *pixels,
                          int width, int height, int stride,
                          uint32_t wl_format, uint64_t pts_ns);

/* Set the pipeline to PLAYING (call after signal server is up). */
void pipeline_start(Pipeline *p);

/*
 * Block until the SDP offer is ready (webrtcbin fires on-negotiation-needed).
 * Returns a heap-allocated JSON string: {"type":"offer","sdp":"..."}
 * Caller must free().
 */
char *pipeline_get_offer_json(Pipeline *p);

/* Feed the browser's SDP answer into webrtcbin. */
void pipeline_set_remote_sdp(Pipeline *p, const char *sdp_answer);

/* Feed an ICE candidate received from the browser. */
void pipeline_add_candidate(Pipeline *p, const char *candidate,
                              int sdp_mline_index);

void pipeline_destroy(Pipeline *p);
