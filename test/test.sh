#!/usr/bin/env bash
args=(
    test/test_child --timeout 2 --rc 2
    ---
    test/test_child --timeout 30
    ---
    test/test_child --timeout 30 --ignore-sigterm
    ---
    test/test_child --timeout 4 --call /bin/false
    ---
    test/test_child --timeout 2 --call test/test_child --exec sleep 5
    ---
)
echo "------------------"
./muinit --- "${args[@]}" &
pid=$!
sleep 0.2
pstree -U -a -p -g -T $pid 2>/dev/null
wait $pid
res=$?
echo "------------------"
echo "Test exited with $res"
