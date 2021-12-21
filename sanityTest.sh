#!/bin/bash
function cleanup {
    kill $!
    wait $! 2>/dev/null
}
trap cleanup EXIT
./maock maock.json &>/dev/null &

result=$(./h2loadrunner --config-file=h2load.json -t 1 -c 10 -D 10|grep "requests:")

requests=$(echo $result|cut -d "," -f 1|cut -d ":" -f 2|cut -d " " -f 2)

succeeded=$(echo $result|cut -d "," -f 4|cut -d " " -f 2)

if [ "$requests" -eq "$succeeded" ] && [ "$requests" -ne "0" ]; then
    echo "test pass"
    exit 0
fi
