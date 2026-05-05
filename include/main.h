#ifndef MAIN_H
#define MAIN_H

#include <sys/epoll.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "client_context.h"

#define MAX_EVENTS 64
#define BUF_SIZE 16384

#endif
