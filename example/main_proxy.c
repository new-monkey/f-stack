/*
 * HTTP Reverse Proxy using F-Stack
 * 
 * This example demonstrates how to use F-Stack to create an HTTP reverse proxy
 * that receives requests from network clients and forwards them to a backend
 * HTTP service running on localhost (127.0.0.1).
 *
 * Configuration:
 * - Frontend: Listens on port 80 (configured via F-Stack)
 * - Backend: Connects to 127.0.0.1:8080 (configurable via BACKEND_PORT macro)
 */

#include <stdio.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>

#include "ff_config.h"
#include "ff_api.h"
#include "ff_epoll.h"
#include "ff_log.h"

#define MAX_EVENTS 512
#define BUFFER_SIZE 8192
#define BACKEND_PORT 8080  // Backend HTTP service port
#define MAX_CONNECTIONS 1024

/* Connection states */
typedef enum {
    CONN_STATE_IDLE = 0,
    CONN_STATE_READING_REQUEST,
    CONN_STATE_CONNECTING_BACKEND,
    CONN_STATE_SENDING_REQUEST,
    CONN_STATE_READING_RESPONSE,
    CONN_STATE_SENDING_RESPONSE,
    CONN_STATE_CLOSING
} conn_state_t;

/* Connection pair structure to track client-backend mappings */
typedef struct connection_pair {
    int client_fd;
    int backend_fd;
    conn_state_t state;
    
    char request_buf[BUFFER_SIZE];
    int request_len;
    int request_sent;
    
    char response_buf[BUFFER_SIZE];
    int response_len;
    int response_sent;
    
    int active;
} connection_pair_t;

/* Global state */
struct epoll_event ev;
struct epoll_event events[MAX_EVENTS];
int epfd;
int sockfd;
connection_pair_t connections[MAX_CONNECTIONS];

/* Find a free connection slot */
static connection_pair_t* alloc_connection(void) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!connections[i].active) {
            memset(&connections[i], 0, sizeof(connection_pair_t));
            connections[i].active = 1;
            connections[i].client_fd = -1;
            connections[i].backend_fd = -1;
            connections[i].state = CONN_STATE_IDLE;
            return &connections[i];
        }
    }
    return NULL;
}

/* Find connection by client fd */
static connection_pair_t* find_connection_by_client(int client_fd) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].active && connections[i].client_fd == client_fd) {
            return &connections[i];
        }
    }
    return NULL;
}

/* Find connection by backend fd */
static connection_pair_t* find_connection_by_backend(int backend_fd) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].active && connections[i].backend_fd == backend_fd) {
            return &connections[i];
        }
    }
    return NULL;
}

/* Free a connection */
static void free_connection(connection_pair_t* conn) {
    if (conn->client_fd >= 0) {
        ff_epoll_ctl(epfd, EPOLL_CTL_DEL, conn->client_fd, NULL);
        ff_close(conn->client_fd);
        conn->client_fd = -1;
    }
    if (conn->backend_fd >= 0) {
        ff_epoll_ctl(epfd, EPOLL_CTL_DEL, conn->backend_fd, NULL);
        ff_close(conn->backend_fd);
        conn->backend_fd = -1;
    }
    conn->active = 0;
    conn->state = CONN_STATE_IDLE;
}

/* Connect to backend server */
static int connect_to_backend(connection_pair_t* conn) {
    int backend_fd = ff_socket(AF_INET, SOCK_STREAM, 0);
    if (backend_fd < 0) {
        ff_log(FF_LOG_ERR, FF_LOGTYPE_FSTACK_APP, "Failed to create backend socket: %s\n", strerror(errno));
        return -1;
    }
    
    /* Set non-blocking */
    int on = 1;
    ff_ioctl(backend_fd, FIONBIO, &on);
    
    /* Connect to backend (127.0.0.1:BACKEND_PORT) */
    struct sockaddr_in backend_addr;
    bzero(&backend_addr, sizeof(backend_addr));
    backend_addr.sin_family = AF_INET;
    backend_addr.sin_port = htons(BACKEND_PORT);
    inet_pton(AF_INET, "127.0.0.1", &backend_addr.sin_addr);
    
    int ret = ff_connect(backend_fd, (struct linux_sockaddr *)&backend_addr, sizeof(backend_addr));
    if (ret < 0 && errno != EINPROGRESS) {
        ff_log(FF_LOG_ERR, FF_LOGTYPE_FSTACK_APP, "Failed to connect to backend: %s\n", strerror(errno));
        ff_close(backend_fd);
        return -1;
    }
    
    conn->backend_fd = backend_fd;
    
    /* Add backend socket to epoll for write events (connection complete) and read events */
    ev.data.fd = backend_fd;
    ev.events = EPOLLOUT | EPOLLIN | EPOLLERR;
    if (ff_epoll_ctl(epfd, EPOLL_CTL_ADD, backend_fd, &ev) != 0) {
        ff_log(FF_LOG_ERR, FF_LOGTYPE_FSTACK_APP, "Failed to add backend to epoll: %s\n", strerror(errno));
        ff_close(backend_fd);
        conn->backend_fd = -1;
        return -1;
    }
    
    return 0;
}

/* Handle new client connection */
static void handle_new_connection(void) {
    while (1) {
        int nclientfd = ff_accept(sockfd, NULL, NULL);
        if (nclientfd < 0) {
            break;
        }
        
        ff_log(FF_LOG_DEBUG, FF_LOGTYPE_FSTACK_APP, "New client connection: fd=%d\n", nclientfd);
        
        /* Allocate connection structure */
        connection_pair_t* conn = alloc_connection();
        if (!conn) {
            ff_log(FF_LOG_ERR, FF_LOGTYPE_FSTACK_APP, "No free connection slots\n");
            ff_close(nclientfd);
            break;
        }
        
        conn->client_fd = nclientfd;
        conn->state = CONN_STATE_READING_REQUEST;
        
        /* Set non-blocking */
        int on = 1;
        ff_ioctl(nclientfd, FIONBIO, &on);
        
        /* Add client socket to epoll */
        ev.data.fd = nclientfd;
        ev.events = EPOLLIN | EPOLLERR;
        if (ff_epoll_ctl(epfd, EPOLL_CTL_ADD, nclientfd, &ev) != 0) {
            ff_log(FF_LOG_ERR, FF_LOGTYPE_FSTACK_APP, "Failed to add client to epoll: %s\n", strerror(errno));
            free_connection(conn);
            break;
        }
    }
}

/* Handle client data (reading request) */
static void handle_client_read(connection_pair_t* conn) {
    int space_left = BUFFER_SIZE - conn->request_len;
    if (space_left <= 0) {
        ff_log(FF_LOG_ERR, FF_LOGTYPE_FSTACK_APP, "Request buffer full\n");
        free_connection(conn);
        return;
    }
    
    ssize_t n = ff_read(conn->client_fd, conn->request_buf + conn->request_len, space_left);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;  /* No data available yet */
        }
        ff_log(FF_LOG_DEBUG, FF_LOGTYPE_FSTACK_APP, "Client read error: %s\n", strerror(errno));
        free_connection(conn);
        return;
    }
    
    if (n == 0) {
        /* Client closed connection */
        ff_log(FF_LOG_DEBUG, FF_LOGTYPE_FSTACK_APP, "Client closed connection\n");
        free_connection(conn);
        return;
    }
    
    conn->request_len += n;
    
    /* Check if we have a complete HTTP request (simple check for \r\n\r\n) */
    if (conn->request_len >= 4) {
        char* end_marker = strstr(conn->request_buf, "\r\n\r\n");
        if (end_marker) {
            /* We have at least the headers, check for body */
            char* content_length_str = strcasestr(conn->request_buf, "Content-Length:");
            int content_length = 0;
            if (content_length_str) {
                content_length = atoi(content_length_str + 15);
            }
            
            int headers_len = (end_marker - conn->request_buf) + 4;
            int total_expected = headers_len + content_length;
            
            if (conn->request_len >= total_expected) {
                /* Complete request received, connect to backend */
                ff_log(FF_LOG_DEBUG, FF_LOGTYPE_FSTACK_APP, "Complete request received (%d bytes)\n", conn->request_len);
                
                conn->state = CONN_STATE_CONNECTING_BACKEND;
                if (connect_to_backend(conn) < 0) {
                    free_connection(conn);
                    return;
                }
                conn->state = CONN_STATE_SENDING_REQUEST;
            }
        }
    }
}

/* Handle sending request to backend */
static void handle_backend_write(connection_pair_t* conn) {
    if (conn->state == CONN_STATE_CONNECTING_BACKEND) {
        /* Connection just completed, now send request */
        conn->state = CONN_STATE_SENDING_REQUEST;
    }
    
    if (conn->state != CONN_STATE_SENDING_REQUEST) {
        return;
    }
    
    int remaining = conn->request_len - conn->request_sent;
    if (remaining <= 0) {
        /* All request data sent, now wait for response */
        conn->state = CONN_STATE_READING_RESPONSE;
        
        /* Update epoll to only monitor read events on backend */
        ev.data.fd = conn->backend_fd;
        ev.events = EPOLLIN | EPOLLERR;
        ff_epoll_ctl(epfd, EPOLL_CTL_MOD, conn->backend_fd, &ev);
        return;
    }
    
    ssize_t n = ff_write(conn->backend_fd, conn->request_buf + conn->request_sent, remaining);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;  /* Socket not ready yet */
        }
        ff_log(FF_LOG_ERR, FF_LOGTYPE_FSTACK_APP, "Backend write error: %s\n", strerror(errno));
        free_connection(conn);
        return;
    }
    
    conn->request_sent += n;
    ff_log(FF_LOG_DEBUG, FF_LOGTYPE_FSTACK_APP, "Sent %zd bytes to backend (%d/%d)\n", 
           n, conn->request_sent, conn->request_len);
}

/* Handle reading response from backend */
static void handle_backend_read(connection_pair_t* conn) {
    int space_left = BUFFER_SIZE - conn->response_len;
    if (space_left <= 0) {
        /* Buffer full, need to send to client first */
        handle_client_write(conn);
        return;
    }
    
    ssize_t n = ff_read(conn->backend_fd, conn->response_buf + conn->response_len, space_left);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;  /* No data available yet */
        }
        ff_log(FF_LOG_ERR, FF_LOGTYPE_FSTACK_APP, "Backend read error: %s\n", strerror(errno));
        free_connection(conn);
        return;
    }
    
    if (n == 0) {
        /* Backend closed connection */
        ff_log(FF_LOG_DEBUG, FF_LOGTYPE_FSTACK_APP, "Backend closed connection\n");
        
        if (conn->response_len > conn->response_sent) {
            /* Still have data to send to client */
            conn->state = CONN_STATE_SENDING_RESPONSE;
            handle_client_write(conn);
        }
        
        /* Close backend connection */
        if (conn->backend_fd >= 0) {
            ff_epoll_ctl(epfd, EPOLL_CTL_DEL, conn->backend_fd, NULL);
            ff_close(conn->backend_fd);
            conn->backend_fd = -1;
        }
        
        /* Check if we're done */
        if (conn->response_sent >= conn->response_len) {
            free_connection(conn);
        }
        return;
    }
    
    conn->response_len += n;
    conn->state = CONN_STATE_SENDING_RESPONSE;
    
    ff_log(FF_LOG_DEBUG, FF_LOGTYPE_FSTACK_APP, "Received %zd bytes from backend (total: %d)\n", 
           n, conn->response_len);
    
    /* Try to send to client immediately */
    handle_client_write(conn);
}

/* Handle sending response to client */
static void handle_client_write(connection_pair_t* conn) {
    if (conn->response_sent >= conn->response_len) {
        return;  /* Nothing to send */
    }
    
    int remaining = conn->response_len - conn->response_sent;
    ssize_t n = ff_write(conn->client_fd, conn->response_buf + conn->response_sent, remaining);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Enable write events on client socket */
            ev.data.fd = conn->client_fd;
            ev.events = EPOLLIN | EPOLLOUT | EPOLLERR;
            ff_epoll_ctl(epfd, EPOLL_CTL_MOD, conn->client_fd, &ev);
            return;
        }
        ff_log(FF_LOG_ERR, FF_LOGTYPE_FSTACK_APP, "Client write error: %s\n", strerror(errno));
        free_connection(conn);
        return;
    }
    
    conn->response_sent += n;
    ff_log(FF_LOG_DEBUG, FF_LOGTYPE_FSTACK_APP, "Sent %zd bytes to client (%d/%d)\n", 
           n, conn->response_sent, conn->response_len);
    
    if (conn->response_sent >= conn->response_len) {
        /* All response data sent */
        if (conn->backend_fd < 0) {
            /* Backend already closed, we're done */
            free_connection(conn);
            return;
        }
        
        /* Reset buffers for potential keep-alive */
        conn->request_len = 0;
        conn->request_sent = 0;
        conn->response_len = 0;
        conn->response_sent = 0;
        conn->state = CONN_STATE_READING_REQUEST;
        
        /* Disable write events on client socket */
        ev.data.fd = conn->client_fd;
        ev.events = EPOLLIN | EPOLLERR;
        ff_epoll_ctl(epfd, EPOLL_CTL_MOD, conn->client_fd, &ev);
    }
}

/* Main event loop */
int loop(void *arg) {
    int nevents = ff_epoll_wait(epfd, events, MAX_EVENTS, 0);
    
    for (int i = 0; i < nevents; i++) {
        int fd = events[i].data.fd;
        
        /* Handle new client connections */
        if (fd == sockfd) {
            handle_new_connection();
            continue;
        }
        
        /* Check if this is a client or backend socket */
        connection_pair_t* conn = find_connection_by_client(fd);
        if (conn) {
            /* This is a client socket event */
            if (events[i].events & EPOLLERR) {
                ff_log(FF_LOG_DEBUG, FF_LOGTYPE_FSTACK_APP, "Client socket error\n");
                free_connection(conn);
            } else if (events[i].events & EPOLLIN) {
                if (conn->state == CONN_STATE_READING_REQUEST) {
                    handle_client_read(conn);
                }
            } else if (events[i].events & EPOLLOUT) {
                if (conn->state == CONN_STATE_SENDING_RESPONSE) {
                    handle_client_write(conn);
                }
            }
            continue;
        }
        
        conn = find_connection_by_backend(fd);
        if (conn) {
            /* This is a backend socket event */
            if (events[i].events & EPOLLERR) {
                ff_log(FF_LOG_DEBUG, FF_LOGTYPE_FSTACK_APP, "Backend socket error\n");
                free_connection(conn);
            } else if (events[i].events & EPOLLOUT) {
                handle_backend_write(conn);
            } else if (events[i].events & EPOLLIN) {
                if (conn->state == CONN_STATE_READING_RESPONSE || 
                    conn->state == CONN_STATE_SENDING_RESPONSE) {
                    handle_backend_read(conn);
                }
            }
            continue;
        }
        
        ff_log(FF_LOG_WARNING, FF_LOGTYPE_FSTACK_APP, "Unknown fd in event: %d\n", fd);
    }
    
    return 0;
}

int main(int argc, char * argv[]) {
    /* Initialize F-Stack */
    ff_init(argc, argv);
    
    /* Initialize connection pool */
    memset(connections, 0, sizeof(connections));
    
    /* Create listening socket */
    sockfd = ff_socket(AF_INET, SOCK_STREAM, 0);
    ff_log(FF_LOG_INFO, FF_LOGTYPE_FSTACK_APP, "sockfd:%d\n", sockfd);
    if (sockfd < 0) {
        ff_log(FF_LOG_ERR, FF_LOGTYPE_FSTACK_APP, "ff_socket failed\n");
        exit(1);
    }
    
    /* Set non-blocking */
    int on = 1;
    ff_ioctl(sockfd, FIONBIO, &on);
    
    /* Enable address reuse */
    ff_setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    
    /* Bind to port 80 */
    struct sockaddr_in my_addr;
    bzero(&my_addr, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(80);
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    int ret = ff_bind(sockfd, (struct linux_sockaddr *)&my_addr, sizeof(my_addr));
    if (ret < 0) {
        ff_log(FF_LOG_ERR, FF_LOGTYPE_FSTACK_APP, "ff_bind failed: %s\n", strerror(errno));
        exit(1);
    }
    
    /* Listen for connections */
    ret = ff_listen(sockfd, MAX_EVENTS);
    if (ret < 0) {
        ff_log(FF_LOG_ERR, FF_LOGTYPE_FSTACK_APP, "ff_listen failed: %s\n", strerror(errno));
        exit(1);
    }
    
    /* Create epoll instance */
    assert((epfd = ff_epoll_create(0)) > 0);
    
    /* Add listening socket to epoll */
    ev.data.fd = sockfd;
    ev.events = EPOLLIN;
    ff_epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);
    
    ff_log(FF_LOG_INFO, FF_LOGTYPE_FSTACK_APP, 
           "HTTP Reverse Proxy started. Listening on port 80, forwarding to 127.0.0.1:%d\n", 
           BACKEND_PORT);
    
    /* Run the event loop */
    ff_run(loop, NULL);
    
    return 0;
}
