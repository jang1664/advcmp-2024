#!/bin/bash

SCRIPT_DIR="$(dirname $0)"

cmake -S $SCRIPT_DIR/project2 -G Ninja -B build
cmake --build build
