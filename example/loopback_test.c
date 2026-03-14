/*
 * F-Stack Loopback Interface Test
 * 
 * This example demonstrates F-Stack loopback functionality:
 * - Server binds to 127.0.0.1:8080 using F-Stack APIs
 * - Client connects to 127.0.0.1:8080 using F-Stack APIs
 * - Both communicate through F-Stack's internal loopback (lo0)
 * 
 * Compile:
 *   make loopback_test
 * 
 * Run:
 *   ./loopback_test -c config.ini
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>

#include "ff_api.h"

#define SERVER_PORT 8080
#define BUFFER_SIZE 1024
#define TEST_MESSAGE "Hello from F-Stack loopback!"

static volatile int server_ready = 0;
static volatile int test_passed = 0;

/*
 * Server thread - listens on 127.0.0.1:8080
 */
void* server_thread(void* arg) {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    char buffer[BUFFER_SIZE];
    ssize_t n;

    printf("[Server] Starting on 127.0.0.1:%d\n", SERVER_PORT);

    // Create server socket using F-Stack
    server_fd = ff_socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[Server] ff_socket failed");
        return NULL;
    }

    // Enable address reuse
    int opt = 1;
    ff_setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind to loopback address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (ff_bind(server_fd, (struct linux_sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[Server] ff_bind failed");
        ff_close(server_fd);
        return NULL;
    }

    // Listen for connections
    if (ff_listen(server_fd, 5) < 0) {
        perror("[Server] ff_listen failed");
        ff_close(server_fd);
        return NULL;
    }

    printf("[Server] Listening on 127.0.0.1:%d (F-Stack loopback)\n", SERVER_PORT);
    server_ready = 1;  // Signal that server is ready

    // Accept connection
    client_len = sizeof(client_addr);
    client_fd = ff_accept(server_fd, (struct linux_sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("[Server] ff_accept failed");
        ff_close(server_fd);
        return NULL;
    }

    printf("[Server] Accepted connection from %s:%d\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    // Read data from client
    memset(buffer, 0, BUFFER_SIZE);
    n = ff_read(client_fd, buffer, BUFFER_SIZE - 1);
    if (n > 0) {
        printf("[Server] Received %zd bytes: %s\n", n, buffer);
        
        // Verify message
        if (strcmp(buffer, TEST_MESSAGE) == 0) {
            printf("[Server] ✓ Message verification passed!\n");
            test_passed = 1;
        } else {
            printf("[Server] ✗ Message verification failed!\n");
        }

        // Echo back to client
        const char* response = "ACK from server";
        ff_write(client_fd, response, strlen(response));
    } else {
        perror("[Server] ff_read failed");
    }

    // Cleanup
    ff_close(client_fd);
    ff_close(server_fd);

    printf("[Server] Closed\n");
    return NULL;
}

/*
 * Client thread - connects to 127.0.0.1:8080
 */
void* client_thread(void* arg) {
    int client_fd;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    ssize_t n;

    // Wait for server to be ready
    printf("[Client] Waiting for server to start...\n");
    while (!server_ready) {
        usleep(100000);  // 100ms
    }
    usleep(500000);  // Extra 500ms to ensure server is listening

    printf("[Client] Connecting to 127.0.0.1:%d\n", SERVER_PORT);

    // Create client socket using F-Stack
    client_fd = ff_socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) {
        perror("[Client] ff_socket failed");
        return NULL;
    }

    // Connect to server on loopback
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (ff_connect(client_fd, (struct linux_sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("[Client] ff_connect failed");
        ff_close(client_fd);
        return NULL;
    }

    printf("[Client] Connected successfully (via F-Stack lo0)\n");

    // Send test message
    printf("[Client] Sending: %s\n", TEST_MESSAGE);
    n = ff_write(client_fd, TEST_MESSAGE, strlen(TEST_MESSAGE));
    if (n < 0) {
        perror("[Client] ff_write failed");
        ff_close(client_fd);
        return NULL;
    }
    printf("[Client] Sent %zd bytes\n", n);

    // Read response
    memset(buffer, 0, BUFFER_SIZE);
    n = ff_read(client_fd, buffer, BUFFER_SIZE - 1);
    if (n > 0) {
        printf("[Client] Received response: %s\n", buffer);
    }

    // Cleanup
    ff_close(client_fd);
    printf("[Client] Closed\n");

    return NULL;
}

/*
 * Main function
 */
int main(int argc, char* argv[]) {
    pthread_t server_tid, client_tid;
    int ret;

    printf("=================================================\n");
    printf("  F-Stack Loopback Interface Test\n");
    printf("=================================================\n\n");

    // Initialize F-Stack
    printf("Initializing F-Stack...\n");
    ret = ff_init(argc, argv);
    if (ret < 0) {
        fprintf(stderr, "ff_init failed\n");
        return 1;
    }
    printf("F-Stack initialized successfully\n\n");

    // Create server thread
    if (pthread_create(&server_tid, NULL, server_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create server thread\n");
        return 1;
    }

    // Create client thread
    if (pthread_create(&client_tid, NULL, client_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create client thread\n");
        return 1;
    }

    // Wait for threads to complete
    pthread_join(server_tid, NULL);
    pthread_join(client_tid, NULL);

    printf("\n=================================================\n");
    if (test_passed) {
        printf("  TEST RESULT: ✓ PASSED\n");
        printf("  F-Stack loopback interface is working!\n");
    } else {
        printf("  TEST RESULT: ✗ FAILED\n");
        printf("  F-Stack loopback interface test failed.\n");
    }
    printf("=================================================\n");

    return test_passed ? 0 : 1;
}
