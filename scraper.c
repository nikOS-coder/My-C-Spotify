#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <netdb.h>
#include <mongoc/mongoc.h>
#include <bson/bson.h>
#include <curl/curl.h>
#include "cJSON.h"

#define BUFFER_LINES 10
#define CONSUMERS 5

typedef struct{
    char *memory;
    size_t size;
}Memory;

// Libcurl write callback function to dynamically resize buffer memory as data arrives
size_t Callback(void* contents, size_t size, size_t num_memb, void* userp) {
    size_t bytes = size * num_memb;
    Memory *chunk = (Memory*)userp;

    char *ptr = realloc(chunk->memory, chunk->size + bytes + 1);
    if(!ptr) {
        fprintf(stderr, "Problem with memory allocation!\n");
        return EXIT_FAILURE;
    }

    chunk->memory = ptr;
    memcpy(&(chunk->memory[chunk->size]), contents, bytes);
    chunk->size += bytes;
    chunk->memory[chunk->size] = '\0';

    return bytes;
}

typedef struct{
    char title[256];
    char artist[256];
    char link[512];
}Song;

typedef struct{
    Song buffer[BUFFER_LINES];
    int in;
    int out;
    int count;

    pthread_mutex_t mutex;
    sem_t full;
    sem_t empty;

    mongoc_client_pool_t *pool;
}BufferQueue;

// Producer thread: fetches data from API via libcurl, parses JSON, and pushes items to buffer queue
void *Producer(void *args) {
    BufferQueue *q = (BufferQueue *)args;

    Memory chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    CURL *curl = curl_easy_init();
    if(!curl) {
        fprintf(stderr, "Problem setting up curl!\n");
    }
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.deezer.com/search?q=metal&limit=100");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);

    CURLcode res = curl_easy_perform(curl);        
    if(res != CURLE_OK) {
        fprintf(stderr, "Internet failed: %s", curl_easy_strerror(res));
    }
    curl_easy_cleanup(curl);

    // Parse fetched JSON response using cJSON
    cJSON *json = cJSON_Parse(chunk.memory);
    if(json == NULL) {
            printf("Failed to read from JSon!\n");
    }
    cJSON *data_array = cJSON_GetObjectItemCaseSensitive(json, "data");

    if (cJSON_IsArray(data_array)) {
            int num_songs = cJSON_GetArraySize(data_array);

            for (int i = 0; i < num_songs; i++) {
                cJSON *song_item = cJSON_GetArrayItem(data_array, i);
                    
                cJSON *title = cJSON_GetObjectItemCaseSensitive(song_item, "title");
                cJSON *preview = cJSON_GetObjectItemCaseSensitive(song_item, "preview");

                cJSON *artist_obj = cJSON_GetObjectItemCaseSensitive(song_item, "artist");
                cJSON *artist_name = cJSON_GetObjectItemCaseSensitive(artist_obj, "name");

                if (cJSON_IsString(title) && cJSON_IsString(artist_name) && cJSON_IsString(preview)) {
                        
                    // Thread-safe insertion into bounded queue using semaphores and mutexes
                    sem_wait(&q->empty);
                    pthread_mutex_lock(&q->mutex);

                    strncpy(q->buffer[q->in].title, title->valuestring, 255);
                    strncpy(q->buffer[q->in].artist, artist_name->valuestring, 255);
                    strncpy(q->buffer[q->in].link, preview->valuestring, 511);
                    q->buffer->link[511] = '\0';

                    q->in = (q->in + 1) % BUFFER_LINES;
                    q->count++; 

                    sem_post(&q->full);
                    pthread_mutex_unlock(&q->mutex);
                        
                    printf("Producer: I put in the Buffer -> %s - %s\n", artist_name->valuestring, title->valuestring);
                }
            }
            
            // Broadcast EXIT signals to all consumer threads to terminate gracefully
            for(int i = 0; i < CONSUMERS; i++) {
                 sem_wait(&q->empty);
                    pthread_mutex_lock(&q->mutex);

                    strncpy(q->buffer[q->in].title, "EXIT", 255);
                    q->in = (q->in + 1) % BUFFER_LINES;
                    q->count++;

                    sem_post(&q->full);
                    pthread_mutex_unlock(&q->mutex);
            }
    }
    cJSON_Delete(json);
    free(chunk.memory);
    return NULL;
}

// Consumer thread: pops items from queue and inserts them into MongoDB using a thread pool connection
void *Consumer(void *args) {
    Song current_song;
    BufferQueue *q = (BufferQueue *)args;

    mongoc_client_t *client = mongoc_client_pool_pop(q->pool);
    mongoc_collection_t *collection = mongoc_client_get_collection(client, "MySpotify", "Songs");

    while(1) {
        // Thread-safe extraction from queue
        sem_wait(&q->full);
        pthread_mutex_lock(&q->mutex);

        current_song = q->buffer[q->out];
        q->out = (q->out + 1) % BUFFER_LINES;
        q->count--;

        sem_post(&q->empty);
        pthread_mutex_unlock(&q->mutex);

        // Check for termination signal
        if (strcmp(current_song.title, "EXIT") == 0) {
            printf("Consumer: Found EXIT, adieu...\n");
            break; 
        }

        // Create BSON document and insert into MongoDB collection
        bson_t *doc = bson_new();
        BSON_APPEND_UTF8(doc, "title", current_song.title);
        BSON_APPEND_UTF8(doc, "artist", current_song.artist);
        BSON_APPEND_UTF8(doc, "link", current_song.link);

        bson_error_t error;
        if (!mongoc_collection_insert_one(collection, doc, NULL, NULL, &error)) {
            printf("Failed to insert into MongoDB: %s\n", error.message);
        }
        else {
            printf("Consumer: Just gave a text of %zu characters to the DB!\n", sizeof(current_song));
        }

        bson_destroy(doc);
    }
    mongoc_collection_destroy(collection);
    mongoc_client_pool_push(q->pool, client);

    return NULL;
}

int main(void) {
    struct timespec start, end;
    double elapsed;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Initialize global CURL and MongoDB connection pool
    curl_global_init(CURL_GLOBAL_ALL);
    mongoc_init();
    mongoc_uri_t *uri = mongoc_uri_new("mongodb://127.0.0.1:27017/?appname=multithread_app");
    mongoc_client_pool_t *pool = mongoc_client_pool_new(uri);   

    BufferQueue q;
    q.in = 0;
    q.out = 0;
    q.count = 0;
    q.pool = pool;

    // Initialize synchronization primitives (Mutex and Semaphores)
    pthread_mutex_init(&q.mutex, NULL);
    sem_init(&q.full, 0, 0);
    sem_init(&q.empty, 0, BUFFER_LINES);

    pthread_t producer;
    pthread_t consumer[CONSUMERS];

    // Spawn Producer and Consumer threads
    pthread_create(&producer, NULL, Producer, &q);
    for(int i = 0; i < CONSUMERS; i++) {
        pthread_create(&consumer[i], NULL, Consumer, &q);
    }
 
    // Join threads upon completion
    pthread_join(producer, NULL);
    for(int i = 0; i < CONSUMERS; i++) {
        pthread_join(consumer[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1000000000.0; 
    printf("elapsed time: %.6f seconds\n", elapsed);

    // Destroy synchronization primitives and cleanup libraries
    pthread_mutex_destroy(&q.mutex);
    sem_destroy(&q.full);
    sem_destroy(&q.empty);

    mongoc_client_pool_destroy(pool);
    mongoc_uri_destroy(uri);
    mongoc_cleanup();

    curl_global_cleanup();
    return 0;
}