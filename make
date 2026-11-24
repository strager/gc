#!/usr/bin/env bash

set -e
set -u
set -o pipefail

cc -g gc.c -fPIC -std=c23 -shared -o libgc.so
LD_PRELOAD="${PWD}/libgc.so" figlet hello
