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

#include "./producer.h"

typedef struct
{
    int max_connnections;
    int backlog;
    int port;
    int filter_window_samples;
} server_config_t;

#define CONFIG_FILE "server_config.cfg"
#define DEFAULT_SERVER_CONFIG {.max_connnections = 10, .backlog = 5, .port = 3737, .filter_window_samples = 5}

static volatile sig_atomic_t running = 1;
static int sigpipe_fds[2] = {-1, -1};

server_config_t g_server_config;
shared_data_t *g_shared_data = NULL;
sem_t *g_sem = NULL;

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
        fscanf(file, "port=%d\n", &server_config->port) != 1 ||
        fscanf(file, "filter_window_samples=%d\n", &server_config->filter_window_samples) != 1)
    {
        fclose(file);
        return -2;
    }

    fclose(file);
    return 0;
}

void handle_signal(int sig)
{
    printf("Received signal %d\n", sig);
    switch (sig)
    {
    case SIGUSR2:
        printf("Received SIGUSR2\n");
        read_config_from_file(&g_server_config); // Reload configuration
        printf("Configuration reloaded: max_connections=%d, filter_window_samples=%d\n",
               g_server_config.max_connnections, g_server_config.filter_window_samples);
        break;
    case SIGTERM:
    case SIGINT:
    default:
        running = 0;
        if (sigpipe_fds[1] != -1)
        {
            /* wake select() */
            write(sigpipe_fds[1], "x", 1);
        }
        break;
    }
    return;
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

void *conn_handler(void *arg)
{
    if (!arg)
        return NULL;
    int client_fd = *(int *)arg;
    free(arg);

    // Read HTTP request
    char request[1024] = {0};
    ssize_t bytes_read = recv(client_fd, request, sizeof(request) - 1, 0);
    if (bytes_read <= 0)
    {
        close(client_fd);
        return NULL;
    }

    // Parse request line to get the path
    char method[16], path[256], version[16];
    if (sscanf(request, "%15s %255s %15s", method, path, version) != 3)
    {
        close(client_fd);
        return NULL;
    }

    mpu6050_sample_float_t local_avg = {0};

    if (g_shared_data && g_sem)
    {
        sem_wait(g_sem);
        local_avg = g_shared_data->average;
        sem_post(g_sem);
    }

    char body[512];
    char header[256];
    char avg_str[128];
    int body_len, header_len;

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
                            "{\n"
                            "  \"status\": \"ok\",\n"
                            "  \"ax\": %f,\n"
                            "  \"ay\": %f,\n"
                            "  \"az\": %f,\n"
                            "  \"gx\": %f,\n"
                            "  \"gy\": %f,\n"
                            "  \"gz\": %f,\n"
                            "  \"temp\": %f,\n"
                            "  \"timestamp\": %ld\n"
                            "}\n",
                            local_avg.ax, local_avg.ay, local_avg.az,
                            local_avg.gx, local_avg.gy, local_avg.gz,
                            local_avg.temp, (long)time(NULL));

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
            close(client_fd);
            return NULL;
        }
        char json[256];
        char event[320];
        int jlen, elen;
        bool first_event = true;
        // Optional: suggest client retry delay
        // Seems to be part of SSE spec
        const char *retry = "retry: 1000\n\n";
        (void)send(client_fd, retry, strlen(retry), MSG_NOSIGNAL);

        while (running)
        {
            if (first_event)
            {
                first_event = false;
            }
            else
            {
                usleep(1000 * 1000); // 1s
            }

            if (g_shared_data && g_sem)
            {
                sem_wait(g_sem);
                local_avg = g_shared_data->average;
                sem_post(g_sem);
            }
            jlen = snprintf(json, sizeof(json),
                            "{"
                            "\"status\":\"ok\","
                            "\"ax\":%.6f,\"ay\":%.6f,\"az\":%.6f,"
                            "\"gx\":%.6f,\"gy\":%.6f,\"gz\":%.6f,"
                            "\"temp\":%.6f,"
                            "\"timestamp\":%ld"
                            "}",
                            local_avg.ax, local_avg.ay, local_avg.az,
                            local_avg.gx, local_avg.gy, local_avg.gz,
                            local_avg.temp, (long)time(NULL));
            elen = snprintf(event, sizeof(event), "data: %.*s\n\n", jlen, json);
            if (send(client_fd, event, (size_t)elen, MSG_NOSIGNAL) <= 0)
                break;
        }

        close(client_fd);
        return NULL;
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
            return NULL;
        }
    }
    if (body_len > 0)
        (void)send(client_fd, body, (size_t)body_len, MSG_NOSIGNAL);

    close(client_fd);
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
        printf("max_connections=%d, backlog=%d, port=%d, filter_window_samples=%d\n",
               g_server_config.max_connnections, g_server_config.backlog,
               g_server_config.port, g_server_config.filter_window_samples);
    }

    // Initialize shared memory connection
    if (init_shared_memory() != 0)
    {
        printf("Failed to initialize shared memory. Make sure the producer is running first.\n");
        return 1;
    }

    printf("Connected to shared memory from producer process\n");

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

            pthread_t tid;
            if (pthread_create(&tid, NULL, conn_handler, client_fd) != 0)
            {
                perror("pthread_create");
                close(*client_fd);
                free(client_fd);
                continue;
            }
            pthread_detach(tid);
        }
    }

    printf("Shutting down...\n");
    close(server_fd);
    close(sigpipe_fds[0]);
    close(sigpipe_fds[1]);
    cleanup_shared_memory();

    return 0;
}
