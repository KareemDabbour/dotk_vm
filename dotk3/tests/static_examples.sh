#!/bin/sh
# The same program must behave identically under the legacy gradual pipeline and --static.
set -eu
bin=$1
root=$(dirname "$0")/..
for name in collection_queries core_services core inheritance language_api library list_methods map_methods model_types object_lifecycle operators procedural_loot string_methods tetris_model traits; do
    file="$root/examples/$name.k3"
    legacy=$("$bin" "$file" </dev/null 2>&1) || true
    typed=$("$bin" --static "$file" </dev/null 2>&1) || true
    if [ "$legacy" != "$typed" ]; then
        echo "static output differs from legacy for $name" >&2
        printf '%s\n' "$legacy" > /tmp/legacy.$$; printf '%s\n' "$typed" > /tmp/typed.$$
        diff /tmp/legacy.$$ /tmp/typed.$$ >&2 || true; rm -f /tmp/legacy.$$ /tmp/typed.$$
        exit 1
    fi
done
echo 'static examples: PASS'
