redisContext* getRedisContext(const char* host, int port) {
    redisContext* context = redisConnectNonBlock(host, port);
    if (context == NULL) {
        return NULL;
    }
    
    // Wait for connection to complete
    int fd = context->fd;
    struct timeval timeout = {1, 0}; // 1 second timeout
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(fd, &write_fds);
    
    if (select(fd + 1, NULL, &write_fds, NULL, &timeout) > 0 && FD_ISSET(fd, &write_fds)) {
        // Check if connection succeeded
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
            return context;
        }
    }
    
    redisFree(context);
    return NULL;
}