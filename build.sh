#!/bin/sh
set -xe
cc `cat compile_flags.txt` -g -o glos src/*.c
