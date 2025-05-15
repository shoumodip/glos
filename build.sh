#!/bin/sh

set -xe

cc $(cat compile_flags.txt) -o glos src/*.c -lLLVM
