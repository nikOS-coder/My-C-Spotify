all : scraper server

scraper : scraper.c cJSON.c
	gcc scraper.c cJSON.c -o scraper $(shell pkg-config --cflags --libs libmongoc-1.0) -lpthread -lcurl

server : server.c
	gcc server.c -o server $(shell pkg-config --cflags --libs libmongoc-1.0) -lpthread