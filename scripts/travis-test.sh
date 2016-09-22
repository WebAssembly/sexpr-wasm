#!/bin/bash
#
# Copyright 2016 WebAssembly Community Group participants
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

set -o nounset
set -o errexit

SCRIPT_DIR="$(cd "$(dirname "$0")"; pwd -P)"
source "${SCRIPT_DIR}/travis-common.sh"

log_and_run() {
  echo $*
  exec $*
}

run_tests() {
  (cd ${ROOT_DIR} && log_and_run test/run-tests.py ${RUN_TEST_ARGS} $* --timeout=10)
}

check_and_add_flag() {
  local FLAG=$1
  local NAME=$2
  if [ ! -e ${NAME} ]; then
    echo "${NAME} doesn't exist; skipping test."
    return 1
  fi
  RUN_TEST_ARGS="${RUN_TEST_ARGS} ${FLAG} ${NAME}"
  return 0
}

set_run_test_args() {
  local COMPILER=$1
  local BUILD_TYPE=$2
  local CONFIG=${3:-}

  RUN_TEST_ARGS=""
  local EXE_DIR=out/${COMPILER}/${BUILD_TYPE}/${CONFIG}

  WAST2WASM=${EXE_DIR}/wast2wasm
  WASM2WAST=${EXE_DIR}/wasm2wast
  WASM_INTERP=${EXE_DIR}/wasm-interp

  check_and_add_flag "--wast2wasm-executable" ${WAST2WASM} && \
      check_and_add_flag "--wasm2wast-executable" ${WASM2WAST} && \
      check_and_add_flag "--wasm-interp-executable" ${WASM_INTERP}
}

if [ ${CC} = "gcc" ]; then
  if set_run_test_args gcc Debug no-re2c-bison; then
    run_tests
  fi
fi

for BUILD_TYPE in ${BUILD_TYPES_UPPER}; do
  if [[ -n "${SANITIZER:-}" ]]; then
    if set_run_test_args ${COMPILER} ${BUILD_TYPE} ${SANITIZER}; then
      run_tests
      run_tests -a=--use-libc-allocator
    fi
  else
    if set_run_test_args ${COMPILER} ${BUILD_TYPE}; then
      run_tests
      run_tests -a=--use-libc-allocator
    fi
  fi
done
