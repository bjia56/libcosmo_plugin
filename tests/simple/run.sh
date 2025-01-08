#!/bin/bash

set -ex

# Build separate binaries
cosmoc++ -o cosmo.com -I../../include main.cpp
g++ -shared -fPIC -o native.so -I../../include main.cpp

./cosmo.com