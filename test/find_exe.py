#!/usr/bin/env python
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

import os
import sys

from utils import Error

IS_WINDOWS = sys.platform == 'win32'
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT_DIR = os.path.dirname(SCRIPT_DIR)
DEFAULT_SEXPR_WASM_EXE = os.path.join(REPO_ROOT_DIR, 'out', 'sexpr-wasm')
BUILT_D8_EXE = os.path.join(REPO_ROOT_DIR, 'third_party', 'v8', 'v8', 'out',
                            'Release', 'd8')
DOWNLOAD_D8_EXE = os.path.join(REPO_ROOT_DIR, 'out', 'd8')
BUILT_SM_EXE = os.path.join(REPO_ROOT_DIR, 'third_party', 'gecko-dev', 'js',
                            'src', 'build_OPT.OBJ', 'js', 'src', 'js')
DOWNLOAD_SM_EXE = os.path.join(REPO_ROOT_DIR, 'out', 'js')
DOWNLOAD_CHAKRA_EXE = os.path.join(REPO_ROOT_DIR, 'out', 'ch')
DEFAULT_WASM_WAST_EXE = os.path.join(REPO_ROOT_DIR, 'out', 'wasm-wast')
DEFAULT_WASM_INTERP_EXE = os.path.join(REPO_ROOT_DIR, 'out', 'wasm-interp')


if IS_WINDOWS:
  DEFAULT_SEXPR_WASM_EXE += '.exe'
  BUILT_D8_EXE += '.exe'
  DOWNLOAD_D8_EXE += '.exe'
  BUILT_SM_EXE += '.exe'
  DOWNLOAD_SM_EXE += '.exe'
  DOWNLOAD_CHAKRA_EXE += '.exe'
  DEFAULT_WASM_WAST_EXE += '.exe'
  DEFAULT_WASM_INTERP_EXE += '.exe'


def FindExeWithFallback(name, default_exe_list, override_exe=None):
  result = override_exe
  if result is not None:
    if IS_WINDOWS and os.path.splitext(result)[1] != '.exe':
      result += '.exe'
    if os.path.exists(result):
      return os.path.abspath(result)
    raise Error('%s executable not found.\nsearch path: %s\n' % (name, result))

  for result in default_exe_list:
    if os.path.exists(result):
      return os.path.abspath(result)

  raise Error('%s executable not found.\n%s\n' % (
      name,
      '\n'.join('search path: %s' % path for path in default_exe_list)))


def GetSexprWasmExecutable(override=None):
  return FindExeWithFallback('sexpr-wasm', [DEFAULT_SEXPR_WASM_EXE], override)


def GetWasmWastExecutable(override=None):
  return FindExeWithFallback('wasm-wast', [DEFAULT_WASM_WAST_EXE], override)


def GetWasmInterpExecutable(override=None):
  return FindExeWithFallback('wasm-interp', [DEFAULT_WASM_INTERP_EXE], override)


def GetJSExecutable(override=None):
  return FindExeWithFallback('js',
      [BUILT_D8_EXE, BUILT_SM_EXE, DOWNLOAD_D8_EXE, DOWNLOAD_SM_EXE, DOWNLOAD_CHAKRA_EXE], override)
