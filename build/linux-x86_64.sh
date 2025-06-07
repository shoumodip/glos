#!/bin/sh
set -xe
cc `cat compile_flags.txt` -g -o glos src/*.c src/token/*.c src/lexer/*.c src/node/*.c src/parser/*.c src/checker/*.c src/compiler/*.c -L./src/qbe -lqbe-linux-x86_64
