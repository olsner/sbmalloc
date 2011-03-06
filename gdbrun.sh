#!/bin/sh
malloc_so=`dirname $0`/malloc_debug.so
gdb -ex "set exec-wrapper env LD_PRELOAD=$malloc_so" --args "$@"
