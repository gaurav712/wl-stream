#define _GNU_SOURCE
#include "signal_srv.h"
#include "pipeline.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char HTML[] =
"<!DOCTYPE html><html><head><meta charset=utf-8><title>stream</title>"
"<style>body{margin:0;background:#000}video{width:100%;height:100vh;display:block}</style>"
"</head><body><video id=v autoplay playsinline muted></video><script>"
"(async()=>{"
"const pc=new RTCPeerConnection({iceServers:[{urls:'stun:stun.l.google.com:19302'}]});"
"pc.ontrack=e=>document.getElementById('v').srcObject=e.streams[0];"
"pc.onicecandidate=({candidate})=>candidate&&fetch('/candidate',{method:'POST',"
"headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({candidate:candidate.candidate,sdpMLineIndex:candidate.sdpMLineIndex})});"
"const offer=await(await fetch('/offer')).json();"
"await pc.setRemoteDescription({type:'offer',sdp:offer.sdp});"
"const answer=await pc.createAnswer();"
"await pc.setLocalDescription(answer);"
"await fetch('/answer',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({sdp:answer.sdp})});"
"for(const c of offer.candidates||[])pc.addIceCandidate(c).catch(()=>{});"
"})();"
"</script></body></html>";

static char *json_get_string(const char *json, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return NULL;
    p++;
    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    while (*p && !(*p == '"' && *(p-1) != '\\')) {
        if (len + 4 >= cap) { cap *= 2; out = realloc(out, cap); }
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
            case 'n': out[len++] = '\n'; break;
            case 'r': out[len++] = '\r'; break;
            default:  out[len++] = *p;  break;
            }
        } else {
            out[len++] = *p;
        }
        p++;
    }
    out[len] = '\0';
    return out;
}

static int json_get_int(const char *json, const char *key)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    return atoi(p);
}

static void send_response(int fd, int code, const char *content_type,
                           const char *body, size_t body_len)
{
    char header[256];
    const char *status = code == 200 ? "OK" : code == 204 ? "No Content" : "Bad Request";
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\n\r\n",
                     code, status, content_type, body_len);
    (void)write(fd, header, (size_t)n);
    if (body && body_len) (void)write(fd, body, body_len);
}

static void recv_request(int fd, char *buf, int bufsz, char **body_out)
{
    int total = 0;
    *body_out = NULL;
    while (total < bufsz - 1) {
        int n = (int)read(fd, buf + total, (size_t)(bufsz - total - 1));
        if (n <= 0) break;
        total += n; buf[total] = '\0';
        char *header_end = strstr(buf, "\r\n\r\n");
        if (header_end) {
            *body_out = header_end + 4;
            char *cl = strstr(buf, "Content-Length:");
            if (!cl) cl = strstr(buf, "content-length:");
            if (cl) {
                int content_len = atoi(cl + 15);
                int body_received = (int)((buf + total) - *body_out);
                while (body_received < content_len && total < bufsz - 1) {
                    int space = bufsz - total - 1;
                    int need  = content_len - body_received;
                    n = (int)read(fd, buf + total, (size_t)(need < space ? need : space));
                    if (n <= 0) break;
                    total += n; body_received += n; buf[total] = '\0';
                }
            }
            break;
        }
    }
}

static void handle_request(int fd, Pipeline *pipeline)
{
    char buf[65536];
    char *body = NULL;
    recv_request(fd, buf, (int)sizeof(buf), &body);

    char method[8] = {0}, path[256] = {0};
    sscanf(buf, "%7s %255s", method, path);

    if (strcmp(method, "OPTIONS") == 0) {
        send_response(fd, 204, "text/plain", NULL, 0);
    } else if (strcmp(path, "/") == 0) {
        send_response(fd, 200, "text/html; charset=utf-8", HTML, sizeof(HTML) - 1);
    } else if (strcmp(path, "/offer") == 0) {
        char *json = pipeline_get_offer_json(pipeline);
        send_response(fd, 200, "application/json", json, strlen(json));
        free(json);
    } else if (strcmp(path, "/answer") == 0 && body && *body) {
        char *sdp = json_get_string(body, "sdp");
        if (sdp) { pipeline_set_remote_sdp(pipeline, sdp); free(sdp); }
        send_response(fd, 204, "text/plain", NULL, 0);
    } else if (strcmp(path, "/candidate") == 0 && body && *body) {
        char *candidate = json_get_string(body, "candidate");
        if (candidate) {
            pipeline_add_candidate(pipeline, candidate,
                                   json_get_int(body, "sdpMLineIndex"));
            free(candidate);
        }
        send_response(fd, 204, "text/plain", NULL, 0);
    } else {
        send_response(fd, 400, "text/plain", "not found\n", 10);
    }
}

struct SignalServer {
    int        server_fd;
    Pipeline  *pipeline;
    pthread_t  thread;
};

static void *server_thread(void *arg)
{
    SignalServer *server = arg;
    while (1) {
        int fd = accept(server->server_fd, NULL, NULL);
        if (fd < 0) { if (errno == EINTR || errno == EBADF) break; continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        handle_request(fd, server->pipeline);
        close(fd);
    }
    return NULL;
}

SignalServer *signal_server_create(int port, Pipeline *pipeline)
{
    SignalServer *server = calloc(1, sizeof(*server));
    if (!server) return NULL;
    server->pipeline  = pipeline;
    server->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->server_fd < 0) { free(server); return NULL; }

    int opt = 1;
    setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons((uint16_t)port),
    };
    if (bind(server->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(server->server_fd, 8) < 0 ||
        pthread_create(&server->thread, NULL, server_thread, server) != 0) {
        close(server->server_fd); free(server); return NULL;
    }
    return server;
}

void signal_server_destroy(SignalServer *server)
{
    if (!server) return;
    close(server->server_fd);
    pthread_join(server->thread, NULL);
    free(server);
}
