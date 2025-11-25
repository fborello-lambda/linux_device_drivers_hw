#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <stdint.h>
#include <time.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sys/epoll.h>

#include "./producer.h"

typedef struct
{
    int max_connnections;
    int backlog;
    int port;
} server_config_t;

/* Cached data protected by mutex + condvar for thread-safe access */
typedef struct
{
    mpu6050_sample_float_t current_sample;
    mpu6050_sample_float_t average;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint64_t version; /* incremented on each update */
} cached_data_t;

static cached_data_t g_cached_data = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .version = 0};

#define CONFIG_FILE "server_config.cfg"
#define DEFAULT_SERVER_CONFIG {.max_connnections = 10, .backlog = 5, .port = 3737}

static volatile sig_atomic_t running = 1;
static int sigpipe_fds[2] = {-1, -1};

server_config_t g_server_config;
shared_data_t *g_shared_data = NULL;
sem_t *g_sem = NULL;
static atomic_int g_active_connections = 0;

/* Cleanup handler to decrement the active connection counter when a
   connection handler thread exits through any code path. */
static void conn_active_dec_cleanup(void *unused)
{
    atomic_fetch_sub(&g_active_connections, 1);
}

int read_config_from_file(server_config_t *server_config)
{
    FILE *file = fopen(CONFIG_FILE, "r");
    if (!file)
    {
        perror("fopen");
        return -1;
    }

    // Read configuration values from the file
    if (fscanf(file, "max_connections=%d\n", &server_config->max_connnections) != 1 ||
        fscanf(file, "backlog=%d\n", &server_config->backlog) != 1 ||
        fscanf(file, "port=%d\n", &server_config->port) != 1)
    {
        fclose(file);
        return -2;
    }

    fclose(file);
    return 0;
}

void handle_signal(int sig)
{
    switch (sig)
    {
    case SIGUSR2:
        printf("Received SIGUSR2\n");
        read_config_from_file(&g_server_config); // Reload configuration
        printf("Configuration reloaded: max_connections=%d\n",
               g_server_config.max_connnections);
        break;
    case SIGTERM:
    case SIGINT:
        running = 0;
        if (sigpipe_fds[1] != -1)
        {
            /* wake select() */
            write(sigpipe_fds[1], "x", 1);
        }
        break;
    case SIGWINCH:
        break;
    default:
        printf("Received signal %d\n", sig);
        break;
        return;
    }
}

int init_shared_memory()
{
    // Open existing shared memory (producer should create it)
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1)
    {
        perror("shm_open - make sure producer is running first");
        return -1;
    }

    // Map the shared memory object
    // It's directly linked to the object, no need to keep shm_fd open after mmap
    g_shared_data = mmap(NULL, sizeof(shared_data_t),
                         PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (g_shared_data == MAP_FAILED)
    {
        perror("mmap");
        close(shm_fd);
        return -1;
    }

    // Open semaphore
    g_sem = sem_open(SEM_NAME, 0);
    if (g_sem == SEM_FAILED)
    {
        perror("sem_open - make sure producer is running first");
        munmap(g_shared_data, sizeof(shared_data_t));
        close(shm_fd);
        return -1;
    }

    close(shm_fd);
    return 0;
}

void cleanup_shared_memory()
{
    if (g_shared_data)
    {
        munmap(g_shared_data, sizeof(shared_data_t));
        g_shared_data = NULL;
    }
    if (g_sem)
    {
        sem_close(g_sem);
        g_sem = NULL;
    }
}

/* Data reader thread: reads from shared memory and broadcasts to waiting threads */
static void *data_reader_thread(void *arg)
{
    (void)arg;
    while (running)
    {
        if (g_shared_data && g_sem)
        {
            mpu6050_sample_float_t sample, avg;

            sem_wait(g_sem);
            sample = g_shared_data->current_sample;
            avg = g_shared_data->buffer[BUFFER_SIZE - 1];
            sem_post(g_sem);

            pthread_mutex_lock(&g_cached_data.mutex);
            g_cached_data.current_sample = sample;
            g_cached_data.average = avg;
            g_cached_data.version++;
            pthread_cond_broadcast(&g_cached_data.cond); /* wake all waiting threads */
            pthread_mutex_unlock(&g_cached_data.mutex);
        }
        usleep(REFRESH_MS * 1000); /* match producer refresh rate */
    }
    return NULL;
}

/* Helper: get cached data with mutex protection */
static void get_cached_data(mpu6050_sample_float_t *sample, mpu6050_sample_float_t *avg)
{
    pthread_mutex_lock(&g_cached_data.mutex);
    if (sample)
        *sample = g_cached_data.current_sample;
    if (avg)
        *avg = g_cached_data.average;
    pthread_mutex_unlock(&g_cached_data.mutex);
}

/* Helper: wait for new data (returns new version number) */
static uint64_t wait_for_new_data(uint64_t last_version, mpu6050_sample_float_t *sample,
                                  mpu6050_sample_float_t *avg, int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    pthread_mutex_lock(&g_cached_data.mutex);
    while (g_cached_data.version == last_version && running)
    {
        int rc = pthread_cond_timedwait(&g_cached_data.cond, &g_cached_data.mutex, &ts);
        if (rc == ETIMEDOUT)
            break;
    }
    uint64_t new_version = g_cached_data.version;
    if (sample)
        *sample = g_cached_data.current_sample;
    if (avg)
        *avg = g_cached_data.average;
    pthread_mutex_unlock(&g_cached_data.mutex);
    return new_version;
}

void *conn_handler(void *arg)
{
    pthread_cleanup_push(conn_active_dec_cleanup, NULL);
    if (!arg)
        goto out;
    int client_fd = *(int *)arg;
    free(arg);

    // Read HTTP request
    char request[1024] = {0};
    ssize_t bytes_read = recv(client_fd, request, sizeof(request) - 1, 0);
    if (bytes_read <= 0)
    {
        close(client_fd);
        goto out;
    }

    // Parse request line to get the path
    char method[16], path[256], version[16];
    if (sscanf(request, "%15s %255s %15s", method, path, version) != 3)
    {
        close(client_fd);
        goto out;
    }

    mpu6050_sample_float_t local_sample = {0}, local_avg = {0};

    /* Use cached data instead of directly accessing shared memory */
    get_cached_data(&local_sample, &local_avg);

    char body[512];
    char header[256];
    char avg_str[128];
    char sample_str[128];
    int body_len, header_len;

    snprintf(sample_str, sizeof(sample_str),
             "%f,%f,%f, [g]\n"
             "%f,%f,%f, [dps]\n"
             "%f, [°C]\n",
             local_sample.ax, local_sample.ay, local_sample.az,
             local_sample.gx, local_sample.gy, local_sample.gz,
             local_sample.temp);
    snprintf(avg_str, sizeof(avg_str),
             "%f,%f,%f, [g]\n"
             "%f,%f,%f, [dps]\n"
             "%f, [°C]\n",
             local_avg.ax, local_avg.ay, local_avg.az,
             local_avg.gx, local_avg.gy, local_avg.gz,
             local_avg.temp);

    // Route based on path
    if (strcmp(path, "/") == 0)
    {
        // Root endpoint - Live page using SSE
        const char *page =
            "<!doctype html>\n"
            "<meta charset=\"utf-8\">\n"
            "<title>Live MPU6050</title>\n"
            "<pre id=\"out\">connecting...</pre>\n"
            "<p><a href=\"/json\">View JSON data</a></p>\n"
            "<script>\n"
            "const out = document.getElementById('out');\n"
            "const es = new EventSource('/events');\n"
            "es.onmessage = (e) => { out.textContent = e.data; };\n"
            "es.onerror = () => { out.textContent += \"\\n[stream error]\"; };\n"
            "</script>\n";

        body_len = snprintf(body, sizeof(body), "%s", page);
        header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/html; charset=utf-8\r\n"
                              "Cache-Control: no-store\r\n"
                              "Content-Length: %d\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              body_len);
    }
    else if (strcmp(path, "/json") == 0)
    {
        // JSON endpoint
        body_len = snprintf(body, sizeof(body),
                            "{\"status\":\"ok\","
                            "\"sample\":{\"ax\":%.6f,\"ay\":%.6f,\"az\":%.6f,\"gx\":%.6f,\"gy\":%.6f,\"gz\":%.6f,\"temp\":%.6f},"
                            "\"average\":{\"ax\":%.6f,\"ay\":%.6f,\"az\":%.6f,\"gx\":%.6f,\"gy\":%.6f,\"gz\":%.6f,\"temp\":%.6f},"
                            "\"timestamp\":%ld}",
                            local_sample.ax, local_sample.ay, local_sample.az,
                            local_sample.gx, local_sample.gy, local_sample.gz,
                            local_sample.temp,
                            local_avg.ax, local_avg.ay, local_avg.az,
                            local_avg.gx, local_avg.gy, local_avg.gz,
                            local_avg.temp,
                            (long)time(NULL));

        header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: %d\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              body_len);
    }
    else if (strcmp(path, "/events") == 0)
    {
        // --- Send SSE headers ---
        const char *hdr =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream; charset=utf-8\r\n"
            "Cache-Control: no-cache, no-transform\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "X-Accel-Buffering: no\r\n"
            "\r\n";

        if (send(client_fd, hdr, strlen(hdr), MSG_NOSIGNAL) <= 0)
        {
            fprintf(stderr, "[SSE] failed to send header (errno=%d)\n", errno);
            close(client_fd);
            goto out;
        }

        // 100 ms retry directive
        const char *retry = "retry: 100\n\n";
        if (send(client_fd, retry, strlen(retry), MSG_NOSIGNAL) <= 0)
        {
            fprintf(stderr, "[SSE] failed to send retry (errno=%d)\n", errno);
            close(client_fd);
            goto out;
        }

        // --- epoll setup for this client ---
        int epfd = epoll_create1(EPOLL_CLOEXEC);
        if (epfd < 0)
        {
            perror("[SSE] epoll_create1");
            close(client_fd);
            goto out;
        }

        struct epoll_event ev = {0};
        ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
        ev.data.fd = client_fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) < 0)
        {
            perror("[SSE] epoll_ctl");
            close(epfd);
            close(client_fd);
            goto out;
        }

        // --- SSE main loop: wait for new data via condition variable ---
        char json[1024];
        char event[1152];
        int jlen, elen;
        uint64_t last_version = 0;

        while (running)
        {
            struct epoll_event rev;
            int n = epoll_wait(epfd, &rev, 1, 10); // short poll to check for disconnect

            if (n < 0)
            {
                if (errno == EINTR)
                    continue;
                perror("[SSE] epoll_wait");
                break;
            }

            if (n == 0)
            {
                /* Wait for new data using condition variable (more efficient than polling) */
                uint64_t new_version = wait_for_new_data(last_version, &local_sample, &local_avg, 100);
                if (new_version == last_version)
                    continue; /* timeout, no new data */
                last_version = new_version;

                jlen = snprintf(json, sizeof(json),
                                "{\"status\":\"ok\","
                                "\"sample\":{\"ax\":%.6f,\"ay\":%.6f,\"az\":%.6f,"
                                "\"gx\":%.6f,\"gy\":%.6f,\"gz\":%.6f,\"temp\":%.6f},"
                                "\"average\":{\"ax\":%.6f,\"ay\":%.6f,\"az\":%.6f,"
                                "\"gx\":%.6f,\"gy\":%.6f,\"gz\":%.6f,\"temp\":%.6f},"
                                "\"timestamp\":%ld}",
                                local_sample.ax, local_sample.ay, local_sample.az,
                                local_sample.gx, local_sample.gy, local_sample.gz,
                                local_sample.temp,
                                local_avg.ax, local_avg.ay, local_avg.az,
                                local_avg.gx, local_avg.gy, local_avg.gz,
                                local_avg.temp,
                                (long)time(NULL));

                elen = snprintf(event, sizeof(event), "data: %.*s\n\n", jlen, json);

                ssize_t sent = send(client_fd, event, (size_t)elen, MSG_NOSIGNAL);
                if (sent <= 0)
                {
                    fprintf(stderr, "[SSE] send failed (errno=%d)\n", errno);
                    break;
                }
                continue;
            }

            // n > 0: check events
            uint32_t e = rev.events;
            if (e & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                fprintf(stderr, "[SSE] peer closed or error (events=0x%x)\n", e);
                break;
            }

            if (e & EPOLLIN)
            {
                // Drain any input and detect EOF
                char tmp[256];
                for (;;)
                {
                    ssize_t r = recv(client_fd, tmp, sizeof(tmp), MSG_DONTWAIT);
                    if (r == 0)
                    {
                        fprintf(stderr, "[SSE] peer orderly shutdown (EOF)\n");
                        break;
                    }
                    if (r < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        if (errno == ECONNRESET)
                        {
                            fprintf(stderr, "[SSE] connection reset by peer\n");
                        }
                        break;
                    }
                    if (r < (ssize_t)sizeof(tmp))
                        break;
                }
            }
        }

        fprintf(stderr, "[SSE] exiting SSE loop\n");
        fflush(stderr);
        close(epfd);
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        goto out;
    }
    else
    {
        // 404 Not Found
        body_len = snprintf(body, sizeof(body),
                            "<!doctype html><meta charset=\"utf-8\"><title>404 Not Found</title>"
                            "<h1>404 Not Found</h1>"
                            "<p>The requested path was not found.</p>"
                            "<p><a href=\"/\">Go to home</a> | <a href=\"/json\">View JSON</a> | <a href=\"/events\">View Events</a></p>\n");

        header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 404 Not Found\r\n"
                              "Content-Type: text/html; charset=utf-8\r\n"
                              "Content-Length: %d\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              body_len);
    }

    // Ensure lengths are valid
    if (body_len < 0)
        body_len = 0;
    if (body_len >= (int)sizeof(body))
        body_len = (int)sizeof(body) - 1;
    if (header_len < 0)
        header_len = 0;
    if (header_len >= (int)sizeof(header))
        header_len = (int)sizeof(header) - 1;

    // Send response
    if (header_len > 0)
    {
        ssize_t s = send(client_fd, header, (size_t)header_len, MSG_NOSIGNAL);
        if (s != header_len)
        {
            close(client_fd);
            goto out;
        }
    }
    if (body_len > 0)
        (void)send(client_fd, body, (size_t)body_len, MSG_NOSIGNAL);

    close(client_fd);
out:
    pthread_cleanup_pop(1);
    return NULL;
}

int main()
{

    if (read_config_from_file(&g_server_config) != 0)
    {
        printf("Using default configuration\n");
        g_server_config = (server_config_t)DEFAULT_SERVER_CONFIG;
    }
    else
    {
        printf("Configuration loaded from: %s\n", CONFIG_FILE);
        printf("max_connections=%d, backlog=%d, port=%d\n",
               g_server_config.max_connnections, g_server_config.backlog,
               g_server_config.port);
    }

    // Initialize shared memory connection
    if (init_shared_memory() != 0)
    {
        printf("Failed to initialize shared memory. Make sure the producer is running first.\n");
        return 1;
    }

    printf("Connected to shared memory from producer process\n");

    /* Start data reader thread to cache shared memory data */
    pthread_t reader_tid;
    if (pthread_create(&reader_tid, NULL, data_reader_thread, NULL) != 0)
    {
        perror("pthread_create data_reader_thread");
        cleanup_shared_memory();
        return 1;
    }
    pthread_detach(reader_tid);

    /**
     * The self-pipe trick is a simple and reliable way to handle signals in a program that uses select() or poll().
     * The idea is to create a pipe and write to it from the signal handler. The read end of the pipe is included in the
     * set of file descriptors monitored by select()/poll(). When a signal is caught, the handler writes a byte to the pipe,
     */
    if (pipe(sigpipe_fds) < 0)
    {
        perror("pipe");
        return 1;
    }

    /* make write end non-blocking, set CLOEXEC */
    int flags = fcntl(sigpipe_fds[1], F_GETFL, 0);
    if (flags >= 0)
        fcntl(sigpipe_fds[1], F_SETFL, flags | O_NONBLOCK);
    else
    {
        perror("fcntl");
        return 1;
    }
    fcntl(sigpipe_fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(sigpipe_fds[1], F_SETFD, FD_CLOEXEC);

    /**
     * SIGPIPE: its default action is to terminate the process when you write to a closed socket/pipe.
     * For network servers the common pattern is to set SIGPIPE to SIG_IGN and handle EPIPE from write()
     * (or use send() with MSG_NOSIGNAL). That avoids unexpected process termination and keeps error handling
     * in user code rather than in a handler.
     */
    struct sigaction ign = {0};
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, NULL);

    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    for (int s = 1; s < _NSIG; ++s)
    {
        if (s == SIGKILL || s == SIGSTOP || s == SIGPIPE)
            continue;
        sigaction(s, &sa, NULL);
    }

    /* server socket */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_server_config.port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        return -1;
    }
    if (listen(server_fd, g_server_config.backlog) < 0)
    {
        perror("listen");
        close(server_fd);
        return -1;
    }

    printf("Listening on http://localhost:%d\n", g_server_config.port);

    while (running)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        FD_SET(sigpipe_fds[0], &rfds);
        int maxfd = server_fd > sigpipe_fds[0] ? server_fd : sigpipe_fds[0];

        // Using select to wait for incoming connections or signals
        // Used to handle graceful shutdown.
        int ret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }

        if (FD_ISSET(sigpipe_fds[0], &rfds))
        {
            /* drain and exit loop */
            char buf[64];
            while (read(sigpipe_fds[0], buf, sizeof(buf)) > 0)
            {
            }
            break;
        }

        if (FD_ISSET(server_fd, &rfds))
        {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int *client_fd = malloc(sizeof(int));
            if (!client_fd)
                continue;
            *client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
            if (*client_fd < 0)
            {
                perror("accept");
                free(client_fd);
                continue;
            }

            /* Limit concurrent connections if configured (>0). */
            atomic_int active_connections = atomic_load(&g_active_connections);
            if (g_server_config.max_connnections > 0 &&
                active_connections >= g_server_config.max_connnections)
            {
                // Reject connection when limit reached
                printf("Max connections reached: %d/%d, rejecting.\n", active_connections, g_server_config.max_connnections);
                close(*client_fd);
                free(client_fd);
                continue;
            }
            else if (g_server_config.max_connnections == 0)
            {
                // No connections allowed
                printf("No connections allowed, rejecting.\n");
                close(*client_fd);
                free(client_fd);
                continue;
            }
            else
            {
                pthread_t tid;
                /* Reserve a slot before creating the thread */
                atomic_fetch_add(&g_active_connections, 1);
                if (pthread_create(&tid, NULL, conn_handler, client_fd) != 0)
                {
                    perror("pthread_create");
                    /* release reserved slot on failure */
                    atomic_fetch_sub(&g_active_connections, 1);
                    close(*client_fd);
                    free(client_fd);
                    continue;
                }
                pthread_detach(tid);
            }
        }
    }

    printf("Shutting down...\n");
    close(server_fd);
    close(sigpipe_fds[0]);
    close(sigpipe_fds[1]);
    cleanup_shared_memory();

    return 0;
}
