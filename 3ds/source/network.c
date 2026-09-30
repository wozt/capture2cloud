#include "network.h"

#include <3ds.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SOC_BUFFER_SIZE (1024 * 1024)
#define VIDEO_SLOTS 3
#define VIDEO_CAPACITY (512 * 1024)
#define VIDEO_PADDING 64
#define AUDIO_SLOTS 24
#define AUDIO_CAPACITY 4096

enum { SLOT_FREE, SLOT_WRITING, SLOT_READY, SLOT_DECODING };

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t received_ms;
    uint32_t sequence;
    uint8_t codec;
    uint8_t flags;
    int state;
} VideoSlot;

typedef struct {
    uint8_t data[AUDIO_CAPACITY];
    uint32_t size;
} AudioSlot;

static uint32_t *g_soc_buffer;
static Thread g_thread;
static Thread g_audio_thread;
static volatile bool g_running;
static volatile bool g_audio_running;
static volatile bool g_wanted;
static volatile bool g_auto_reconnect;
static int g_socket = -1;
static int g_audio_socket = -1;
static LightLock g_lock;
static LightLock g_send_lock;
static AppConfig g_config;
static NetworkStats g_stats;
static VideoSlot g_video[VIDEO_SLOTS];
static uint32_t g_video_sequence;
static AudioSlot g_audio[AUDIO_SLOTS];
static int g_audio_read;
static int g_audio_count;
static uint32_t g_video_window_start;
static uint32_t g_video_window_frames;
static uint64_t g_video_window_bytes;
static uint64_t g_video_window_receive_ms;
static uint8_t g_video_codec = C2S_CODEC_OLD3DS_JPEG;
static uint32_t g_last_keyframe_request_ms;
static bool g_need_keyframe;
static volatile bool g_audio_udp_active;
static uint32_t g_audio_udp_sequence;
static bool g_audio_udp_sequence_valid;

static void set_status(NetworkState state, const char *text)
{
    LightLock_Lock(&g_lock);
    g_stats.state = state;
    snprintf(g_stats.status, sizeof(g_stats.status), "%s", text ? text : "");
    LightLock_Unlock(&g_lock);
}

static void close_socket(void)
{
    LightLock_Lock(&g_send_lock);
    if (g_socket >= 0) {
        shutdown(g_socket, SHUT_RDWR);
        close(g_socket);
        g_socket = -1;
    }
    LightLock_Unlock(&g_send_lock);
}

static int send_all_locked(const void *data, size_t size)
{
    const uint8_t *at = data;
    while (size && g_socket >= 0) {
        ssize_t sent = send(g_socket, at, size, 0);
        if (sent > 0) {
            at += sent;
            size -= (size_t)sent;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        } else {
            return 0;
        }
    }
    return size == 0;
}

static void send_message(uint8_t type, const void *payload, uint32_t size)
{
    C2sFrameHeader header = {type, 0, 0, size};
    LightLock_Lock(&g_send_lock);
    if (g_socket >= 0) {
        if (!send_all_locked(&header, sizeof(header)) ||
            (size && !send_all_locked(payload, size))) {
            shutdown(g_socket, SHUT_RDWR);
        }
    }
    LightLock_Unlock(&g_send_lock);
}

static int receive_exact(int socket_fd, void *data, size_t size)
{
    uint8_t *at = data;
    while (size && g_running && g_wanted) {
        ssize_t got = recv(socket_fd, at, size, 0);
        if (got > 0) {
            at += got;
            size -= (size_t)got;
        } else if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        } else {
            return 0;
        }
    }
    return size == 0;
}

static int resolve_host(const char *host, uint16_t port, struct sockaddr_in *address)
{
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons(port);
    if (inet_pton(AF_INET, host, &address->sin_addr) == 1) return 1;

    struct addrinfo hints;
    struct addrinfo *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char service[8];
    snprintf(service, sizeof(service), "%u", port);
    if (getaddrinfo(host, service, &hints, &result) != 0 || !result) return 0;
    memcpy(address, result->ai_addr, sizeof(*address));
    freeaddrinfo(result);
    return 1;
}

static int open_tcp(const char *host, uint16_t port)
{
    struct sockaddr_in address;
    if (!resolve_host(host, port, &address)) return -1;
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) return -1;

    /* Keep connect() blocking on 3DS.  SOCU completes the TCP handshake, but
     * its poll(POLLOUT) path does not reliably report a completed nonblocking
     * connect on retail Old 3DS firmware.  That made us wait five seconds and
     * then reset an already established connection.  This runs only on the
     * network worker, so the UI and decoder remain responsive. */
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

enum {
    LOGIN_OK = 1,
    LOGIN_CONNECT_FAILED = -1,
    LOGIN_UNAUTHORIZED = -2,
    LOGIN_LOCKED = -3,
    LOGIN_BAD_RESPONSE = -4
};

static int login_password(const AppConfig *config, char token[C2S_MAX_TOKEN_LEN + 1])
{
    token[0] = '\0';
    if (!config->password[0]) return LOGIN_OK;
    int socket_fd = open_tcp(config->host, config->web_port);
    if (socket_fd < 0) return LOGIN_CONNECT_FAILED;
    LightLock_Lock(&g_send_lock);
    g_socket = socket_fd;
    LightLock_Unlock(&g_send_lock);

    char request[512];
    int length = snprintf(request, sizeof(request),
        "POST /login HTTP/1.1\r\nHost: %s\r\nContent-Length: %u\r\n"
        "Connection: close\r\n\r\n%s",
        config->host, (unsigned)strlen(config->password), config->password);
    size_t sent = 0;
    while (sent < (size_t)length) {
        ssize_t amount = send(socket_fd, request + sent, (size_t)length - sent, 0);
        if (amount <= 0) {
            close_socket();
            return LOGIN_CONNECT_FAILED;
        }
        sent += (size_t)amount;
    }

    char reply[1024];
    size_t received = 0;
    while (received < sizeof(reply) - 1) {
        ssize_t amount = recv(socket_fd, reply + received,
                              sizeof(reply) - 1 - received, 0);
        if (amount <= 0) break;
        received += (size_t)amount;
    }
    close_socket();
    reply[received] = '\0';
    int http_status = 0;
    if (sscanf(reply, "HTTP/1.1 %d", &http_status) != 1) return LOGIN_BAD_RESPONSE;
    if (http_status == 401) return LOGIN_UNAUTHORIZED;
    if (http_status == 429) return LOGIN_LOCKED;
    if (http_status != 200) return LOGIN_BAD_RESPONSE;
    char *body = strstr(reply, "\r\n\r\n");
    if (!body) return LOGIN_BAD_RESPONSE;
    body += 4;
    body[strcspn(body, "\r\n")] = '\0';
    if (!body[0] || strlen(body) > C2S_MAX_TOKEN_LEN) return LOGIN_BAD_RESPONSE;
    snprintf(token, C2S_MAX_TOKEN_LEN + 1, "%s", body);
    return LOGIN_OK;
}

static int reserve_video_slot(uint8_t incoming_codec)
{
    int chosen = -1;
    uint32_t oldest = UINT32_MAX;
    LightLock_Lock(&g_lock);
    for (int i = 0; i < VIDEO_SLOTS; i++) {
        if (g_video[i].state == SLOT_FREE) {
            chosen = i;
            break;
        }
        if (g_video[i].state == SLOT_READY && g_video[i].sequence < oldest) {
            chosen = i;
            oldest = g_video[i].sequence;
        }
    }
    if (chosen >= 0) {
        if (g_video[chosen].state == SLOT_READY) {
            if (g_video[chosen].codec == C2S_CODEC_OLD3DS_MPEG4) {
                g_need_keyframe = true;
            }
            g_stats.video_dropped++;
        }
        g_video[chosen].state = SLOT_WRITING;
    } else {
        if (incoming_codec == C2S_CODEC_OLD3DS_MPEG4) g_need_keyframe = true;
        g_stats.video_dropped++;
    }
    LightLock_Unlock(&g_lock);
    return chosen;
}

static void publish_video(int slot, uint32_t size, uint8_t codec, uint8_t flags)
{
    LightLock_Lock(&g_lock);
    g_video[slot].size = size;
    g_video[slot].received_ms = osGetTime();
    g_video[slot].sequence = ++g_video_sequence;
    g_video[slot].codec = codec;
    g_video[slot].flags = flags;
    g_video[slot].state = SLOT_READY;
    g_stats.last_receive_ms = g_video[slot].received_ms;
    LightLock_Unlock(&g_lock);
}

static void note_video_received(uint32_t size, uint32_t receive_ms)
{
    const uint32_t now = osGetTime();
    LightLock_Lock(&g_lock);
    g_stats.video_received++;
    g_video_window_frames++;
    g_video_window_bytes += size;
    g_video_window_receive_ms += receive_ms;
    if (!g_video_window_start) g_video_window_start = now;
    const uint32_t elapsed = now - g_video_window_start;
    if (elapsed >= 1000) {
        const float seconds = elapsed / 1000.0f;
        g_stats.video_fps = g_video_window_frames / seconds;
        g_stats.video_kbps = (float)(g_video_window_bytes * 8u) / elapsed;
        g_stats.receive_ms = g_video_window_frames
            ? (float)g_video_window_receive_ms / g_video_window_frames : 0.0f;
        g_video_window_start = now;
        g_video_window_frames = 0;
        g_video_window_bytes = 0;
        g_video_window_receive_ms = 0;
    }
    LightLock_Unlock(&g_lock);
}

static void queue_audio(const uint8_t *data, uint32_t size)
{
    if (size > AUDIO_CAPACITY) return;
    LightLock_Lock(&g_lock);
    if (g_audio_count == AUDIO_SLOTS) {
        g_audio_read = (g_audio_read + 1) % AUDIO_SLOTS;
        g_audio_count--;
        g_stats.audio_dropped++;
    }
    int write = (g_audio_read + g_audio_count) % AUDIO_SLOTS;
    memcpy(g_audio[write].data, data, size);
    g_audio[write].size = size;
    g_audio_count++;
    LightLock_Unlock(&g_lock);
}

static void audio_udp_thread(void *unused)
{
    (void)unused;
    uint8_t packet[sizeof(C2sOpusUdpHeader) + C2S_OPUS_UDP_MAX_BYTES];
    while (g_audio_running) {
        ssize_t got = recv(g_audio_socket, packet, sizeof(packet), 0);
        if (got < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                svcSleepThread(1000000LL);
                continue;
            }
            break;
        }
        if ((size_t)got < sizeof(C2sOpusUdpHeader)) continue;
        C2sOpusUdpHeader header;
        memcpy(&header, packet, sizeof(header));
        const uint32_t magic = c2s_le32(header.magic);
        const uint32_t sequence = c2s_le32(header.sequence);
        const uint16_t bytes = c2s_le16(header.bytes);
        if (magic != C2S_OPUS_UDP_MAGIC || bytes == 0 ||
            bytes > C2S_OPUS_UDP_MAX_BYTES ||
            (size_t)got != sizeof(header) + bytes || !g_audio_udp_active) {
            continue;
        }
        if (g_audio_udp_sequence_valid && sequence > g_audio_udp_sequence + 1) {
            LightLock_Lock(&g_lock);
            g_stats.audio_dropped += sequence - g_audio_udp_sequence - 1;
            LightLock_Unlock(&g_lock);
        }
        g_audio_udp_sequence = sequence;
        g_audio_udp_sequence_valid = true;
        queue_audio(packet + sizeof(header), bytes);
    }
}

static int discard_payload(int socket_fd, uint32_t size)
{
    uint8_t scratch[1024];
    while (size) {
        uint32_t chunk = size > sizeof(scratch) ? sizeof(scratch) : size;
        if (!receive_exact(socket_fd, scratch, chunk)) return 0;
        size -= chunk;
    }
    return 1;
}

static int stream_session(const AppConfig *config)
{
    char token[C2S_MAX_TOKEN_LEN + 1];
    const int login = login_password(config, token);
    if (login != LOGIN_OK) {
        char error[96];
        if (login == LOGIN_CONNECT_FAILED) {
            snprintf(error, sizeof(error), "No web server %s:%u",
                     config->host, config->web_port);
        } else if (login == LOGIN_UNAUTHORIZED) {
            snprintf(error, sizeof(error), "Wrong password");
        } else if (login == LOGIN_LOCKED) {
            snprintf(error, sizeof(error), "Login locked - retry in 30s");
        } else {
            snprintf(error, sizeof(error), "Invalid login reply %s:%u",
                     config->host, config->web_port);
        }
        set_status(NETWORK_ERROR, error);
        return 0;
    }

    int socket_fd = open_tcp(config->host, config->stream_port);
    if (socket_fd < 0) {
        char error[96];
        snprintf(error, sizeof(error), "No stream server %s:%u",
                 config->host, config->stream_port);
        set_status(NETWORK_ERROR, error);
        return 0;
    }
    LightLock_Lock(&g_send_lock);
    g_socket = socket_fd;
    LightLock_Unlock(&g_send_lock);

    C2sHello hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = C2S_MAGIC;
    hello.version = C2S_VERSION;
    hello.token_len = (uint8_t)strlen(token);
    uint16_t capabilities = C2S_HELLO_CAP_OLD3DS;
    if (g_audio_socket >= 0) capabilities |= C2S_HELLO_CAP_OPUS_UDP;
    hello.reserved = c2s_le16(capabilities);
    LightLock_Lock(&g_send_lock);
    int hello_ok = send_all_locked(&hello, sizeof(hello)) &&
                   (!hello.token_len || send_all_locked(token, hello.token_len));
    LightLock_Unlock(&g_send_lock);
    C2sHelloAck ack;
    if (!hello_ok || !receive_exact(socket_fd, &ack, sizeof(ack)) || !ack.accepted ||
        ack.magic != C2S_MAGIC || ack.version != C2S_VERSION ||
        (ack.video_codec != C2S_CODEC_OLD3DS_JPEG &&
         ack.video_codec != C2S_CODEC_OLD3DS_MPEG4)) {
        set_status(NETWORK_ERROR, "Server refused the OLD3DS profile");
        close_socket();
        return 0;
    }

    LightLock_Lock(&g_lock);
    g_stats.state = NETWORK_CONNECTED;
    g_stats.may_control = ack.may_control != 0;
    snprintf(g_stats.status, sizeof(g_stats.status), "Connected%s",
             ack.may_control ? " (player)" : " (viewer)");
    g_video_codec = ack.video_codec;
    g_stats.video_codec = ack.video_codec;
    LightLock_Unlock(&g_lock);

    g_audio_udp_active =
        ack.audio_codec == C2S_CODEC_OPUS &&
        (ack.reserved & C2S_ACK_FLAG_OPUS_UDP) != 0;
    g_audio_udp_sequence_valid = false;

    if (config->video_codec != ack.video_codec) {
        send_message(C2S_MSG_CODEC, &config->video_codec, 1);
    }

    C2sProfile profile = {400, 240, 30, 1200};
    send_message(C2S_MSG_PROFILE, &profile, sizeof(profile));

    while (g_running && g_wanted) {
        C2sFrameHeader header;
        if (!receive_exact(socket_fd, &header, sizeof(header))) break;
        if (header.size > C2S_MAX_PAYLOAD) break;
        LightLock_Lock(&g_lock);
        g_stats.bytes_received += sizeof(header) + header.size;
        LightLock_Unlock(&g_lock);

        if (header.type == C2S_MSG_PING && header.size == 0) {
            send_message(C2S_MSG_PING, NULL, 0);
        } else if (header.type == C2S_MSG_VIDEO) {
            const uint32_t receive_start = osGetTime();
            const uint8_t frame_codec = (header.flags & C2S_FLAG_OLD3DS_MPEG4)
                ? C2S_CODEC_OLD3DS_MPEG4 : C2S_CODEC_OLD3DS_JPEG;
            if (header.size > VIDEO_CAPACITY) {
                LightLock_Lock(&g_lock);
                if (frame_codec == C2S_CODEC_OLD3DS_MPEG4) {
                    g_need_keyframe = true;
                }
                g_stats.video_dropped++;
                LightLock_Unlock(&g_lock);
                if (!discard_payload(socket_fd, header.size)) break;
                note_video_received(header.size, osGetTime() - receive_start);
                continue;
            }
            int slot = reserve_video_slot(frame_codec);
            if (slot < 0) {
                if (!discard_payload(socket_fd, header.size)) break;
                note_video_received(header.size, osGetTime() - receive_start);
                continue;
            }
            if (!receive_exact(socket_fd, g_video[slot].data, header.size)) {
                LightLock_Lock(&g_lock);
                g_video[slot].state = SLOT_FREE;
                LightLock_Unlock(&g_lock);
                break;
            }
            memset(g_video[slot].data + header.size, 0, VIDEO_PADDING);
            note_video_received(header.size, osGetTime() - receive_start);
            publish_video(slot, header.size, frame_codec, header.flags);
        } else if (header.type == C2S_MSG_AUDIO && header.size <= AUDIO_CAPACITY) {
            uint8_t packet[AUDIO_CAPACITY];
            if (!receive_exact(socket_fd, packet, header.size)) break;
            LightLock_Lock(&g_lock);
            const bool audio_enabled = g_config.audio_enabled;
            LightLock_Unlock(&g_lock);
            if (audio_enabled && !g_audio_udp_active) queue_audio(packet, header.size);
        } else if (header.type == C2S_MSG_STREAM_INFO &&
                   header.size == sizeof(C2sStreamInfo)) {
            C2sStreamInfo info;
            if (!receive_exact(socket_fd, &info, sizeof(info))) break;
            if (info.video_codec == C2S_CODEC_OLD3DS_JPEG ||
                info.video_codec == C2S_CODEC_OLD3DS_MPEG4) {
                LightLock_Lock(&g_lock);
                g_video_codec = info.video_codec;
                g_stats.video_codec = info.video_codec;
                /* Frames decoded under the old codec are stale by
                 * definition once the switch marker arrives. */
                for (int i = 0; i < VIDEO_SLOTS; i++) {
                    if (g_video[i].state == SLOT_READY) {
                        g_video[i].state = SLOT_FREE;
                        g_stats.video_dropped++;
                    }
                }
                LightLock_Unlock(&g_lock);
            }
        } else {
            if (!discard_payload(socket_fd, header.size)) break;
        }
    }

    close_socket();
    g_audio_udp_active = false;
    if (g_wanted) set_status(NETWORK_ERROR, "Connection lost");
    else set_status(NETWORK_DISCONNECTED, "Disconnected");
    return 1;
}

static void network_thread(void *unused)
{
    (void)unused;
    while (g_running) {
        if (!g_wanted) {
            svcSleepThread(50000000LL);
            continue;
        }
        AppConfig config;
        LightLock_Lock(&g_lock);
        config = g_config;
        LightLock_Unlock(&g_lock);
        set_status(NETWORK_CONNECTING, "Connecting...");
        stream_session(&config);
        if (!g_running || !g_wanted) continue;
        if (!g_auto_reconnect) {
            g_wanted = false;
            continue;
        }
        LightLock_Lock(&g_lock);
        g_stats.reconnects++;
        LightLock_Unlock(&g_lock);
        for (int i = 0; i < 20 && g_running && g_wanted; i++) {
            svcSleepThread(50000000LL);
        }
    }
}

bool network_init(void)
{
    LightLock_Init(&g_lock);
    LightLock_Init(&g_send_lock);
    g_soc_buffer = memalign(0x1000, SOC_BUFFER_SIZE);
    if (!g_soc_buffer || R_FAILED(socInit(g_soc_buffer, SOC_BUFFER_SIZE))) {
        free(g_soc_buffer);
        g_soc_buffer = NULL;
        return false;
    }

    g_audio_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_audio_socket >= 0) {
        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(C2S_AUDIO_UDP_PORT);
        int flags = fcntl(g_audio_socket, F_GETFL, 0);
        if (bind(g_audio_socket, (struct sockaddr *)&address, sizeof(address)) != 0 ||
            flags < 0 || fcntl(g_audio_socket, F_SETFL, flags | O_NONBLOCK) != 0) {
            close(g_audio_socket);
            g_audio_socket = -1;
        }
    }
    for (int i = 0; i < VIDEO_SLOTS; i++) {
        g_video[i].data = malloc(VIDEO_CAPACITY + VIDEO_PADDING);
        if (!g_video[i].data) {
            network_exit();
            return false;
        }
    }
    set_status(NETWORK_DISCONNECTED, "Disconnected");
    g_running = true;
    if (g_audio_socket >= 0) {
        g_audio_running = true;
        g_audio_thread = threadCreate(audio_udp_thread, NULL, 16 * 1024,
                                      0x31, -2, false);
        if (!g_audio_thread) {
            g_audio_running = false;
            close(g_audio_socket);
            g_audio_socket = -1;
        }
    }
    g_thread = threadCreate(network_thread, NULL, 32 * 1024, 0x30, -2, false);
    if (!g_thread) {
        network_exit();
        return false;
    }
    return true;
}

void network_exit(void)
{
    g_running = false;
    g_wanted = false;
    g_audio_udp_active = false;
    close_socket();
    if (g_thread) {
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = NULL;
    }
    g_audio_running = false;
    if (g_audio_socket >= 0) {
        close(g_audio_socket);
        g_audio_socket = -1;
    }
    if (g_audio_thread) {
        threadJoin(g_audio_thread, U64_MAX);
        threadFree(g_audio_thread);
        g_audio_thread = NULL;
    }
    for (int i = 0; i < VIDEO_SLOTS; i++) {
        free(g_video[i].data);
        g_video[i].data = NULL;
    }
    if (g_soc_buffer) {
        socExit();
        free(g_soc_buffer);
        g_soc_buffer = NULL;
    }
}

void network_apply_config(const AppConfig *config)
{
    LightLock_Lock(&g_lock);
    g_config = *config;
    g_auto_reconnect = config->auto_connect;
    LightLock_Unlock(&g_lock);
}

void network_connect(void) { g_wanted = true; }

void network_disconnect(void)
{
    g_wanted = false;
    close_socket();
}

void network_reconnect(void)
{
    g_wanted = false;
    close_socket();
    svcSleepThread(100000000LL);
    g_wanted = true;
}

void network_set_auto_reconnect(bool enabled) { g_auto_reconnect = enabled; }

void network_get_stats(NetworkStats *stats)
{
    LightLock_Lock(&g_lock);
    *stats = g_stats;
    uint32_t depth = 0;
    for (int i = 0; i < VIDEO_SLOTS; i++) {
        if (g_video[i].state == SLOT_READY) depth++;
    }
    stats->queue_depth = depth;
    LightLock_Unlock(&g_lock);
}

bool network_acquire_video(const uint8_t **data, uint32_t *size,
                           uint32_t *received_ms, uint8_t *codec, int *slot)
{
    int newest = -1;
    uint32_t sequence = 0;
    bool dropped_predictive = false;
    bool request_keyframe = false;
    LightLock_Lock(&g_lock);
    for (int i = 0; i < VIDEO_SLOTS; i++) {
        if (g_video[i].state == SLOT_READY) {
            if (g_video[i].sequence >= sequence) {
                if (newest >= 0) {
                    if (g_video[newest].codec == C2S_CODEC_OLD3DS_MPEG4) {
                        dropped_predictive = true;
                    }
                    g_video[newest].state = SLOT_FREE;
                    g_stats.video_dropped++;
                }
                newest = i;
                sequence = g_video[i].sequence;
            } else {
                if (g_video[i].codec == C2S_CODEC_OLD3DS_MPEG4) {
                    dropped_predictive = true;
                }
                g_video[i].state = SLOT_FREE;
                g_stats.video_dropped++;
            }
        }
    }
    if (newest >= 0) {
        g_video[newest].state = SLOT_DECODING;
        *data = g_video[newest].data;
        *size = g_video[newest].size;
        *received_ms = g_video[newest].received_ms;
        *codec = g_video[newest].codec;
        *slot = newest;
        if (g_video[newest].codec == C2S_CODEC_OLD3DS_MPEG4) {
            if (g_video[newest].flags & C2S_FLAG_KEYFRAME) {
                g_need_keyframe = false;
            } else if (dropped_predictive || g_need_keyframe) {
                g_need_keyframe = true;
                request_keyframe = true;
            }
        }
    }
    LightLock_Unlock(&g_lock);
    if (request_keyframe) network_request_keyframe();
    return newest >= 0;
}

void network_release_video(int slot)
{
    if (slot < 0 || slot >= VIDEO_SLOTS) return;
    LightLock_Lock(&g_lock);
    if (g_video[slot].state == SLOT_DECODING) g_video[slot].state = SLOT_FREE;
    LightLock_Unlock(&g_lock);
}

bool network_take_audio(uint8_t *data, uint32_t capacity, uint32_t *size)
{
    bool result = false;
    LightLock_Lock(&g_lock);
    if (g_audio_count > 0 && g_audio[g_audio_read].size <= capacity) {
        *size = g_audio[g_audio_read].size;
        memcpy(data, g_audio[g_audio_read].data, *size);
        g_audio_read = (g_audio_read + 1) % AUDIO_SLOTS;
        g_audio_count--;
        result = true;
    }
    LightLock_Unlock(&g_lock);
    return result;
}

void network_send_input(const int8_t state[C2S_PAD_SLOTS])
{
    send_message(C2S_MSG_INPUT, state, C2S_PAD_SLOTS);
}

void network_send_home(void) { send_message(C2S_MSG_HOME, NULL, 0); }
void network_send_capture(void) { send_message(C2S_MSG_CAPTURE, NULL, 0); }

void network_request_codec(uint8_t codec)
{
    if (codec != C2S_CODEC_OLD3DS_JPEG &&
        codec != C2S_CODEC_OLD3DS_MPEG4) return;
    LightLock_Lock(&g_lock);
    g_config.video_codec = codec;
    const bool connected = g_stats.state == NETWORK_CONNECTED;
    LightLock_Unlock(&g_lock);
    if (connected) send_message(C2S_MSG_CODEC, &codec, 1);
}

void network_request_keyframe(void)
{
    const uint32_t now = osGetTime();
    LightLock_Lock(&g_lock);
    const bool send_now = g_stats.state == NETWORK_CONNECTED &&
        (!g_last_keyframe_request_ms ||
         now - g_last_keyframe_request_ms >= 1000);
    if (send_now) g_last_keyframe_request_ms = now;
    LightLock_Unlock(&g_lock);
    if (send_now) send_message(C2S_MSG_KEYFRAME, NULL, 0);
}
