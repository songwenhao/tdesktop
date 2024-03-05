#!/bin/sh

# Copyright 2019-2020 Yury Gribov
#
# The MIT License (MIT)
# 
# Use of this source code is governed by MIT license that can be
# found in the LICENSE.txt file.

# This test checks that exceptions are successfully propagated
# through implib wrappers.

set -eu

cd $(dirname $0)

if test -n "${1:-}"; then
  ARCH="$1"
fi

. ../common.sh

export LD_LIBRARY_PATH=.:${LD_LIBRARY_PATH:-}

$CXX $CFLAGS -shared -fPIC interposed.cpp -o libinterposed.so
$CXX $CFLAGS main.cpp -L. -linterposed
$INTERP ./a.out 2>&1 | tee ref.log

${PYTHON:-} ../../implib-gen.py -q --target $TARGET libinterposed.so
$CXX $CFLAGS -Wno-deprecated main.cpp libinterposed.so.tramp.S libinterposed.so.init.c $LIBS
$INTERP ./a.out 2>&1 | tee new.log

if ! diff ref.log new.log; then
  echo "Exceptions do NOT propagate through implibs"
  exit 1
fi

echo SUCCESS
