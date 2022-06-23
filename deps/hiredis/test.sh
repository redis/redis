#!/bin/sh -ue

REDIS_SERVER=${REDIS_SERVER:-redis-server}
REDIS_PORT=${REDIS_PORT:-56379}
REDIS_SSL_PORT=${REDIS_SSL_PORT:-56443}
TEST_SSL=${TEST_SSL:-0}
SKIPS_AS_FAILS=${SKIPS_AS_FAILS-:0}
SSL_TEST_ARGS=
SKIPS_ARG=

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
  set +e
  kill $(cat ${PID_FILE})
  rm -rf ${tmpdir}
}
trap cleanup INT TERM EXIT

cat > ${tmpdir}/redis.conf <<EOF
daemonize yes
pidfile ${PID_FILE}
port ${REDIS_PORT}
bind 127.0.0.1
unixsocket ${SOCK_FILE}
EOF

if [ "$TEST_SSL" = "1" ]; then
    cat >> ${tmpdir}/redis.conf <<EOF
tls-port ${REDIS_SSL_PORT}
tls-ca-cert-file ${SSL_CA_CERT}
tls-cert-file ${SSL_CERT}
tls-key-file ${SSL_KEY}
EOF
fi

cat ${tmpdir}/redis.conf
${REDIS_SERVER} ${tmpdir}/redis.conf

# Wait until we detect the unix socket
while [ ! -S "${SOCK_FILE}" ]; do sleep 1; done

# Treat skips as failures if directed
[ "$SKIPS_AS_FAILS" = 1 ] && SKIPS_ARG="--skips-as-fails"

${TEST_PREFIX:-} ./hiredis-test -h 127.0.0.1 -p ${REDIS_PORT} -s ${SOCK_FILE} ${SSL_TEST_ARGS} ${SKIPS_ARG}
