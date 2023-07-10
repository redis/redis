#!/bin/sh -ue

REDIS_SERVER=${REDIS_SERVER:-redis-server}
REDIS_PORT=${REDIS_PORT:-56379}
REDIS_SSL_PORT=${REDIS_SSL_PORT:-56443}
TEST_SSL=${TEST_SSL:-0}
SKIPS_AS_FAILS=${SKIPS_AS_FAILS:-0}
ENABLE_DEBUG_CMD=
SSL_TEST_ARGS=
SKIPS_ARG=${SKIPS_ARG:-}
REDIS_DOCKER=${REDIS_DOCKER:-}

# We need to enable the DEBUG command for redis-server >= 7.0.0
REDIS_MAJOR_VERSION="$(redis-server --version|awk -F'[^0-9]+' '{ print $2 }')"
if [ "$REDIS_MAJOR_VERSION" -gt "6" ]; then
    ENABLE_DEBUG_CMD="enable-debug-command local"
fi

tmpdir=$(mktemp -d)
PID_FILE=${tmpdir}/hiredis-test-redis.pid
SOCK_FILE=${tmpdir}/hiredis-test-redis.sock

if [ "$TEST_SSL" = "1" ]; then
    SSL_CA_CERT=${tmpdir}/ca.crt
    SSL_CA_KEY=${tmpdir}/ca.key
    SSL_CERT=${tmpdir}/redis.crt
    SSL_KEY=${tmpdir}/redis.key

    openssl genrsa -out ${tmpdir}/ca.key 4096
    openssl req \
        -x509 -new -nodes -sha256 \
        -key ${SSL_CA_KEY} \
        -days 3650 \
        -subj '/CN=Hiredis Test CA' \
        -out ${SSL_CA_CERT}
    openssl genrsa -out ${SSL_KEY} 2048
    openssl req \
        -new -sha256 \
        -key ${SSL_KEY} \
        -subj '/CN=Hiredis Test Cert' | \
        openssl x509 \
            -req -sha256 \
            -CA ${SSL_CA_CERT} \
            -CAkey ${SSL_CA_KEY} \
            -CAserial ${tmpdir}/ca.txt \
            -CAcreateserial \
            -days 365 \
            -out ${SSL_CERT}

    SSL_TEST_ARGS="--ssl-host 127.0.0.1 --ssl-port ${REDIS_SSL_PORT} --ssl-ca-cert ${SSL_CA_CERT} --ssl-cert ${SSL_CERT} --ssl-key ${SSL_KEY}"
fi

cleanup() {
  if [ -n "${REDIS_DOCKER}" ] ; then
    docker kill redis-test-server
  else
    set +e
    kill $(cat ${PID_FILE})
  fi
  rm -rf ${tmpdir}
}
trap cleanup INT TERM EXIT

# base config
cat > ${tmpdir}/redis.conf <<EOF
pidfile ${PID_FILE}
port ${REDIS_PORT}
unixsocket ${SOCK_FILE}
unixsocketperm 777
EOF

# if not running in docker add these:
if [ ! -n "${REDIS_DOCKER}" ]; then
cat >> ${tmpdir}/redis.conf <<EOF
daemonize yes
${ENABLE_DEBUG_CMD}
bind 127.0.0.1
EOF
fi

# if doing ssl, add these
if [ "$TEST_SSL" = "1" ]; then
    cat >> ${tmpdir}/redis.conf <<EOF
tls-port ${REDIS_SSL_PORT}
tls-ca-cert-file ${SSL_CA_CERT}
tls-cert-file ${SSL_CERT}
tls-key-file ${SSL_KEY}
EOF
fi

echo ${tmpdir}
cat ${tmpdir}/redis.conf
if [ -n "${REDIS_DOCKER}" ] ; then
    chmod a+wx ${tmpdir}
    chmod a+r ${tmpdir}/*
    docker run -d --rm --name redis-test-server \
        -p ${REDIS_PORT}:${REDIS_PORT} \
        -p ${REDIS_SSL_PORT}:${REDIS_SSL_PORT} \
        -v ${tmpdir}:${tmpdir} \
        ${REDIS_DOCKER} \
        redis-server ${tmpdir}/redis.conf
else
    ${REDIS_SERVER} ${tmpdir}/redis.conf
fi
# Wait until we detect the unix socket
echo waiting for server
while [ ! -S "${SOCK_FILE}" ]; do sleep 1; done

# Treat skips as failures if directed
[ "$SKIPS_AS_FAILS" = 1 ] && SKIPS_ARG="${SKIPS_ARG} --skips-as-fails"

${TEST_PREFIX:-} ./hiredis-test -h 127.0.0.1 -p ${REDIS_PORT} -s ${SOCK_FILE} ${SSL_TEST_ARGS} ${SKIPS_ARG}
