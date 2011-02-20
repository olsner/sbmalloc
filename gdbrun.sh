#!/bin/sh
malloc_so=`dirname $0`/malloc_debug.so
gdb -ex "set environment LD_PRELOAD=$malloc_so" --args "$@"
