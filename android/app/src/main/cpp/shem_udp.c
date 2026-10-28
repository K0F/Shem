/* =============================================================================
 * shem_udp.c - Multi-User Peer-to-Peer Live Coding Sync for Shem
 * Author: Jáchym Pešek
 * =============================================================================
 */

#include "shem_udp.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TAG "ShemUDP"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define QUEUE_SIZE 32
#define MSG_BUF_SIZE 1024

static int g_sock = -1;
static int g_port = 10000;
static char g_username[64] = "kof";
static pthread_t g_rx_thread;
static bool g_running = false;

static char g_rx_queue[QUEUE_SIZE][MSG_BUF_SIZE];
static int g_queue_head = 0;
static int g_queue_tail = 0;
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *rx_worker(void *arg) {
    (void)arg;
    char buf[MSG_BUF_SIZE];
    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);

    while (g_running) {
        ssize_t n = recvfrom(g_sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src_addr, &addr_len);
        if (n > 0) {
            buf[n] = '\0';
            pthread_mutex_lock(&g_queue_mutex);
            int next_head = (g_queue_head + 1) % QUEUE_SIZE;
            if (next_head != g_queue_tail) {
                strncpy(g_rx_queue[g_queue_head], buf, MSG_BUF_SIZE - 1);
                g_rx_queue[g_queue_head][MSG_BUF_SIZE - 1] = '\0';
                g_queue_head = next_head;
            }
            pthread_mutex_unlock(&g_queue_mutex);
        } else {
            usleep(20000); // 20ms
        }
    }
    return NULL;
}

bool shem_udp_init(const char *username, int port) {
    if (username && strlen(username) > 0) {
        strncpy(g_username, username, sizeof(g_username) - 1);
    }
    g_port = (port > 0) ? port : 10000;

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) {
        LOGE("Failed to create UDP socket: %s", strerror(errno));
        return false;
    }

    int broadcast = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    int reuse = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // Non-blocking
    int flags = fcntl(g_sock, F_GETFL, 0);
    fcntl(g_sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(g_port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(g_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        LOGE("UDP bind failed on port %d: %s", g_port, strerror(errno));
        close(g_sock);
        g_sock = -1;
        return false;
    }

    g_running = true;
    pthread_create(&g_rx_thread, NULL, rx_worker, NULL);

    LOGI("Shem UDPServer initialized for '%s' on port %d", g_username, g_port);
    return true;
}

void shem_udp_broadcast_code(const char *code) {
    if (g_sock < 0 || !code) return;

    char msg[MSG_BUF_SIZE];
    snprintf(msg, sizeof(msg), "[%s: %s]", g_username, code);

    struct sockaddr_in bcast_addr;
    memset(&bcast_addr, 0, sizeof(bcast_addr));
    bcast_addr.sin_family = AF_INET;
    bcast_addr.sin_port = htons(g_port);
    bcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    sendto(g_sock, msg, strlen(msg), 0, (struct sockaddr *)&bcast_addr, sizeof(bcast_addr));
}

bool shem_udp_pop_incoming(char *out_msg, int max_len) {
    if (!out_msg || max_len <= 0) return false;
    bool found = false;

    pthread_mutex_lock(&g_queue_mutex);
    if (g_queue_tail != g_queue_head) {
        strncpy(out_msg, g_rx_queue[g_queue_tail], max_len - 1);
        out_msg[max_len - 1] = '\0';
        g_queue_tail = (g_queue_tail + 1) % QUEUE_SIZE;
        found = true;
    }
    pthread_mutex_unlock(&g_queue_mutex);

    return found;
}

void shem_udp_close(void) {
    g_running = false;
    if (g_sock >= 0) {
        pthread_join(g_rx_thread, NULL);
        close(g_sock);
        g_sock = -1;
    }
}
