# C Multi-Threaded Music Scraper & Web Streaming Server ("My-C-Spotify")

A concurrent, high-performance system written in **C** that scrapes music data from a public API, coordinates a multi-threaded **Producer-Consumer** pipeline using synchronization primitives, persists records into **MongoDB** via connection pooling, and serves a live web interface with embedded audio players.

## Architecture & Workflow

1. **The Producer (Web Scraper):** Fetches JSON data from the Deezer API using `libcurl` and parses it via `cJSON`.
2. **Safe Concurrency Pipeline:** Implements a bounded buffer queue protected by **POSIX semaphores** (`sem_t`) and **mutexes** (`pthread_mutex_t`) to safely pass data from the producer to multiple consumer threads.
3. **Database Persistence (MongoDB Pool):** Worker consumer threads (`CONSUMERS = 5`) concurrently pop items from the queue and store song metadata into MongoDB utilizing a `mongoc_client_pool` for efficient connection management.
4. **HTTP Streaming Server:** A multi-threaded TCP socket server that listens on port `8080`, queries the database, and dynamically generates a dark-mode HTML page with playable audio elements (`<audio controls>`).

## Tech Stack

- **Language:** C (POSIX-compliant)
- **Networking & Web:** TCP/IP Sockets, `libcurl` (HTTP requests)
- **Concurrency & Synchronization:** POSIX Threads (`pthreads`), Semaphores (`<semaphore.h>`), Mutexes
- **Parsing & Database:** `cJSON`, MongoDB C Driver (`libmongoc`) & BSON
- **Build System:** GCC / Make

---

## Prerequisites & Dependencies

To compile and run this project on a UNIX/Linux environment, you need the following libraries installed:

1. **MongoDB C Driver & BSON:**
   ```bash
   sudo apt-get install libmongoc-dev libbson-dev
Libcurl:

Bash
sudo apt-get install libcurl4-openssl-dev
cJSON:

Bash
sudo apt-get install libcjson-dev
Ensure MongoDB is running locally:

Bash
mongod --dbpath /path/to/your/db
Compilation & Usage
1. Run the Data Scraper & Ingester
Compile and run the scraper to fetch data from the web and populate your MongoDB database:

Bash
gcc scraper.c -o scraper $(pkg-config --cflags --libs libmongoc-1.0 libbson-1.0 libcurl) -lcjson -lpthread
./scraper
2. Run the Web Server
In a separate terminal, compile and run the HTTP server:

Bash
gcc server.c -o server $(pkg-config --cflags --libs libmongoc-1.0 libbson-1.0) -lpthread
./server
3. Open in Your Browser
Open your web browser and navigate to:

Plaintext
http://localhost:8080
You will see your custom terminal-styled music player populated with the scraped songs and working audio controls!

Author
Christopoulos Nikolas