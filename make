#!/usr/bin/env bash

set -e
set -u
set -o pipefail

cc -g gc.c -pthread -fPIC -std=c23 -shared -o libgc.so -Wall -Wextra -pedantic -fno-strict-aliasing -Wno-unused-function
LD_PRELOAD="${PWD}/libgc.so" figlet hello
