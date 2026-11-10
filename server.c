#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <signal.h>
#include <mongoc/mongoc.h>
#include <bson/bson.h>

#define PORT 8080
#define MAX_RESPONSE_SIZE 65536

volatile sig_atomic_t keep_running = 1;

mongoc_collection_t *collection;

// Signal handler for graceful server shutdown via SIGINT (Ctrl+C)
void handle_sigint(int sig) {
    keep_running = 0;
    printf("\n[Server] Recieved a ^C, Closing...\n");
}

// Thread function to handle individual client requests and serve music list from MongoDB
void *handle_client(void* new_socket) {
    int sock = *(int *)new_socket;

    char request[1024] = {0};
    read(sock, request, 1024);

    char *response = malloc(MAX_RESPONSE_SIZE);
    memset(response, 0, MAX_RESPONSE_SIZE);

    // Build HTTP response headers and dark-mode terminal style HTML layout
    strcpy(response, "HTTP/1.1 200 OK\nContent-Type: text/html; charset=UTF-8\n\n");
    strcat(response, "<html><body style='background-color: #222; color: #0f0; font-family: monospace;'>");
    strcat(response, "<h1>🎶 My C-Spotify 🎶</h1><hr>");

    // Query all songs from the MongoDB collection
    bson_t *query = bson_new();
    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(collection, query, NULL, NULL);
    const bson_t *doc;
    while (mongoc_cursor_next(cursor, &doc)) {
        bson_iter_t iter;
        const char *title = "";
        const char *artist = "";
        const char *link = "";

        if (bson_iter_init_find(&iter, doc, "title") && BSON_ITER_HOLDS_UTF8(&iter)) {
            title = bson_iter_utf8(&iter, NULL);
        }
        if (bson_iter_init_find(&iter, doc, "artist") && BSON_ITER_HOLDS_UTF8(&iter)) {
            artist = bson_iter_utf8(&iter, NULL);
        }
        if (bson_iter_init_find(&iter, doc, "link") && BSON_ITER_HOLDS_UTF8(&iter)) {
            link = bson_iter_utf8(&iter, NULL);
        }

        // Format song metadata into HTML audio player elements
        char song_html[1024];
        snprintf(song_html, sizeof(song_html), 
                "<p><b>%s</b> - %s</p>"
                "<audio controls><source src='%s' type='audio/mpeg'></audio><hr>", 
                artist, title, link);

        strcat(response, song_html);
    }
    strcat(response, "</body></html>");
    write(sock, response, strlen(response));

    // Cleanup resources for this client connection
    mongoc_cursor_destroy(cursor);
    bson_destroy(query);
    close(sock);
    free(response);
    free(new_socket);
    return NULL;
}

int main(void) {
    // Create server TCP socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd == -1) {
        fprintf(stderr, "Failed to create socket!\n");
        return EXIT_FAILURE;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // Bind socket to port
    if(bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        fprintf(stderr, "Failed to bind socket to port: %d\n", PORT);
        return EXIT_FAILURE;
    }

    // Listen for incoming connections
    if(listen(server_fd, 10) == -1) {
        fprintf(stderr, "Failed to listen!\n");
        return EXIT_FAILURE;
    }

    printf("server is open\n");

    // Initialize MongoDB driver and establish collection connection
    mongoc_init();
    mongoc_client_t *client = mongoc_client_new("mongodb://127.0.0.1:27017/?appname=multithread_app");
    collection = mongoc_client_get_collection(client, "MySpotify", "Songs");

    // Setup signal action for graceful exit
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; 
    sigaction(SIGINT, &sa, NULL);

    // Accept incoming client connections loop
    while(keep_running) {
        int addrlen = sizeof(address);
        int client_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen);

        if(client_socket < 0) {
            if(!keep_running) { break; }
            fprintf(stderr, "Failed to connect with a costumer!\n");
            continue;
        }
        printf("Someone's there\n");
        sleep(10);

        int *new_sock_ptr = (int *)malloc(sizeof(int));
        *new_sock_ptr = client_socket;

        // Spawn detached thread to handle client request asynchronously
        pthread_t client;
        if(pthread_create(&client, NULL, handle_client, new_sock_ptr) < 0) {
            fprintf(stderr, "Couldnt create thread!\n");
            free(new_sock_ptr);
            continue;
        }
        pthread_detach(client);
    }

    // Cleanup database and server socket handles
    mongoc_collection_destroy(collection);
    mongoc_client_destroy(client);
    mongoc_cleanup();

    close(server_fd);
    return 0;
}