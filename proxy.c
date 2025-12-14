#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <signal.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE (1024 * 1024)  // 1 MiB
#define MAX_OBJECT_SIZE (100 * 1024)  // 100 KiB
#define MAXLINE 8192
#define USER_AGENT "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n"    
/* Cache entry structure */                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      
typedef struct cache_entry {
    char *url;
    char *data;
    size_t size;
    time_t last_used;
    struct cache_entry *next;
    struct cache_entry *prev;
} cache_entry_t;

/* Cache structure */
typedef struct {
    cache_entry_t *head;
    cache_entry_t *tail;
    size_t total_size;
    pthread_rwlock_t rwlock;
} cache_t;

/* Global cache */
cache_t cache;

/* Function prototypes */
void *handle_client_thread(void *vargp);
void handle_client(int clientfd);
void parse_url(const char *url, char *host, char *path, int *port);
void init_cache();
cache_entry_t *cache_find(const char *url);
void cache_insert(const char *url, const char *data, size_t size);
void cache_evict();
void cache_update_lru(cache_entry_t *entry);
void cache_free();

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    int listenfd, *clientfdp;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    init_cache();

    listenfd = open_listenfd(argv[1]);

    while (1) {
        clientlen = sizeof(struct sockaddr_storage);
        clientfdp = malloc(sizeof(int));
        *clientfdp = accept(listenfd, (SA *)&clientaddr, &clientlen);
        
        pthread_create(&tid, NULL, handle_client_thread, clientfdp);
        pthread_detach(tid);
    }

    cache_free();
    return 0;
}

/* Thread routine */
void *handle_client_thread(void *vargp) {
    int clientfd = *((int *)vargp);
    free(vargp);
    handle_client(clientfd);
    close(clientfd);
    return NULL;
}

void handle_client(int clientfd) {
    char buf[MAXLINE], method[MAXLINE], url[MAXLINE], version[MAXLINE];
    char host[MAXLINE], path[MAXLINE], request[MAXLINE];
    char *object_buf = NULL;
    size_t object_size = 0;
    int port = 80;
    rio_t rio;

    /* Read request line */
    rio_readinitb(&rio, clientfd);
    if (rio_readlineb(&rio, buf, MAXLINE) <= 0)
        return;

    if (sscanf(buf, "%s %s %s", method, url, version) != 3)
        return;

    /* Only support GET */
    if (strcasecmp(method, "GET") != 0) {
        fprintf(stderr, "Only GET supported\n");
        return;
    }

    /* Check cache first */
    pthread_rwlock_rdlock(&cache.rwlock);
    cache_entry_t *entry = cache_find(url);
    if (entry) {
        Rio_writen(clientfd, entry->data, entry->size);
        pthread_rwlock_unlock(&cache.rwlock);

        /* Upgrade to WRITE lock to modify LRU */
        pthread_rwlock_wrlock(&cache.rwlock);
        cache_update_lru(entry);
        pthread_rwlock_unlock(&cache.rwlock);
        return;
    }
    pthread_rwlock_unlock(&cache.rwlock);

    /* Cache miss - forward request to server */
    parse_url(url, host, path, &port);

    /* Build HTTP/1.0 request */
    snprintf(request, MAXLINE,
             "GET %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "%s"
             "Connection: close\r\n"
             "Proxy-Connection: close\r\n",
             path, host, USER_AGENT);

    /* Forward remaining client request headers */
    char header[MAXLINE];
    while (rio_readlineb(&rio, header, MAXLINE) > 0) {
        if (!strcmp(header, "\r\n"))
            break;

        /* Skip headers we override */
        if (!strncasecmp(header, "Host:", 5) ||
            !strncasecmp(header, "User-Agent:", 11) ||
            !strncasecmp(header, "Connection:", 11) ||
            !strncasecmp(header, "Proxy-Connection:", 17))
            continue;

        strcat(request, header);
    }

    /* End headers */
    strcat(request, "\r\n");

    /* Connect to server */
    char port_str[10];
    snprintf(port_str, sizeof(port_str), "%d", port);
    int serverfd = open_clientfd(host, port_str);
    if (serverfd < 0) {
        fprintf(stderr, "Failed to connect to server %s:%d\n", host, port);
        return;
    }

    /* Send request to server */
    rio_writen(serverfd, request, strlen(request));

    /* Allocate buffer for potential caching */
    object_buf = malloc(MAX_OBJECT_SIZE);
    object_size = 0;
    int cacheable = 1;

    /* Relay response back to client and cache if possible */
    rio_t server_rio;
    rio_readinitb(&server_rio, serverfd);
    ssize_t n;
    while ((n = rio_readnb(&server_rio, buf, MAXLINE)) > 0) {
        rio_writen(clientfd, buf, n);

        /* Accumulate data for caching */
        if (cacheable && object_size + n <= MAX_OBJECT_SIZE) {
            memcpy(object_buf + object_size, buf, n);
            object_size += n;
        } else {
            cacheable = 0;
        }
    }

    /* Cache the object if it's small enough */
    if (cacheable && object_size > 0 && object_size <= MAX_OBJECT_SIZE) {
        cache_insert(url, object_buf, object_size);
    }

    free(object_buf);
    close(serverfd);
}

/* Parse URL into host, path, and port */
void parse_url(const char *url, char *host, char *path, int *port) {
    *port = 80;  // default
    if (strncasecmp(url, "http://", 7) != 0) {
        fprintf(stderr, "Invalid URL: %s\n", url);
        strcpy(host, "");
        strcpy(path, "/");
        return;
    }

    const char *hostbegin = url + 7;
    const char *pathbegin = strchr(hostbegin, '/');
    if (pathbegin) {
        strncpy(path, pathbegin, MAXLINE-1);
        path[MAXLINE-1] = '\0';
    } else {
        strcpy(path, "/");
        pathbegin = hostbegin + strlen(hostbegin);
    }

    char hostport[MAXLINE] = {0};
    strncpy(hostport, hostbegin, pathbegin - hostbegin);
    hostport[pathbegin - hostbegin] = '\0';

    char *colon = strchr(hostport, ':');
    if (colon) {
        *colon = '\0';
        int tmp = atoi(colon + 1);
        if (tmp > 0) *port = tmp;
    }

    strncpy(host, hostport, MAXLINE-1);
    host[MAXLINE-1] = '\0';
}

/* Initialize cache */
void init_cache() {
    cache.head = NULL;
    cache.tail = NULL;
    cache.total_size = 0;
    pthread_rwlock_init(&cache.rwlock, NULL);
}

/* Find entry in cache (caller must hold read lock) */
cache_entry_t *cache_find(const char *url) {
    cache_entry_t *curr = cache.head;
    while (curr) {
        if (strcmp(curr->url, url) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

/* Update LRU ordering (caller must hold write lock) */
void cache_update_lru(cache_entry_t *entry) {
    entry->last_used = time(NULL);
    
    /* Move to front of list */
    if (entry == cache.head)
        return;
    
    /* Remove from current position */
    if (entry->prev)
        entry->prev->next = entry->next;
    if (entry->next)
        entry->next->prev = entry->prev;
    if (entry == cache.tail)
        cache.tail = entry->prev;
    
    /* Insert at head */
    entry->prev = NULL;
    entry->next = cache.head;
    if (cache.head)
        cache.head->prev = entry;
    cache.head = entry;
    if (!cache.tail)
        cache.tail = entry;
}

/* Insert entry into cache */
void cache_insert(const char *url, const char *data, size_t size) {
    pthread_rwlock_wrlock(&cache.rwlock);
    
    /* Check if already in cache */
    if (cache_find(url)) {
        pthread_rwlock_unlock(&cache.rwlock);
        return;
    }
    
    /* Evict entries if necessary */
    while (cache.total_size + size > MAX_CACHE_SIZE && cache.tail) {
        cache_evict();
    }
    
    /* Create new entry */
    cache_entry_t *entry = malloc(sizeof(cache_entry_t));
    entry->url = malloc(strlen(url) + 1);
    strcpy(entry->url, url);
    entry->data = malloc(size);
    memcpy(entry->data, data, size);
    entry->size = size;
    entry->last_used = time(NULL);
    entry->prev = NULL;
    entry->next = cache.head;
    
    /* Insert at head */
    if (cache.head)
        cache.head->prev = entry;
    cache.head = entry;
    if (!cache.tail)
        cache.tail = entry;
    
    cache.total_size += size;
    
    pthread_rwlock_unlock(&cache.rwlock);
}

/* Evict LRU entry (caller must hold write lock) */
void cache_evict() {
    if (!cache.tail)
        return;
    
    cache_entry_t *victim = cache.tail;
    
    /* Remove from list */
    if (victim->prev)
        victim->prev->next = NULL;
    cache.tail = victim->prev;
    if (victim == cache.head)
        cache.head = NULL;
    
    cache.total_size -= victim->size;
    
    /* Free memory */
    free(victim->url);
    free(victim->data);
    free(victim);
}

/* Free entire cache */
void cache_free() {
    pthread_rwlock_wrlock(&cache.rwlock);
    cache_entry_t *curr = cache.head;
    while (curr) {
        cache_entry_t *next = curr->next;
        free(curr->url);
        free(curr->data);
        free(curr);
        curr = next;
    }
    pthread_rwlock_unlock(&cache.rwlock);
    pthread_rwlock_destroy(&cache.rwlock);
}
