#!/bin/sh

# Usage: $0 /path/to/clusterclient-binary

clientprog=${1:-./clusterclient}
testname=set-get-test

# Install the readiness handler before starting the simulated server.
ready=0
trap 'ready=1' CONT

# Start simulated server
timeout 5s ./simulated-valkey.pl -p 7400 -d --sigcont "$$" <<'EOF' &
EXPECT CONNECT
EXPECT ["CLUSTER", "SLOTS"]
SEND [[0, 16383, ["127.0.0.1", 7400, "nodeid123"]]]
EXPECT ["SET", "foo", "bar"]
SEND +OK
EXPECT ["GET", "foo"]
SEND "bar"
EXPECT CLOSE
EOF
server=$!

# The server sends CONT after listen() succeeds.
while [ "$ready" -eq 0 ] && kill -0 "$server" 2>/dev/null; do
    sleep 0.05
done
trap - CONT
if [ "$ready" -eq 0 ]; then
    wait "$server"
    echo "Simulated server exited before becoming ready"
    exit 1
fi

# Run client
timeout 3s "$clientprog" 127.0.0.1:7400 > "$testname.out" <<'EOF'
SET foo bar
GET foo
EOF
clientexit=$?

# Wait for server to exit
wait $server; serverexit=$?

# Check exit statuses
if [ $serverexit -ne 0 ]; then
    echo "Simulated server exited with status $serverexit"
    exit $serverexit
fi
if [ $clientexit -ne 0 ]; then
    echo "$clientprog exited with status $clientexit"
    exit $clientexit
fi

# Check the output from clusterclient
printf 'OK\nbar\n' | cmp "$testname.out" - || exit 99

# Clean up
rm "$testname.out"
