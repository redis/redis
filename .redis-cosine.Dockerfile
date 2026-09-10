FROM rust:1-bookworm
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential clang cmake git pkg-config python3 python3-pip \
    libssl-dev ca-certificates curl doxygen unzip && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /work
COPY . /work
RUN make distclean && make -j8 MALLOC=libc
RUN make -C modules/redisearch/src build COORD=oss IGNORE_MISSING_DEPS=1
RUN cp /work/src/redis-server /usr/local/bin/redis-server-local && \
    REDISEARCH_SO=$(find /work/modules/redisearch/src/bin -path '*/search-community/redisearch.so' | head -1) && \
    test -n "$REDISEARCH_SO" && \
    echo "Using redisearch module: $REDISEARCH_SO" && \
    cp "$REDISEARCH_SO" /usr/local/lib/redisearch.so
EXPOSE 3781
CMD ["redis-server-local","--port","3781","--save","","--appendonly","no","--loadmodule","/usr/local/lib/redisearch.so"]
