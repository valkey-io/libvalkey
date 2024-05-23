#!/bin/sh -ue

VALKEY_SERVER=${VALKEY_SERVER:-valkey-server}
VALKEY_PORT=${VALKEY_PORT:-56379}
VALKEY_SSL_PORT=${VALKEY_SSL_PORT:-56443}
TEST_SSL=${TEST_SSL:-0}
SKIPS_AS_FAILS=${SKIPS_AS_FAILS:-0}
ENABLE_DEBUG_CMD=
SSL_TEST_ARGS=
SKIPS_ARG=${SKIPS_ARG:-}
VALKEY_DOCKER=${VALKEY_DOCKER:-}

# We need to enable the DEBUG command for valkey-server >= 7.0.0
VALKEY_MAJOR_VERSION="$(valkey-server --version|awk -F'[^0-9]+' '{ print $2 }')"
if [ "$VALKEY_MAJOR_VERSION" -gt "6" ]; then
    ENABLE_DEBUG_CMD="enable-debug-command local"
fi

tmpdir=$(mktemp -d)
PID_FILE=${tmpdir}/libvalkey-test-valkey.pid
SOCK_FILE=${tmpdir}/libvalkey-test-valkey.sock

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
cat > ${tmpdir}/valkey.comf <<EOF
pidfile ${PID_FILE}
port ${VALKEY_PORT}
unixsocket ${SOCK_FILE}
unixsocketperm 777
EOF

# if not running in docker add these:
if [ ! -n "${VALKEY_DOCKER}" ]; then
cat >> ${tmpdir}/valkey.comf <<EOF
daemonize yes
${ENABLE_DEBUG_CMD}
bind 127.0.0.1
EOF
fi

# if doing ssl, add these
if [ "$TEST_SSL" = "1" ]; then
    cat >> ${tmpdir}/valkey.comf <<EOF
tls-port ${VALKEY_SSL_PORT}
tls-ca-cert-file ${SSL_CA_CERT}
tls-cert-file ${SSL_CERT}
tls-key-file ${SSL_KEY}
EOF
fi

echo ${tmpdir}
cat ${tmpdir}/valkey.comf
if [ -n "${VALKEY_DOCKER}" ] ; then
    chmod a+wx ${tmpdir}
    chmod a+r ${tmpdir}/*
    docker run -d --rm --name valkey-test-server \
        -p ${VALKEY_PORT}:${VALKEY_PORT} \
        -p ${VALKEY_SSL_PORT}:${VALKEY_SSL_PORT} \
        -v ${tmpdir}:${tmpdir} \
        ${VALKEY_DOCKER} \
        valkey-server ${tmpdir}/valkey.comf
else
    ${VALKEY_SERVER} ${tmpdir}/valkey.comf
fi
# Wait until we detect the unix socket
echo waiting for server
while [ ! -S "${SOCK_FILE}" ]; do sleep 1; done

# Treat skips as failures if directed
[ "$SKIPS_AS_FAILS" = 1 ] && SKIPS_ARG="${SKIPS_ARG} --skips-as-fails"

${TEST_PREFIX:-} ./libvalkey-test -h 127.0.0.1 -p ${VALKEY_PORT} -s ${SOCK_FILE} ${SSL_TEST_ARGS} ${SKIPS_ARG}
