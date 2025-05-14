#!/bin/sh

set -xe

cc -o glos src/*.c -lLLVM
