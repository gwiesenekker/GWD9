#!/bin/sh
set -eu

program=./gwd9

starting=$($program 'W:W31-50:B1-20')
printf '%s\n' "$starting" | grep -F 'FEN: W:W31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50:B1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20' >/dev/null
printf '%s\n' "$starting" | grep -F 'Total: 9 legal moves' >/dev/null

capture=$($program 'W:W32:B27,17')
printf '%s\n' "$capture" | grep -F '32x12 (captures 17 27)' >/dev/null
printf '%s\n' "$capture" | grep -F 'Total: 1 legal move' >/dev/null

king=$($program 'W:WK36:B22')
printf '%s\n' "$king" | grep -F '36x' >/dev/null
printf '%s\n' "$king" | grep -F '(captures 22)' >/dev/null

if $program 'W:W1:B2' >/dev/null 2>&1; then
    echo 'invalid promotion-row man was accepted' >&2
    exit 1
fi

replay=$($program --pop 1 'W:W31-50:B1-20' 31-26 16-21)
printf '%s\n' "$replay" | grep -F 'Moves (1 ply from root): 31-26' >/dev/null
printf '%s\n' "$replay" | grep -F 'FEN: B:W26,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50:B1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20' >/dev/null

$program -j3 'W:W31-50:B1-20' 31-26 16-21 >/dev/null
for index in 0 1 2; do
    test -s "logs/log${index}.txt"
    grep -E '^\[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\] \[thread ' "logs/log${index}.txt" >/dev/null
    grep -F 'root ply: 2' "logs/log${index}.txt" >/dev/null
    grep -F 'Zobrist key:' "logs/log${index}.txt" >/dev/null
    grep -F 'alpha-beta search started' "logs/log${index}.txt" >/dev/null
    grep -F 'search finished:' "logs/log${index}.txt" >/dev/null
    grep -F 'principal variation (' "logs/log${index}.txt" >/dev/null
    grep -F 'TT: probes ' "logs/log${index}.txt" >/dev/null
    grep -F 'iteration depth 1 ' "logs/log${index}.txt" >/dev/null
done

echo 'All tests passed.'
