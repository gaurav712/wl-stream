#define _GNU_SOURCE
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib-unix.h>
#include <gst/gst.h>

#include "audio.h"
#include "capture.h"
#include "pipeline.h"
#include "signal_srv.h"

static GMainLoop *g_loop;

static gboolean on_signal(gpointer ud)
{
    (void)ud;
    g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
}

static void on_frame(void *ud, void *pixels, int w, int h, int stride,
                     uint32_t fmt, uint64_t ts)
{
    pipeline_push_frame(ud, pixels, w, h, stride, fmt, ts);
}

int main(int argc, char **argv)
{
    int         port    = 8080;
    int         bitrate = 8000;
    bool        use_vp9   = false;
    bool        no_audio  = false;
    const char *monitor_override = NULL;

    static const struct option opts[] = {
        { "port",     required_argument, NULL, 'p' },
        { "bitrate",  required_argument, NULL, 'b' },
        { "codec",    required_argument, NULL, 'c' },
        { "monitor",  required_argument, NULL, 'm' },
        { "no-audio", no_argument,       NULL, 'n' },
        { NULL, 0, NULL, 0 }
    };

    int o;
    while ((o = getopt_long(argc, argv, "p:b:c:m:nv", opts, NULL)) != -1) {
        switch (o) {
        case 'p': port    = atoi(optarg);                         break;
        case 'b': bitrate = atoi(optarg);                         break;
        case 'c': use_vp9 = strcmp(optarg, "vp9") == 0;          break;
        case 'm': monitor_override = optarg;                          break;
        case 'n': no_audio = true;                                break;
        case 'v': setenv("GST_DEBUG", "3", 1);                   break;
        default:
            fprintf(stderr, "Usage: %s [-p PORT] [-b KBPS] [--codec h264|vp9] "
                    "[-m MONITOR] [--no-audio] [-v]\n", argv[0]);
            return 1;
        }
    }

    gst_init(&argc, &argv);
    g_loop = g_main_loop_new(NULL, FALSE);

    CaptureState *cap = capture_init();
    if (!cap) { fprintf(stderr, "capture_init failed\n"); return 1; }

    char *monitor = NULL;
    if (!no_audio)
        monitor = monitor_override ? strdup(monitor_override) : audio_get_default_monitor();

    Pipeline *pipeline = pipeline_create(capture_width(cap), capture_height(cap),
                                          monitor, bitrate, use_vp9);
    if (!pipeline) { capture_destroy(cap); free(monitor); return 1; }

    SignalServer *server = signal_server_create(port, pipeline);
    if (!server) {
        fprintf(stderr, "Failed to start server on port %d (already in use?)\n", port);
        pipeline_destroy(pipeline); capture_destroy(cap); free(monitor); return 1;
    }

    capture_set_callback(cap, on_frame, pipeline);
    capture_start(cap, g_loop);
    pipeline_start(pipeline);

    g_unix_signal_add(SIGINT,  on_signal, NULL);
    g_unix_signal_add(SIGTERM, on_signal, NULL);

    fprintf(stdout, "Open http://localhost:%d\n", port);
    fflush(stdout);

    g_main_loop_run(g_loop);

    capture_destroy(cap);
    signal_server_destroy(server);
    pipeline_destroy(pipeline);
    free(monitor);
    g_main_loop_unref(g_loop);
    return 0;
}
