#!/bin/sh
# Build gcon-vm: bin/gcon-vm, from src/*.c. Needs only a C compiler
# ($CC, default cc). Never uses the network.
set -eu
cd "$(dirname "$0")"
CC=${CC:-cc}
mkdir -p bin
$CC -std=gnu11 -O2 -Wall -Wextra -o bin/gcon-vm src/*.c
