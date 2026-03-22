#pragma once
#include <stdint.h>
#include <glib.h>

typedef struct CaptureState CaptureState;

typedef void (*FrameCallback)(void *userdata, void *pixels,
                              int width, int height, int stride,
                              uint32_t wl_format, uint64_t timestamp_ns);

CaptureState *capture_init(void);
void capture_set_callback(CaptureState *cs, FrameCallback cb, void *userdata);
void capture_start(CaptureState *cs, GMainLoop *loop);
int  capture_width(const CaptureState *cs);
int  capture_height(const CaptureState *cs);
void capture_destroy(CaptureState *cs);
