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

import argparse
import difflib
import os
import re
import shutil
import sys
import tempfile

import find_exe
import findtests
import utils
from utils import Error

OK = 0
ERROR = 1
SKIPPED = 2


def FilesAreEqual(filename1, filename2, verbose=False):
  try:
    with open(filename1, 'rb') as file1:
      data1 = file1.read()

    with open(filename2, 'rb') as file2:
      data2 = file2.read()
  except OSError as e:
    return (ERROR, str(e))

  if data1 != data2:
    msg = 'files differ'
    if verbose:
      hexdump1 = utils.Hexdump(data1)
      hexdump2 = utils.Hexdump(data2)
      diff_lines = []
      for line in (difflib.unified_diff(
          hexdump1, hexdump2, fromfile=filename1, tofile=filename2)):
        diff_lines.append(line)
      msg += ''.join(diff_lines)
    msg += '\n'
    return (ERROR, msg)
  return (OK, '')


def TwoRoundtrips(sexpr_wasm, wasm_wast, out_dir, filename, verbose):
  basename = os.path.basename(filename)
  basename_noext = os.path.splitext(basename)[0]
  wasm1_file = os.path.join(out_dir, basename_noext + '-1.wasm')
  wast2_file = os.path.join(out_dir, basename_noext + '-2.wast')
  wasm3_file = os.path.join(out_dir, basename_noext + '-3.wasm')
  try:
    sexpr_wasm.RunWithArgs('-o', wasm1_file, filename)
  except Error as e:
    # if the file doesn't parse properly, just skip it (it may be a "bad-*"
    # test)
    return (SKIPPED, None)
  try:
    wasm_wast.RunWithArgs('-o', wast2_file, wasm1_file)
    sexpr_wasm.RunWithArgs('-o', wasm3_file, wast2_file)
  except Error as e:
    return (ERROR, str(e))
  return FilesAreEqual(wasm1_file, wasm3_file, verbose)


def OneRoundtripToStdout(sexpr_wasm, wasm_wast, out_dir, filename, verbose):
  basename = os.path.basename(filename)
  basename_noext = os.path.splitext(basename)[0]
  wasm_file = os.path.join(out_dir, basename_noext + '.wasm')
  try:
    sexpr_wasm.RunWithArgs('-o', wasm_file, filename)
  except Error as e:
    # if the file doesn't parse properly, just skip it (it may be a "bad-*"
    # test)
    return (SKIPPED, None)
  try:
    wasm_wast.RunWithArgs(wasm_file)
  except Error as e:
    return (ERROR, str(e))
  return (OK, '')


def main(args):
  parser = argparse.ArgumentParser()
  parser.add_argument('-v', '--verbose', help='print more diagnotic messages.',
                      action='store_true')
  parser.add_argument('-o', '--out-dir', metavar='PATH',
                      help='output directory for files.')
  parser.add_argument('-e', '--wast2wasm-executable', metavar='PATH',
                      help='set the wast2wasm executable to use.')
  parser.add_argument('--wasm2wast-executable', metavar='PATH',
                      help='set the wasm2wast executable to use.')
  parser.add_argument('--stdout', action='store_true',
                      help='do one roundtrip and write wast output to stdout')
  parser.add_argument('--no-error-cmdline',
                      help='don\'t display the subprocess\'s commandline when' +
                          ' an error occurs', dest='error_cmdline',
                      action='store_false')
  parser.add_argument('--print-cmd', help='print the commands that are run.',
                      action='store_true')
  parser.add_argument('--use-libc-allocator', action='store_true')
  parser.add_argument('--debug-names', action='store_true')
  parser.add_argument('--generate-names', action='store_true')
  parser.add_argument('file', help='test file.')
  options = parser.parse_args(args)

  sexpr_wasm = utils.Executable(
      find_exe.GetSexprWasmExecutable(options.sexpr_wasm_executable),
      error_cmdline=options.error_cmdline)
  sexpr_wasm.AppendOptionalArgs({
    '--debug-names': options.debug_names,
    '--use-libc-allocator': options.use_libc_allocator
  })

  wasm_wast = utils.Executable(
      find_exe.GetWasmWastExecutable(options.wasm_wast_executable),
      error_cmdline=options.error_cmdline)
  wasm_wast.AppendOptionalArgs({
    '--debug-names': options.debug_names,
    '--generate-names': options.generate_names,
    '--use-libc-allocator': options.use_libc_allocator
  })

  sexpr_wasm.verbose = options.print_cmd
  wasm_wast.verbose = options.print_cmd

  with utils.TempDirectory(options.out_dir, 'roundtrip-') as out_dir:
    filename = options.file
    if options.stdout:
      result, msg = OneRoundtripToStdout(sexpr_wasm, wasm_wast, out_dir,
                                         filename, options.verbose)
    else:
      result, msg = TwoRoundtrips(sexpr_wasm, wasm_wast, out_dir,
                                  filename, options.verbose)
    if result == ERROR:
      sys.stderr.write(msg)
    return result


if __name__ == '__main__':
  try:
    sys.exit(main(sys.argv[1:]))
  except Error as e:
    sys.stderr.write(str(e) + '\n')
    sys.exit(1)
