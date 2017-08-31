#!/bin/bash
set -e -x
clang++ --std=c++14 -o oterm -lutil term.cc
