#!/bin/bash
# Starts three echo backends and the server with conf/webserv.conf. Ctrl-C stops everything.
BUILD=${1:-build}
cd "$(dirname "$0")/.." || exit 1
pids=""
for spec in "9001 api-1" "9002 api-2" "9003 app"; do
    set -- $spec
    "$BUILD/echo_backend" "$1" "$2" &
    pids="$pids $!"
done
trap 'kill $pids 2>/dev/null; wait' EXIT INT TERM
sleep 0.3
echo "open http://127.0.0.1:8080/   (status: /__status, API: /api/hello, Host route: curl -H 'Host: app.localhost' ...)"
"$BUILD/webserv" -c conf/webserv.conf
