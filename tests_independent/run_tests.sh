#!/bin/bash

set -e

for OPTIONS in "" "-XNoInlineRefcounting" "-XNoEagerCompilation" "-XNoInlineRefcounting -XNoEagerCompilation"; do
  for FILE in *.py; do
    echo "-- pyjit $OPTIONS $FILE"
    ../pyjit $OPTIONS $FILE > output.$FILE.txt
  done
done

echo "-- all tests passed"

rm -f output.*.txt
