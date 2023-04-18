#!/bin/bash

function cleanup()
{
    pkill -f openudsf
    pkill -f maock
    pkill -f h2loadrunner
}

cp ../openudsf/* ./ >/dev/null 2>&1

openudsf_scripts="schema_sanity.json record_sanity.json	block_sanity.json search_filter_sanity.json	subscribe_sanity.json timer_crud_sanity.json"

echo "starting maock to receive the notifications of data change and timer expiry"
./maock notif.json &

echo "starting in-memory udsf"
./openudsf &

sleep 5

for each in $openudsf_scripts
do
  echo "running "$each
  ./h2loadrunner --config-file=$each > output 2>&1
  grep "0 failed, 0 errored, 0 timeout" output >/dev/null 2>&1
  if [ $? -ne 0 ];then
    echo "$each failed"
    tail output
  else
    echo "$each passed"
  fi
done

sleep 15 # to wait for timer to expire and send notif to maock

pkill -f openudsf
pkill -f maock

grep "200OK-to-Timer-Notif" maock.output | grep "1" >/dev/null 2>&1
if [ $? -ne 0 ];then
  echo "timer notif failed"
else
  echo "timer notif passed"
fi

grep "204-To-Record-Notif" maock.output | grep "4" >/dev/null 2>&1
if [ $? -ne 0 ];then
  echo "record notif failed"
else
  echo "record notif passed"
fi

trap cleanup EXIT
