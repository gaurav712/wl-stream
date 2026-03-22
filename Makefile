CC       = gcc
CFLAGS   = -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter \
           -Ibuild/protocols \
           $(shell pkg-config --cflags gstreamer-1.0 gstreamer-webrtc-1.0 \
             gstreamer-app-1.0 gstreamer-video-1.0 wayland-client libpulse)
LIBS     = $(shell pkg-config --libs gstreamer-1.0 gstreamer-webrtc-1.0 \
             gstreamer-app-1.0 gstreamer-video-1.0 wayland-client libpulse) -lm

PROTO_XML = protocols/wlr-screencopy-unstable-v1.xml
PROTO_H   = build/protocols/wlr-screencopy-unstable-v1-client-protocol.h
PROTO_C   = build/protocols/wlr-screencopy-unstable-v1-protocol.c

SRCS = src/main.c src/capture.c src/audio.c src/pipeline.c src/signal_srv.c $(PROTO_C)

.PHONY: all clean

all: build/stream

build/stream: $(SRCS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(PROTO_H): $(PROTO_XML)
	@mkdir -p build/protocols
	wayland-scanner client-header $< $@

$(PROTO_C): $(PROTO_XML) $(PROTO_H)
	wayland-scanner private-code $< $@

src/capture.c: $(PROTO_H)

clean:
	rm -rf build
