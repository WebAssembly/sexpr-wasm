#!/usr/bin/env python3
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
import fnmatch
import multiprocessing
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import threading
import time

import find_exe
from utils import Error

IS_WINDOWS = sys.platform == 'win32'
TEST_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT_DIR = os.path.dirname(TEST_DIR)
OUT_DIR = os.path.join(REPO_ROOT_DIR, 'out')
DEFAULT_TIMEOUT = 120    # seconds
SLOW_TIMEOUT_MULTIPLIER = 3

# default configurations for tests
TOOLS = {
    'wat2wasm': [
        ('RUN', '%(wat2wasm)s'),
        ('ARGS', ['%(in_file)s', '-o', '%(out_dir)s/out.wasm']),
        ('VERBOSE-ARGS', ['-v']),
    ],
    'wast2json': [
        ('RUN', '%(wast2json)s'),
        ('ARGS', ['%(in_file)s', '-o', '%(out_dir)s/out.json']),
        ('VERBOSE-ARGS', ['-v']),
    ],
    'wat-desugar': [
        ('RUN', '%(wat-desugar)s'),
        ('ARGS', ['%(in_file)s']),
    ],
    'run-objdump': [
        ('RUN', '%(wat2wasm)s %(in_file)s -o %(temp_file)s.wasm'),
        ('RUN', '%(wasm-objdump)s -r -d %(temp_file)s.wasm'),
        ('VERBOSE-ARGS', ['-v']),
    ],
    'run-objdump-gen-wasm': [
        ('RUN', '%(gen_wasm_py)s %(in_file)s -o %(temp_file)s.wasm'),
        ('RUN', '%(wasm-objdump)s -r -d %(temp_file)s.wasm'),
        ('VERBOSE-ARGS', ['-v']),
    ],
    'run-objdump-spec': [
        ('RUN', '%(wast2json)s %(in_file)s -o %(temp_file)s.json'),
        # NOTE: wasm files must be passed in manually via ARGS1
        ('RUN', '%(wasm-objdump)s -r -d'),
        ('VERBOSE-ARGS', ['-v']),
    ],
    'run-roundtrip': [
        ('RUN', 'test/run-roundtrip.py'),
        ('ARGS', [
            '%(in_file)s',
            '-v',
            '--bindir=%(bindir)s',
            '--no-error-cmdline',
            '-o',
            '%(out_dir)s',
        ]),
        ('VERBOSE-ARGS', ['--print-cmd', '-v']),
    ],
    'run-interp': [
        ('RUN', '%(wat2wasm)s %(in_file)s -o %(temp_file)s.wasm'),
        ('RUN', '%(wasm-interp)s %(temp_file)s.wasm --run-all-exports'),
        ('VERBOSE-ARGS', ['--print-cmd', '-v']),
    ],
    'run-interp-wasi': [
        ('RUN', '%(wat2wasm)s %(in_file)s -o %(temp_file)s.wasm'),
        ('RUN', '%(wasm-interp)s --wasi %(temp_file)s.wasm'),
        ('VERBOSE-ARGS', ['--print-cmd', '-v']),
    ],
    'run-interp-spec': [
        ('RUN', '%(wast2json)s %(in_file)s -o %(temp_file)s.json'),
        ('RUN', '%(spectest-interp)s %(temp_file)s.json'),
        ('VERBOSE-ARGS', ['--print-cmd', '-v']),
    ],
    'run-gen-wasm': [
        ('RUN', '%(gen_wasm_py)s %(in_file)s -o %(temp_file)s.wasm'),
        ('RUN', '%(wasm-validate)s %(temp_file)s.wasm'),
        ('RUN', '%(wasm2wat)s %(temp_file)s.wasm'),
        ('VERBOSE-ARGS', ['--print-cmd', '-v']),
    ],
    'run-gen-wasm-bad': [
        ('RUN', '%(gen_wasm_py)s %(in_file)s -o %(temp_file)s.wasm'),
        ('RUN', '%(wasm-validate)s %(temp_file)s.wasm'),
        ('ERROR', '1'),
        ('RUN', '%(wasm2wat)s %(temp_file)s.wasm'),
        ('ERROR', '1'),
        ('VERBOSE-ARGS', ['--print-cmd', '-v']),
    ],
    'run-gen-wasm-interp': [
        ('RUN', '%(gen_wasm_py)s %(in_file)s -o %(temp_file)s.wasm'),
        ('RUN', '%(wasm-interp)s --run-all-exports %(temp_file)s.wasm'),
        ('VERBOSE-ARGS', ['--print-cmd', '-v']),
    ],
    'run-gen-wasm-strip': [
        ('RUN', '%(gen_wasm_py)s %(in_file)s -o %(temp_file)s.wasm'),
        ('RUN', '%(wasm-strip)s %(temp_file)s.wasm'),
        ('RUN', '%(wasm-objdump)s -h %(temp_file)s.wasm'),
        ('VERBOSE-ARGS', ['--print-cmd', '-v']),
    ],
    'run-gen-wasm-decompile': [
        ('RUN', '%(gen_wasm_py)s %(in_file)s -o %(temp_file)s.wasm'),
        ('RUN', '%(wasm-validate)s %(temp_file)s.wasm'),
        ('RUN', '%(wasm-decompile)s %(temp_file)s.wasm'),
        ('VERBOSE-ARGS', ['--print-cmd', '-v']),
    ],
    'run-stats': [
        ('RUN', '%(wat2wasm)s %(in_file)s -o %(temp_file)s.wasm'),
        ('RUN', '%(wasm-stats)s %(temp_file)s.wasm'),
        ('VERBOSE-ARGS', ['--print-cmd', '-v']),
    ],
    'run-gen-spec-js': [
        ('RUN', '%(wast2json)s %(in_file)s -o %(temp_file)s.json'),
        ('RUN', '%(gen_spec_js_py)s %(temp_file)s.json'),
        ('VERBOSE-ARGS', ['--print-cmd', '-v']),
    ],
    'run-spec-wasm2c': [
        ('RUN', 'test/run-spec-wasm2c.py'),
        ('ARGS', [
            '%(in_file)s',
            '--bindir=%(bindir)s',
            '--no-error-cmdline',
            '-o',
            '%(out_dir)s',
        ]),
        ('VERBOSE-ARGS', ['--print-cmd', '-v']),
    ],
    'run-wasm2c': [
        ('RUN', '%(wat2wasm)s %(in_file)s -o %(temp_file)s.wasm'),
        ('RUN', '%(wasm2c)s -n test %(temp_file)s.wasm'),
    ],
    'run-wasm-decompile': [
        ('RUN', '%(wat2wasm)s --enable-all %(in_file)s -o %(temp_file)s.wasm'),
        ('RUN', '%(wasm-decompile)s --enable-all %(temp_file)s.wasm'),
    ]
}

ROUNDTRIP_TOOLS = ('wat2wasm',)


class NoRoundtripError(Error):
    pass


def Indent(s, spaces):
    return ''.join(' ' * spaces + l for l in s.splitlines(1))


def DiffLines(expected, actual):
    def Decode(s):
        return s.decode('utf-8', 'replace')

    expected_lines = [Decode(line) for line in expected.splitlines() if line]
    actual_lines = [Decode(line) for line in actual.splitlines() if line]
    return list(
        difflib.unified_diff(expected_lines, actual_lines, fromfile='expected',
                             tofile='actual', lineterm=''))


class Cell(object):

    def __init__(self, value):
        self.value = [value]

    def Set(self, value):
        self.value[0] = value

    def Get(self):
        return self.value[0]


def SplitArgs(value):
    if isinstance(value, list):
        return value
    return shlex.split(value)


def FixPythonExecutable(args):
    """Given an argument list beginning with a `*.py` file, return one with that
         uses sys.executable as arg[0].
    """
    exe, rest = args[0], args[1:]
    if os.path.splitext(exe)[1] == '.py':
        return [sys.executable, os.path.join(REPO_ROOT_DIR, exe)] + rest
    return args


class CommandTemplate(object):

    def __init__(self, exe):
        self.args = SplitArgs(exe)
        self.verbose_args = []
        self.stdin = None
        self.expected_returncode = 0

    def AppendArgs(self, args):
        self.args += SplitArgs(args)

    def SetExpectedReturncode(self, returncode):
        self.expected_returncode = returncode

    def AppendVerboseArgs(self, args_list):
        for level, level_args in enumerate(args_list):
            while level >= len(self.verbose_args):
                self.verbose_args.append([])
            self.verbose_args[level] += SplitArgs(level_args)

    def SetStdin(self, filename):
        self.stdin = filename

    def _Format(self, cmd, variables):
        return [arg % variables for arg in cmd]

    def GetCommand(self, variables, extra_args=None, verbose_level=0):
        args = self.args[:]
        vl = 0
        while vl < verbose_level and vl < len(self.verbose_args):
            args += self.verbose_args[vl]
            vl += 1
        if extra_args:
            args += extra_args
        args = self._Format(args, variables)
        stdin = self.stdin
        if stdin:
            stdin = stdin % variables
        return Command(self, FixPythonExecutable(args), stdin)


class Command(object):

    def __init__(self, template, args, stdin):
        self.template = template
        self.args = args
        self.stdin = stdin

    def GetExpectedReturncode(self):
        return self.template.expected_returncode

    def Run(self, cwd, timeout, console_out=False, env=None):
        process = None
        is_timeout = Cell(False)

        def KillProcess(timeout=True):
            if process:
                try:
                    if IS_WINDOWS:
                        # http://stackoverflow.com/a/10830753: deleting child processes in
                        # Windows
                        subprocess.call(['taskkill', '/F', '/T', '/PID', str(process.pid)])
                    else:
                        os.killpg(os.getpgid(process.pid), 15)
                except OSError:
                    pass
            is_timeout.Set(timeout)

        try:
            start_time = time.time()
            kwargs = {}
            if not IS_WINDOWS:
                kwargs['preexec_fn'] = os.setsid
            stdin_data = None
            if self.stdin:
                stdin_data = open(self.stdin, 'rb').read()

            # http://stackoverflow.com/a/10012262: subprocess with a timeout
            # http://stackoverflow.com/a/22582602: kill subprocess and children
            process = subprocess.Popen(self.args, cwd=cwd, env=env,
                                       stdout=None if console_out else subprocess.PIPE,
                                       stderr=None if console_out else subprocess.PIPE,
                                       stdin=None if not self.stdin else subprocess.PIPE,
                                       **kwargs)
            timer = threading.Timer(timeout, KillProcess)
            try:
                timer.start()
                stdout, stderr = process.communicate(input=stdin_data)
            finally:
                returncode = process.returncode
                process = None
                timer.cancel()
            if is_timeout.Get():
                raise Error('TIMEOUT')
            duration = time.time() - start_time
        except OSError as e:
            raise Error(str(e))
        finally:
            KillProcess(False)

        return RunResult(self, stdout, stderr, returncode, duration)

    def __str__(self):
        return ' '.join(self.args)


class RunResult(object):

    def __init__(self, cmd=None, stdout='', stderr='', returncode=0, duration=0):
        self.cmd = cmd
        self.stdout = stdout
        self.stderr = stderr
        self.returncode = returncode
        self.duration = duration

    def GetExpectedReturncode(self):
        return self.cmd.GetExpectedReturncode()

    def Failed(self):
        return self.returncode != self.GetExpectedReturncode()

    def __repr__(self):
        return 'RunResult(%s, %s, %s, %s, %s)' % (
            self.cmd, self.stdout, self.stderr, self.returncode, self.duration)


class TestResult(object):

    def __init__(self):
        self.results = []
        self.stdout = b''
        self.stderr = b''
        self.duration = 0

    def GetLastCommand(self):
        return self.results[-1].cmd

    def GetLastFailure(self):
        return [r for r in self.results if r.Failed()][-1]

    def Failed(self):
        return any(r.Failed() for r in self.results)

    def Append(self, result):
        self.results.append(result)

        if result.stdout is not None:
            self.stdout += result.stdout

        if result.stderr is not None:
            self.stderr += result.stderr

        self.duration += result.duration


class TestInfo(object):

    def __init__(self):
        self.filename = ''
        self.header = []
        self.input_filename = ''
        self.input_ = ''
        self.expected_stdout = ''
        self.expected_stderr = ''
        self.tool = None
        self.cmds = []
        self.env = {}
        self.slow = False
        self.skip = False
        self.is_roundtrip = False
        self.is_wasm2c = False

    def CreateRoundtripInfo(self, fold_exprs):
        if self.tool not in ROUNDTRIP_TOOLS:
            raise NoRoundtripError()

        if len(self.cmds) != 1:
            raise NoRoundtripError()

        result = TestInfo()
        result.SetTool('run-roundtrip')
        result.filename = self.filename
        result.header = self.header
        result.input_filename = self.input_filename
        result.input_ = self.input_
        result.expected_stdout = ''
        result.expected_stderr = ''

        # TODO(binji): It's kind of cheesy to keep the enable flag based on prefix.
        # Maybe it would be nicer to add a new directive ENABLE instead.
        old_cmd = self.cmds[0]
        new_cmd = result.cmds[0]
        new_cmd.AppendArgs([f for f in old_cmd.args if f.startswith('--enable')])
        if fold_exprs:
            new_cmd.AppendArgs('--fold-exprs')

        result.env = self.env
        result.slow = self.slow
        result.skip = self.skip
        result.is_roundtrip = True
        result.fold_exprs = fold_exprs
        return result

    def GetName(self):
        name = self.filename
        if self.is_roundtrip:
            if self.fold_exprs:
                name += ' (roundtrip fold-exprs)'
            else:
                name += ' (roundtrip)'
        return name

    def GetGeneratedInputFilename(self):
        # All tests should be generated in their own directories, even if they
        # share the same input filename. We want the input filename to be correct,
        # though, so we use the directory of the test (.txt) file, but the basename
        # of the input file (likely a .wast).

        path = OUT_DIR
        if self.input_filename:
            basename = os.path.basename(self.input_filename)
        else:
            basename = os.path.basename(self.filename)

        path = os.path.join(path, os.path.dirname(self.filename), basename)

        if self.is_roundtrip:
            dirname = os.path.dirname(path)
            basename = os.path.basename(path)
            if self.fold_exprs:
                path = os.path.join(dirname, 'roundtrip_folded', basename)
            else:
                path = os.path.join(dirname, 'roundtrip', basename)

        return path

    def SetTool(self, tool):
        if tool not in TOOLS:
            raise Error('Unknown tool: %s' % tool)
        self.tool = tool
        self.is_wasm2c = self.tool == 'run-spec-wasm2c'
        for tool_key, tool_value in TOOLS[tool]:
            self.ParseDirective(tool_key, tool_value)

    def GetCommand(self, index):
        try:
            return self.cmds[index]
        except IndexError:
            raise Error('Invalid command index: %s' % index)

    def GetLastCommand(self):
        return self.GetCommand(len(self.cmds) - 1)

    def ApplyToCommandBySuffix(self, suffix, fn):
        if suffix == '':
            fn(self.GetLastCommand())
        elif re.match(r'^\d+$', suffix):
            fn(self.GetCommand(int(suffix)))
        elif suffix == '*':
            for cmd in self.cmds:
                fn(cmd)
        else:
            raise Error('Invalid directive suffix: %s' % suffix)

    def ParseDirective(self, key, value):
        if key == 'RUN':
            self.cmds.append(CommandTemplate(value))
        elif key == 'STDIN_FILE':
            self.input_filename = value
        elif key.startswith('ARGS'):
            suffix = key[len('ARGS'):]
            self.ApplyToCommandBySuffix(suffix, lambda cmd: cmd.AppendArgs(value))
        elif key.startswith('ERROR'):
            suffix = key[len('ERROR'):]
            self.ApplyToCommandBySuffix(
                suffix, lambda cmd: cmd.SetExpectedReturncode(int(value)))
        elif key == 'SLOW':
            self.slow = True
        elif key == 'SKIP':
            self.skip = True
        elif key == 'VERBOSE-ARGS':
            self.GetLastCommand().AppendVerboseArgs(value)
        elif key in ['TODO', 'NOTE', 'UPSTREAM']:
            pass
        elif key == 'TOOL':
            self.SetTool(value)
        elif key == 'STDIN':
            self.GetLastCommand().SetStdin(value)
        elif key == 'ENV':
            # Pattern: FOO=1 BAR=stuff
            self.env = dict(x.split('=') for x in value.split())
        elif key == 'PLATFORMS':
            self.skip = platform.system() not in value.split()
        elif key == 'NOT-PLATFORMS':
            self.skip = platform.system() in value.split()
        else:
            raise Error('Unknown directive: %s' % key)

    def Parse(self, filename):
        self.filename = filename

        test_path = os.path.join(REPO_ROOT_DIR, filename)
        # Read/write as binary because spec tests may have invalid UTF-8 codes.
        with open(test_path, 'rb') as f:
            state = 'header'
            empty = True
            header_lines = []
            input_lines = []
            stdout_lines = []
            stderr_lines = []
            for line in f.readlines():
                empty = False
                m = re.match(b'\\s*\\(;; (STDOUT|STDERR) ;;;$', line.strip())
                if m:
                    directive = m.group(1).decode('utf-8')
                    if directive == 'STDERR':
                        state = 'stderr'
                        continue
                    elif directive == 'STDOUT':
                        state = 'stdout'
                        continue
                else:
                    m = re.match(b'\\s*;;;(.*)$', line)
                    if m:
                        directive = m.group(1).decode('utf-8').strip()
                        if state == 'header':
                            key, value = directive.split(':', 1)
                            key = key.strip()
                            value = value.strip()
                            self.ParseDirective(key, value)
                        elif state in ('stdout', 'stderr'):
                            if not re.match(r'%s ;;\)$' % state.upper(), directive):
                                raise Error('Bad directive in %s block: %s' % (state,
                                                                               directive))
                            state = 'none'
                        else:
                            raise Error('Unexpected directive: %s' % directive)
                    elif state == 'header':
                        state = 'input'

                if state == 'header':
                    header_lines.append(line.decode('utf-8'))
                if state == 'input':
                    if self.input_filename:
                        raise Error('Can\'t have STDIN_FILE and input')
                    input_lines.append(line)
                elif state == 'stderr':
                    stderr_lines.append(line)
                elif state == 'stdout':
                    stdout_lines.append(line)
        if empty:
            raise Error('empty test file')

        self.header = ''.join(header_lines)
        self.input_ = b''.join(input_lines)
        self.expected_stdout = b''.join(stdout_lines)
        self.expected_stderr = b''.join(stderr_lines)

        if not self.cmds:
            raise Error('test has no commands')

    def CreateInputFile(self):
        gen_input_path = self.GetGeneratedInputFilename()
        gen_input_dir = os.path.dirname(gen_input_path)
        try:
            os.makedirs(gen_input_dir)
        except OSError:
            if not os.path.isdir(gen_input_dir):
                raise

        # Read/write as binary because spec tests may have invalid UTF-8 codes.
        with open(gen_input_path, 'wb') as gen_input_file:
            if self.input_filename:
                input_path = os.path.join(REPO_ROOT_DIR, self.input_filename)
                with open(input_path, 'rb') as input_file:
                    gen_input_file.write(input_file.read())
            else:
                # add an empty line for each header line so the line numbers match
                gen_input_file.write(('\n' * self.header.count('\n')).encode('ascii'))
                gen_input_file.write(self.input_)
            gen_input_file.flush()
            return gen_input_file.name

    def Rebase(self, stdout, stderr):
        test_path = os.path.join(REPO_ROOT_DIR, self.filename)
        with open(test_path, 'wb') as f:
            f.write(self.header.encode('ascii'))
            f.write(self.input_)
            if stderr:
                f.write(b'(;; STDERR ;;;\n')
                f.write(stderr)
                f.write(b';;; STDERR ;;)\n')
            if stdout:
                f.write(b'(;; STDOUT ;;;\n')
                f.write(stdout)
                f.write(b';;; STDOUT ;;)\n')

    def Diff(self, stdout, stderr):
        msg = ''
        if self.expected_stderr != stderr:
            diff_lines = DiffLines(self.expected_stderr, stderr)
            if len(diff_lines) > 0:
                msg += 'STDERR MISMATCH:\n' + '\n'.join(diff_lines) + '\n'

        if self.expected_stdout != stdout:
            diff_lines = DiffLines(self.expected_stdout, stdout)
            if len(diff_lines) > 0:
                msg += 'STDOUT MISMATCH:\n' + '\n'.join(diff_lines) + '\n'

        if msg:
            raise Error(msg)


class Status(object):

    def __init__(self, isatty):
        self.isatty = isatty
        self.start_time = None
        self.last_length = 0
        self.last_finished = None
        self.skipped = 0
        self.passed = 0
        self.failed = 0
        self.total = 0
        self.failed_tests = []

    def Start(self, total):
        self.total = total
        self.start_time = time.time()

    def Passed(self, info, duration):
        self.passed += 1
        if self.isatty:
            self._Clear()
            self._PrintShortStatus(info)
        else:
            sys.stderr.write('+ %s (%.3fs)\n' % (info.GetName(), duration))

    def Failed(self, info, error_msg, result=None):
        self.failed += 1
        self.failed_tests.append((info, result))
        if self.isatty:
            self._Clear()
        sys.stderr.write('- %s\n%s\n' % (info.GetName(), Indent(error_msg, 2)))

    def Skipped(self, info):
        self.skipped += 1
        if not self.isatty:
            sys.stderr.write('. %s (skipped)\n' % info.GetName())

    def Done(self):
        if self.isatty:
            sys.stderr.write('\n')

    def _PrintShortStatus(self, info):
        assert(self.isatty)
        total_duration = time.time() - self.start_time
        name = info.GetName() if info else ''
        if self.total - self.skipped:
            percent = 100 * (self.passed + self.failed) / (self.total - self.skipped)
        else:
            percent = 100
        status = '[+%d|-%d|%%%d] (%.2fs) %s' % (self.passed, self.failed,
                                                percent, total_duration, name)
        self.last_length = len(status)
        self.last_finished = info
        sys.stderr.write(status)
        sys.stderr.flush()

    def _Clear(self):
        assert(self.isatty)
        sys.stderr.write('\r%s\r' % (' ' * self.last_length))


def FindTestFiles(ext, filter_pattern_re, exclude_dirs):
    tests = []
    for root, dirs, files in os.walk(TEST_DIR):
        for ex in exclude_dirs:
            if ex in dirs:
                # Filtering out dirs here causes os.walk not to descend into them
                dirs.remove(ex)
        for f in files:
            path = os.path.join(root, f)
            if os.path.splitext(f)[1] == ext:
                tests.append(os.path.relpath(path, REPO_ROOT_DIR))
    tests.sort()
    return [test for test in tests if re.match(filter_pattern_re, test)]


def GetAllTestInfo(test_names, status):
    infos = []
    for test_name in test_names:
        info = TestInfo()
        try:
            info.Parse(test_name)
            infos.append(info)
        except Error as e:
            status.Failed(info, str(e))
    return infos


def CanonicalizePath(path):
    """In order to get identical test results we canonicalize path
    names that we pass into test code.  We use posix path separators
    because they work sufficiently well on all platforms (including
    windows)."""
    # For some reason this doesn't work on winows:
    # return str(pathlib.PurePosixPath(path))
    return path.replace(os.path.sep, '/')


def RunTest(info, options, variables, verbose_level=0):
    timeout = options.timeout
    if info.slow:
        timeout *= SLOW_TIMEOUT_MULTIPLIER

    # Clone variables dict so it can be safely modified.
    variables = dict(variables)

    cwd = REPO_ROOT_DIR
    env = dict(os.environ)
    env.update(info.env)
    gen_input_path = info.CreateInputFile()
    rel_gen_input_path = os.path.relpath(gen_input_path, cwd)
    variables['in_file'] = CanonicalizePath(rel_gen_input_path)

    # Each test runs with a unique output directory which is removed before
    # we run the test.
    out_dir = os.path.splitext(rel_gen_input_path)[0]
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)
    variables['out_dir'] = CanonicalizePath(out_dir)

    # Temporary files typically are placed in `out_dir` and use the same test's
    # basename. This name does include an extension.
    input_basename = os.path.basename(rel_gen_input_path)
    temp_file = os.path.join(out_dir, os.path.splitext(input_basename)[0])
    variables['temp_file'] = CanonicalizePath(temp_file)

    test_result = TestResult()

    for cmd_template in info.cmds:
        cmd = cmd_template.GetCommand(variables, options.arg, verbose_level)
        if options.print_cmd:
            print(cmd)

        try:
            result = cmd.Run(cwd, timeout, verbose_level > 0, env)
        except (Error, KeyboardInterrupt) as e:
            return e

        test_result.Append(result)
        if result.Failed():
            break

    return test_result


def HandleTestResult(status, info, result, rebase=False):
    try:
        if isinstance(result, (Error, KeyboardInterrupt)):
            raise result

        if info.is_roundtrip:
            if result.Failed():
                if result.GetLastFailure().returncode == 2:
                    # run-roundtrip.py returns 2 if the file couldn't be parsed.
                    # it's likely a "bad-*" file.
                    status.Skipped(info)
                else:
                    raise Error(result.stderr)
            else:
                status.Passed(info, result.duration)
        else:
            if result.Failed():
                if result.GetLastFailure().returncode == 3:
                    # run-spec-wasm2c.py returns 3 to signal that the test
                    # should be skipped.
                    status.Skipped(info)
                    return
                # This test has already failed, but diff it anyway.
                last_failure = result.GetLastFailure()
                msg = 'expected error code %d, got %d.' % (
                    last_failure.GetExpectedReturncode(),
                    last_failure.returncode)
                try:
                    info.Diff(result.stdout, result.stderr)
                except Error as e:
                    msg += '\n' + str(e)
                raise Error(msg)
            else:
                if rebase:
                    info.Rebase(result.stdout, result.stderr)
                else:
                    info.Diff(result.stdout, result.stderr)
                status.Passed(info, result.duration)
    except Error as e:
        status.Failed(info, str(e), result)


# Source : http://stackoverflow.com/questions/3041986/python-command-line-yes-no-input
def YesNoPrompt(question, default='yes'):
    """Ask a yes/no question via input() and return their answer.

    "question" is a string that is presented to the user.
    "default" is the presumed answer if the user just hits <Enter>.
        It must be "yes" (the default), "no" or None (meaning
        an answer is required of the user).

    The "answer" return value is True for "yes" or False for "no".
    """
    valid = {'yes': True, 'y': True, 'ye': True, 'no': False, 'n': False}
    if default is None:
        prompt = ' [y/n] '
    elif default == 'yes':
        prompt = ' [Y/n] '
    elif default == 'no':
        prompt = ' [y/N] '
    else:
        raise ValueError('invalid default answer: \'%s\'' % default)

    while True:
        sys.stdout.write(question + prompt)
        choice = input().lower()
        if default is not None and choice == '':
            return valid[default]
        elif choice in valid:
            return valid[choice]
        else:
            sys.stdout.write('Please respond with \'yes\' or \'no\' '
                             '(or \'y\' or \'n\').\n')


def RunMultiThreaded(infos_to_run, status, options, variables):
    pool = multiprocessing.Pool(options.jobs)
    try:
        results = [(info, pool.apply_async(RunTest, (info, options, variables)))
                   for info in infos_to_run]
        while results:
            new_results = []
            for info, result in results:
                if result.ready():
                    HandleTestResult(status, info, result.get(0), options.rebase)
                else:
                    new_results.append((info, result))
            time.sleep(0.01)
            results = new_results
        pool.close()
    finally:
        pool.terminate()
        pool.join()


def RunSingleThreaded(infos_to_run, status, options, variables):
    continued_errors = 0

    for info in infos_to_run:
        result = RunTest(info, options, variables)
        HandleTestResult(status, info, result, options.rebase)
        if status.failed > continued_errors:
            if options.fail_fast:
                break
            elif options.stop_interactive:
                rerun_verbose = YesNoPrompt(question='Rerun with verbose option?',
                                            default='no')
                if rerun_verbose:
                    RunTest(info, options, variables, verbose_level=2)
                should_continue = YesNoPrompt(question='Continue testing?',
                                              default='yes')
                if not should_continue:
                    break
            elif options.verbose:
                RunTest(info, options, variables, verbose_level=1)
            continued_errors += 1


def GetDefaultJobCount():
    cpu_count = multiprocessing.cpu_count()
    if cpu_count <= 1:
        return 1
    else:
        return cpu_count // 2


def main(args):
    parser = argparse.ArgumentParser()
    parser.add_argument('-a', '--arg',
                        help='additional args to pass to executable',
                        action='append')
    parser.add_argument('--bindir', metavar='PATH',
                        default=find_exe.GetDefaultPath(),
                        help='directory to search for all executables.')
    parser.add_argument('-v', '--verbose', help='print more diagnotic messages.',
                        action='store_true')
    parser.add_argument('-f', '--fail-fast', help='Exit on first failure. '
                        'Extra options with \'--jobs 1\'', action='store_true')
    parser.add_argument('--stop-interactive',
                        help='Enter interactive mode on errors. '
                        'Extra options with \'--jobs 1\'', action='store_true')
    parser.add_argument('-l', '--list', help='list all tests.',
                        action='store_true')
    parser.add_argument('-r', '--rebase',
                        help='rebase a test to its current output.',
                        action='store_true')
    parser.add_argument('-j', '--jobs',
                        help='number of jobs to use to run tests', type=int,
                        default=GetDefaultJobCount())
    parser.add_argument('-t', '--timeout', type=float, default=DEFAULT_TIMEOUT,
                        help='per test timeout in seconds')
    parser.add_argument('--no-roundtrip',
                        help='don\'t run roundtrip.py on all tests',
                        action='store_false', default=True, dest='roundtrip')
    parser.add_argument('-p', '--print-cmd',
                        help='print the commands that are run.',
                        action='store_true')
    parser.add_argument('patterns', metavar='pattern', nargs='*',
                        help='test patterns.')
    parser.add_argument('--exclude-dir', action='append', default=[],
                        help='directory to exclude.')
    options = parser.parse_args(args)
    exclude_dirs = options.exclude_dir

    if options.jobs != 1:
        if options.fail_fast:
            parser.error('--fail-fast only works with -j1')
        if options.stop_interactive:
            parser.error('--stop-interactive only works with -j1')

    if options.patterns:
        pattern_re = '|'.join(
            fnmatch.translate('*%s*' % p) for p in options.patterns)
    else:
        pattern_re = '.*'
        # By default, exclude wasi tests because WASI support is not include
        # by int the build by default.
        # TODO(sbc): Find some way to detect the WASI support.
        exclude_dirs += ['wasi']

    test_names = FindTestFiles('.txt', pattern_re, exclude_dirs)

    if options.list:
        for test_name in test_names:
            print(test_name)
        return 0
    if not test_names:
        print('no tests match that filter')
        return 1

    variables = {}
    variables['test_dir'] = os.path.abspath(TEST_DIR)
    variables['bindir'] = options.bindir
    variables['gen_wasm_py'] = os.path.join(TEST_DIR, 'gen-wasm.py')
    variables['gen_spec_js_py'] = os.path.join(TEST_DIR, 'gen-spec-js.py')
    for exe_basename in find_exe.EXECUTABLES:
        exe_override = os.path.join(options.bindir, exe_basename)
        variables[exe_basename] = find_exe.FindExecutable(exe_basename,
                                                          exe_override)

    status = Status(sys.stderr.isatty() and not options.verbose)
    infos = GetAllTestInfo(test_names, status)
    infos_to_run = []
    for info in infos:
        if info.skip:
            status.Skipped(info)
            continue
        infos_to_run.append(info)

        if options.roundtrip:
            for fold_exprs in False, True:
                try:
                    infos_to_run.append(info.CreateRoundtripInfo(fold_exprs=fold_exprs))
                except NoRoundtripError:
                    pass

    if not os.path.exists(OUT_DIR):
        os.makedirs(OUT_DIR)

    status.Start(len(infos_to_run))

    try:
        if options.jobs > 1:
            RunMultiThreaded(infos_to_run, status, options, variables)
        else:
            RunSingleThreaded(infos_to_run, status, options, variables)
    except KeyboardInterrupt:
        print('\nInterrupted testing\n')

    status.Done()

    ret = 0
    if status.failed:
        sys.stderr.write('**** FAILED %s\n' % ('*' * (80 - 14)))
        for info, result in status.failed_tests:
            if isinstance(result, TestResult):
                msg = result.GetLastCommand()
            elif isinstance(result, Error):
                msg = result
            else:
                msg = ''
            sys.stderr.write('- %s\n    %s\n' % (info.GetName(), msg))
        ret = 1

    return ret


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except Error as e:
        sys.stderr.write(str(e) + '\n')
        sys.exit(1)
