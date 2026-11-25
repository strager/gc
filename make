#!/usr/bin/env bash

set -e
set -u
set -o pipefail

cc -g procmaps_test.c -o procmaps_test
./procmaps_test

cc -g gc.c -pthread -fPIC -std=c23 -shared -o libgc.so -Wall -Wextra -fno-strict-aliasing -Wno-unused-function
LD_PRELOAD="${PWD}/libgc.so" figlet hello
