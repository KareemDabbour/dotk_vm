#!/bin/sh
# Every non-interactive example must type-check cleanly and run to completion.
set -eu
bin=$1
root=$(dirname "$0")/..
for file in "$root"/examples/*.k3; do
    case "$file" in
        */t1.k3|*/t2.k3|*/tcp_lines.k3|*/chat_*|*/browser_server.k3|*/webserver.k3|*/http_client.k3|*/requests.k3|*/input_loop.k3|*/tetris.k3|*/pacman.k3|*/debugger.k3|*/donut_typed.k3) continue ;;
    esac
    if ! "$bin" --steps 0 "$file" </dev/null >/dev/null 2>/tmp/static-example.$$; then
        echo "example failed: $file" >&2; cat /tmp/static-example.$$ >&2; rm -f /tmp/static-example.$$; exit 1
    fi
done
rm -f /tmp/static-example.$$
for file in "$root"/examples/*.k3; do
    case "$file" in */t1.k3) continue ;; esac
    "$bin" --check "$file" >/dev/null 2>/tmp/static-example.$$ || { echo "example does not type-check: $file" >&2; cat /tmp/static-example.$$ >&2; rm -f /tmp/static-example.$$; exit 1; }
done
rm -f /tmp/static-example.$$
echo 'static examples: PASS'
