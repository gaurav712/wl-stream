#pragma once
#include "pipeline.h"

typedef struct SignalServer SignalServer;

SignalServer *signal_server_create(int port, Pipeline *pipeline);
void signal_server_destroy(SignalServer *s);
