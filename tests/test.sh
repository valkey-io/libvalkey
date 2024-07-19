#!/bin/sh -ue
#
check_executable() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Error: $1 is not found or not executable."
        exit 1
    fi
}

VALKEY_SERVER=${VALKEY_SERVER:-valkey-server}
VALKEY_PORT=${VALKEY_PORT:-56379}
VALKEY_SSL_PORT=${VALKEY_SSL_PORT:-56443}
TEST_SSL=${TEST_SSL:-0}
SKIPS_AS_FAILS=${SKIPS_AS_FAILS:-0}
ENABLE_DEBUG_CMD=
SSL_TEST_ARGS=
SKIPS_ARG=${SKIPS_ARG:-}
VALKEY_DOCKER=${VALKEY_DOCKER:-}
TEST_RDMA=${TEST_RDMA:-0}
RDMA_TEST_ARGS=

check_executable "$VALKEY_SERVER"

# Enable debug command for redis-server >= 7.0.0 or any version of valkey-server.
VALKEY_MAJOR_VERSION="$("$VALKEY_SERVER" --version|awk -F'[^0-9]+' '{ print $2 }')"
if [ "$VALKEY_MAJOR_VERSION" -gt "6" ]; then
    ENABLE_DEBUG_CMD="enable-debug-command local"
fi

tmpdir=$(mktemp -d)
PID_FILE=${tmpdir}/libvalkey-test-valkey.pid
SOCK_FILE=${tmpdir}/libvalkey-test-valkey.sock
CONF_FILE=${tmpdir}/valkey.conf

if [ "$TEST_SSL" = "1" ]; then
    SSL_CA_CERT=${tmpdir}/ca.crt
    SSL_CA_KEY=${tmpdir}/ca.key
    SSL_CERT=${tmpdir}/valkey.crt
    SSL_KEY=${tmpdir}/valkey.key

    openssl genrsa -out ${tmpdir}/ca.key 4096
    openssl req \
        -x509 -new -nodes -sha256 \
        -key ${SSL_CA_KEY} \
        -days 3650 \
        -subj '/CN=Libvalkey Test CA' \
        -out ${SSL_CA_CERT}
    openssl genrsa -out ${SSL_KEY} 2048
    openssl req \
        -new -sha256 \
        -key ${SSL_KEY} \
        -subj '/CN=Libvalkey Test Cert' | \
        openssl x509 \
            -req -sha256 \
            -CA ${SSL_CA_CERT} \
            -CAkey ${SSL_CA_KEY} \
            -CAserial ${tmpdir}/ca.txt \
            -CAcreateserial \
            -days 365 \
            -out ${SSL_CERT}

    SSL_TEST_ARGS="--ssl-host 127.0.0.1 --ssl-port ${VALKEY_SSL_PORT} --ssl-ca-cert ${SSL_CA_CERT} --ssl-cert ${SSL_CERT} --ssl-key ${SSL_KEY}"
fi

cleanup() {
  if [ -n "${VALKEY_DOCKER}" ] ; then
    docker kill valkey-test-server
  else
    set +e
    kill $(cat ${PID_FILE})
  fi
  rm -rf ${tmpdir}
}
trap cleanup INT TERM EXIT

# base config
cat > ${CONF_FILE} <<EOF
pidfile ${PID_FILE}
port ${VALKEY_PORT}
unixsocket ${SOCK_FILE}
unixsocketperm 777
EOF

# if not running in docker add these:
if [ ! -n "${VALKEY_DOCKER}" ]; then
cat >> ${CONF_FILE} <<EOF
daemonize yes
${ENABLE_DEBUG_CMD}
bind 127.0.0.1
EOF
fi

# if doing ssl, add these
if [ "$TEST_SSL" = "1" ]; then
    cat >> ${CONF_FILE} <<EOF
tls-port ${VALKEY_SSL_PORT}
tls-ca-cert-file ${SSL_CA_CERT}
tls-cert-file ${SSL_CERT}
tls-key-file ${SSL_KEY}
EOF
fi

# if doing RDMA, add these
if [ "$TEST_RDMA" = "1" ]; then
    cat >> ${CONF_FILE} <<EOF
loadmodule ${VALKEY_RDMA_MODULE} bind=${VALKEY_RDMA_ADDR} port=${VALKEY_PORT}
protected-mode no
EOF
RDMA_TEST_ARGS="--rdma-addr ${VALKEY_RDMA_ADDR}"
fi

echo ${tmpdir}
cat ${CONF_FILE}
if [ -n "${VALKEY_DOCKER}" ] ; then
    chmod a+wx ${tmpdir}
    chmod a+r ${tmpdir}/*
    docker run -d --rm --name valkey-test-server \
        -p ${VALKEY_PORT}:${VALKEY_PORT} \
        -p ${VALKEY_SSL_PORT}:${VALKEY_SSL_PORT} \
        -v ${tmpdir}:${tmpdir} \
        ${VALKEY_DOCKER} \
        ${VALKEY_SERVER} ${CONF_FILE}
else
    ${VALKEY_SERVER} ${CONF_FILE}
fi
# Wait until we detect the unix socket
echo waiting for server
while [ ! -S "${SOCK_FILE}" ]; do
    2>&1 echo "Waiting for server..."
    ps aux|grep valkey-server
    sleep 1;
done

# Treat skips as failures if directed
[ "$SKIPS_AS_FAILS" = 1 ] && SKIPS_ARG="${SKIPS_ARG} --skips-as-fails"

${TEST_PREFIX:-} ./client_test -h 127.0.0.1 -p ${VALKEY_PORT} -s ${SOCK_FILE} ${SSL_TEST_ARGS} ${SKIPS_ARG} ${RDMA_TEST_ARGS}
