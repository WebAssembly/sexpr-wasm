#!/usr/bin/env python
#
# Copyright 2017 WebAssembly Community Group participants
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
import io
import json
import os
import platform
import re
import struct
import sys
import shlex
import subprocess

import find_exe
import utils
from utils import Error

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
WASM2C_DIR = os.path.join(find_exe.REPO_ROOT_DIR, 'wasm2c')
SIMDE_DIR = os.path.join(find_exe.REPO_ROOT_DIR, 'third_party/simde')
IS_WINDOWS = sys.platform == 'win32'
IS_MACOS = platform.mac_ver()[0] != ''
MAX_COMMANDS_PER_FUNCTION = 1024  # GCC has trouble with extremely long function bodies
SKIPPED = 3


def ReinterpretF32(f32_bits):
    return struct.unpack('<f', struct.pack('<I', f32_bits))[0]


def F32ToC(f32_bits):
    F32_SIGN_BIT = 0x80000000
    F32_INF = 0x7f800000
    F32_SIG_MASK = 0x7fffff

    if (f32_bits & F32_INF) == F32_INF:
        sign = '-' if (f32_bits & F32_SIGN_BIT) == F32_SIGN_BIT else ''
        # NaN or infinity
        if f32_bits & F32_SIG_MASK:
            # NaN
            return '%smake_nan_f32(0x%06x)' % (sign, f32_bits & F32_SIG_MASK)
        else:
            return '%sINFINITY' % sign
    elif f32_bits == F32_SIGN_BIT:
        return '-0.f'
    else:
        s = '%.9g' % ReinterpretF32(f32_bits)
        if '.' not in s:
            s += '.'
        return s + 'f'


def ReinterpretF64(f64_bits):
    return struct.unpack('<d', struct.pack('<Q', f64_bits))[0]


def F64ToC(f64_bits):
    F64_SIGN_BIT = 0x8000000000000000
    F64_INF = 0x7ff0000000000000
    F64_SIG_MASK = 0xfffffffffffff

    if (f64_bits & F64_INF) == F64_INF:
        sign = '-' if (f64_bits & F64_SIGN_BIT) == F64_SIGN_BIT else ''
        # NaN or infinity
        if f64_bits & F64_SIG_MASK:
            # NaN
            return '%smake_nan_f64(0x%06x)' % (sign, f64_bits & F64_SIG_MASK)
        else:
            return '%sINFINITY' % sign
    elif f64_bits == F64_SIGN_BIT:
        return '-0.0'
    else:
        return '%#.17g' % ReinterpretF64(f64_bits)


def MangleType(t):
    return {'i32': 'i', 'i64': 'j', 'f32': 'f', 'f64': 'd', 'v128': 'o',
            'externref': 'e', 'funcref': 'r'}[t]


def MangleTypes(types):
    if not types:
        return 'v'
    return ''.join(MangleType(t) for t in types)


def MangleName(s):
    def Mangle(match):
        s = match.group(0)
        return b'0x%02X' % s[0]

    # escape underscores at beginning and end
    s = re.sub(b'((^_)|(_$))', Mangle, s.encode('utf-8'))

    # NOTE(keithw): forced escapes for '0x[hexdigit]' not implemented here
    pattern = b'([^_a-zA-Z0-9])'
    return re.sub(pattern, Mangle, s).decode('utf-8')


def MangleModuleName(s):
    # double underscores
    return MangleName(re.sub('(_)', '__', s))


def IsModuleCommand(command):
    return (command['type'] == 'module' or
            command['type'] == 'assert_uninstantiable')


class CWriter(object):

    def __init__(self, spec_json, prefix, out_file, out_dir):
        self.source_filename = os.path.basename(spec_json['source_filename'])
        self.commands = spec_json['commands']
        self.out_file = out_file
        self.out_dir = out_dir
        self.prefix = prefix
        self.module_idx = 0
        self.module_name_to_idx = {}
        self.module_prefix_map = {}
        self.unmangled_names = {}
        self.idx_to_module_name = {}
        self._MaybeWriteDummyModule()
        self._CacheModulePrefixes()

    def Write(self):
        self._WriteIncludes()
        self.out_file.write(self.prefix)
        self._WriteModuleInstances()
        test_function_num = 0
        self.out_file.write('\nvoid run_spec_tests_0(void) {\n\n')
        for i, command in enumerate(self.commands):
            self._WriteCommand(command)
            if i % MAX_COMMANDS_PER_FUNCTION == MAX_COMMANDS_PER_FUNCTION - 1:
                test_function_num += 1
                self.out_file.write('\n}\n\nvoid run_spec_tests_%d(void) {\n\n' % test_function_num)

        self.out_file.write('\n}\n\nvoid run_spec_tests(void) {\n\n')
        for i in range(test_function_num + 1):
            self.out_file.write('run_spec_tests_%d();\n' % i)
        self._WriteModuleCleanUps()
        self.out_file.write('\n}\n')

    def GetModuleFilenames(self):
        return [c['filename'] for c in self.commands if IsModuleCommand(c)]

    def GetModulePrefix(self, idx_or_name=None):
        if idx_or_name is None:
            idx_or_name = self.module_idx - 1
        return self.module_prefix_map[idx_or_name]

    def GetModulePrefixUnmangled(self, idx):
        return self.unmangled_names[idx]

    def GetModuleInstanceName(self, idx_or_name=None):
        return self.GetModulePrefix(idx_or_name) + '_instance'

    def _CacheModulePrefixes(self):
        idx = 0
        for command in self.commands:
            if IsModuleCommand(command):
                name = os.path.basename(command['filename'])
                name = re.sub(r'[^a-zA-Z0-9_]', '_', name)
                name = os.path.splitext(name)[0]
                self.unmangled_names[idx] = name
                name = MangleModuleName(name)

                self.module_prefix_map[idx] = name

                if 'name' in command:
                    self.module_name_to_idx[command['name']] = idx
                    self.idx_to_module_name[idx] = command['name']
                    self.module_prefix_map[command['name']] = name

                idx += 1
            elif command['type'] == 'register':
                name = MangleModuleName(command['as'])
                if 'name' in command:
                    self.module_prefix_map[command['name']] = name
                    name_idx = self.module_name_to_idx[command['name']]
                else:
                    name_idx = idx - 1

                if name_idx in self.idx_to_module_name:
                    self.module_prefix_map[self.idx_to_module_name[name_idx]] = name
                self.module_prefix_map[name_idx] = name
                self.unmangled_names[name_idx] = command['as']

    def _WriteModuleInitCall(self, command, uninstantiable):
        header_filename = utils.ChangeExt(command['filename'], '.h')
        with open(os.path.join(self.out_dir, header_filename), encoding='utf-8') as f:
            imported_modules = set()
            for line in f:
                if 'import: ' in line:
                    line_split = line.split()
                    import_module_name = MangleModuleName(line_split[2][1:-1])
                    imported_modules.add(import_module_name)

        if uninstantiable:
            self.out_file.write('ASSERT_TRAP(')

        self.out_file.write('wasm2c_%s_instantiate(&%s_instance' % (self.GetModulePrefix(), self.GetModulePrefix()))
        for imported_module in sorted(imported_modules):
            self.out_file.write(', &%s_instance' % imported_module)
        self.out_file.write(')')

        if uninstantiable:
            self.out_file.write(')')

        self.out_file.write(';\n')

    def _MaybeWriteDummyModule(self):
        if len(self.GetModuleFilenames()) == 0:
            # This test doesn't have any valid modules, so just use a dummy instead.
            filename = utils.ChangeExt(self.source_filename, '-dummy.wasm')
            with open(os.path.join(self.out_dir, filename), 'wb') as wasm_file:
                wasm_file.write(b'\x00\x61\x73\x6d\x01\x00\x00\x00')

            dummy_command = {'type': 'module', 'line': 0, 'filename': filename}
            self.commands.insert(0, dummy_command)

    def _WriteFileAndLine(self, command):
        self.out_file.write('#line {line:d} "{name:s}"\n'.format(name=self.source_filename, line=command['line']))

    def _WriteIncludes(self):
        idx = 0
        for filename in self.GetModuleFilenames():
            header = os.path.splitext(filename)[0] + '.h'
            self.out_file.write("#include \"%s\"\n" % header)
            idx += 1

    def _WriteCommand(self, command):
        command_funcs = {
            'module': self._WriteModuleCommand,
            'assert_uninstantiable': self._WriteAssertUninstantiableCommand,
            'action': self._WriteActionCommand,
            'assert_return': self._WriteAssertReturnCommand,
            'assert_trap': self._WriteAssertActionCommand,
            'assert_exception': self._WriteAssertActionCommand,
            'assert_exhaustion': self._WriteAssertActionCommand,
        }

        func = command_funcs.get(command['type'])
        if func is not None:
            self._WriteFileAndLine(command)
            func(command)
            self.out_file.write('\n')

    def _WriteModuleCommand(self, command):
        self.module_idx += 1
        self._WriteModuleInitCall(command, False)

    def _WriteModuleInstances(self):
        idx = 0
        for command in self.commands:
            if IsModuleCommand(command):
                self.out_file.write('w2c_%s %s;\n' % (self.GetModulePrefix(idx), self.GetModuleInstanceName(idx)))
                idx += 1

    def _WriteModuleCleanUps(self):
        for idx in range(self.module_idx):
            self.out_file.write("wasm2c_%s_free(&%s);\n" % (self.GetModulePrefix(idx), self.GetModuleInstanceName(idx)))

    def _WriteAssertUninstantiableCommand(self, command):
        self.module_idx += 1
        self._WriteModuleInitCall(command, True)

    def _WriteActionCommand(self, command):
        self.out_file.write('%s;\n' % self._Action(command))

    def _WriteAssertReturnCommand(self, command):
        expected = command['expected']
        if len(expected) == 1:
            type_ = expected[0]['type']
            value = expected[0]['value']
            if type_ == 'v128':
                lane_type = expected[0]['lane_type']
                lane_count = len(expected[0]['value'])
                # type, fmt_expected, fmt_got, f, compare, expected, found
                self.out_file.write('ASSERT_RETURN_MULTI_T(%s, %s, %s, %s, %s, (%s), (%s));\n' %
                                    ("v128",
                                     " ".join("MULTI_" + ("str" if val in ('nan:canonical', 'nan:arithmetic') else lane_type) for val in value),
                                     " ".join("MULTI_" + lane_type for _ in value),
                                     self._Action(command),
                                     self._SIMDCompareVector(expected[0]),
                                     self._SIMDConstantList(expected[0]),
                                     self._SIMDFoundList(lane_type, lane_count)))
            elif value == 'nan:canonical':
                assert_map = {
                    'f32': 'ASSERT_RETURN_CANONICAL_NAN_F32',
                    'f64': 'ASSERT_RETURN_CANONICAL_NAN_F64',
                }
                assert_macro = assert_map[(type_)]
                self.out_file.write('%s(%s);\n' % (assert_macro, self._Action(command)))
            elif value == 'nan:arithmetic':
                assert_map = {
                    'f32': 'ASSERT_RETURN_ARITHMETIC_NAN_F32',
                    'f64': 'ASSERT_RETURN_ARITHMETIC_NAN_F64',
                }
                assert_macro = assert_map[(type_)]
                self.out_file.write('%s(%s);\n' % (assert_macro, self._Action(command)))
            else:
                assert_map = {
                    'i32': 'ASSERT_RETURN_I32',
                    'f32': 'ASSERT_RETURN_F32',
                    'i64': 'ASSERT_RETURN_I64',
                    'f64': 'ASSERT_RETURN_F64',
                    'externref': 'ASSERT_RETURN_EXTERNREF',
                    'funcref': 'ASSERT_RETURN_FUNCREF',
                }

                assert_macro = assert_map[type_]
                self.out_file.write('%s(%s, %s);\n' %
                                    (assert_macro,
                                     self._Action(command),
                                     self._ConstantList(expected)))
        elif len(expected) == 0:
            self._WriteAssertActionCommand(command)
        else:
            result_types = [result['type'] for result in expected]
            # type, fmt_expected, fmt_got, f, compare, expected, found
            self.out_file.write('ASSERT_RETURN_MULTI_T(%s, %s, %s, %s, %s, (%s), (%s));\n' %
                                ("struct wasm_multi_" + MangleTypes(result_types),
                                 " ".join("MULTI_" + ty for ty in result_types),
                                 " ".join("MULTI_" + ty for ty in result_types),
                                 self._Action(command),
                                 self._CompareList(expected),
                                 self._ConstantList(expected),
                                 self._FoundList(result_types)))

    def _WriteAssertActionCommand(self, command):
        assert_map = {
            'assert_exhaustion': 'ASSERT_EXHAUSTION',
            'assert_return': 'ASSERT_RETURN',
            'assert_trap': 'ASSERT_TRAP',
            'assert_exception': 'ASSERT_EXCEPTION',
        }

        assert_macro = assert_map[command['type']]
        self.out_file.write('%s(%s);\n' % (assert_macro, self._Action(command)))

    def _Constant(self, const):
        type_ = const['type']
        value = const['value']
        if type_ == 'i8':
            return '%su' % int(value)
        if type_ == 'i16':
            return '%su' % int(value)
        if type_ == 'i32':
            return '%su' % int(value)
        elif type_ == 'i64':
            return '%sull' % int(value)
        elif type_ == 'f32':
            if value in ('nan:canonical', 'nan:arithmetic'):
                return '"(f32 %s)"' % value
            return F32ToC(int(value))
        elif type_ == 'f64':
            if value in ('nan:canonical', 'nan:arithmetic'):
                return '"(f64 %s)"' % value
            return F64ToC(int(value))
        elif type_ == 'v128':
            return 'v128_' + const['lane_type'] + 'x' + str(len(const['value'])) + '_make(' + ','.join([self._Constant({'type': const['lane_type'], 'value': x}) for x in value]) + ')'
        elif type_ == 'externref':
            if value == 'null':
                return 'wasm_rt_externref_null_value'
            else:
                return 'spectest_make_externref(%s)' % value
        elif type_ == 'funcref':
            if value == 'null':
                return 'wasm_rt_funcref_null_value'
            else:
                assert False  # can't make an arbitrary funcref from an integer value
        else:
            assert False

    def _ConstantList(self, consts):
        return ', '.join(self._Constant(const) for const in consts)

    def _Found(self, num, type_):
        return "actual.%s%s" % (MangleType(type_), num)

    def _FoundList(self, types):
        return ', '.join(self._Found(num, type_) for num, type_ in enumerate(types))

    def _Compare(self, num, const):
        return "is_equal_%s(%s, %s)" % (const['type'],
                                        self._Constant(const),
                                        self._Found(num, const['type']))

    def _CompareList(self, consts):
        return ' && '.join(self._Compare(num, const) for num, const in enumerate(consts))

    def _SIMDConstantList(self, const):
        return ', '.join(self._Constant({'type': const['lane_type'], 'value': val}) for val in const['value'])

    def _SIMDFound(self, num, lane_type, lane_count):
        return 'v128_%sx%d_extract_lane(actual, %d)' % (lane_type, lane_count, num)

    def _SIMDFoundList(self, lane_type, lane_count):
        return ', '.join(self._SIMDFound(num, lane_type, lane_count) for num in range(lane_count))

    def _SIMDCompare(self, num, val, lane_type, lane_count):
        if val == 'nan:canonical':
            return 'is_canonical_nan_%s(%s_bits(%s))' % (lane_type, lane_type, self._SIMDFound(num, lane_type, lane_count))
        elif val == 'nan:arithmetic':
            return 'is_arithmetic_nan_%s(%s_bits(%s))' % (lane_type, lane_type, self._SIMDFound(num, lane_type, lane_count))
        else:
            return "is_equal_%s(%s, %s)" % (lane_type,
                                            self._Constant({'type': lane_type, 'value': val}),
                                            self._SIMDFound(num, lane_type, lane_count))

    def _SIMDCompareVector(self, const):
        return ' && '.join(self._SIMDCompare(num, val, const['lane_type'], len(const['value'])) for num, val in enumerate(const['value']))

    def _Action(self, command):
        action = command['action']
        type_ = action['type']
        mangled_module_name = self.GetModulePrefix(action.get('module'))
        field = "w2c_" + mangled_module_name + '_' + MangleName(action['field'])
        if type_ == 'invoke':
            args = self._ConstantList(action.get('args', []))
            if len(args) == 0:
                args = f'&{mangled_module_name}_instance'
            else:
                args = f'&{mangled_module_name}_instance, {args}'
            return '%s(%s)' % (field, args)
        elif type_ == 'get':
            return '*%s(%s)' % (field, '&' + mangled_module_name + '_instance')
        else:
            raise Error('Unexpected action type: %s' % type_)


def Compile(cc, c_filename, out_dir, use_c11, *cflags):
    if IS_WINDOWS:
        ext = '.obj'
    else:
        ext = '.o'
    o_filename = utils.ChangeDir(utils.ChangeExt(c_filename, ext), out_dir)
    args = list(cflags)

    if IS_WINDOWS:
        cstd_flag = ['/std:c11', '/experimental:c11atomics'] if use_c11 else []
        args += cstd_flag + ['/nologo', '/MDd', '/c', c_filename, '/Fo' + o_filename]
    else:
        # See "Compiling the wasm2c output" section of wasm2c/README.md
        # When compiling with -O2, GCC and clang require '-fno-optimize-sibling-calls'
        # and '-frounding-math' to maintain conformance with the spec tests
        # (GCC also requires '-fsignaling-nans')
        if use_c11:
            args.append('-std=c11')
        else:
            args.append('-std=c99')
        args += ['-c', c_filename, '-o', o_filename, '-O2',
                 '-Wall', '-Werror', '-Wno-unused',
                 '-Wno-array-bounds',
                 '-Wno-ignored-optimization-argument',
                 '-Wno-tautological-constant-out-of-range-compare',
                 '-Wno-infinite-recursion',
                 # simde calls for pragma clang loop vectorize(enable),
                 # which can generate warnings if the optimization pass fails.
                 '-Wno-pass-failed',
                 '-fno-optimize-sibling-calls',
                 '-frounding-math', '-fsignaling-nans',
                 '-D_DEFAULT_SOURCE']
    # Use RunWithArgsForStdout and discard stdout because cl.exe
    # unconditionally prints the name of input files on stdout
    # and we don't want that to be part of our stdout.
    cc.RunWithArgsForStdout(*args)
    return o_filename


def Link(cc, o_filenames, main_exe, *extra_args):
    args = o_filenames
    if IS_WINDOWS:
        # Windows default to 1Mb of stack but `spec/skip-stack-guard-page.wast`
        # uses more than this.  Set to 8Mb for parity with linux.
        args += ['/nologo', '/MDd', '/link', '/stack:8388608', '/out:' + main_exe]
    else:
        args += ['-o', main_exe]
    args += list(extra_args)
    # Use RunWithArgsForStdout and discard stdout because cl.exe
    # unconditionally prints the name of input files on stdout
    # and we don't want that to be part of our stdout.
    cc.RunWithArgsForStdout(*args)


def main(args):
    default_compiler = 'cc'
    if IS_WINDOWS:
        default_compiler = 'cl.exe'
    default_compiler = os.getenv('WASM2C_CC', os.getenv('CC', default_compiler))
    parser = argparse.ArgumentParser()
    parser.add_argument('-o', '--out-dir', metavar='PATH',
                        help='output directory for files.')
    parser.add_argument('-P', '--prefix', metavar='PATH', help='prefix file.',
                        default=os.path.join(SCRIPT_DIR, 'spec-wasm2c-prefix.c'))
    parser.add_argument('--bindir', metavar='PATH',
                        default=find_exe.GetDefaultPath(),
                        help='directory to search for all executables.')
    parser.add_argument('--wasmrt-dir', metavar='PATH',
                        help='directory with wasm-rt files', default=WASM2C_DIR)
    parser.add_argument('--simde-dir', metavar='PATH',
                        help='directory with SIMD Everywhere files', default=SIMDE_DIR)
    parser.add_argument('--cc', metavar='PATH',
                        help='the path to the C compiler',
                        default=default_compiler)
    parser.add_argument('--cflags', metavar='FLAGS',
                        help='additional flags for C compiler.',
                        action='append', default=[])
    parser.add_argument('--compile', help='compile the C code (default)',
                        dest='compile', action='store_true')
    parser.add_argument('--no-compile', help='don\'t compile the C code',
                        dest='compile', action='store_false')
    parser.set_defaults(compile=True)
    parser.add_argument('--no-run', help='don\'t run the compiled executable',
                        dest='run', action='store_false')
    parser.add_argument('-v', '--verbose', help='print more diagnotic messages.',
                        action='store_true')
    parser.add_argument('--no-error-cmdline',
                        help='don\'t display the subprocess\'s commandline when '
                        'an error occurs', dest='error_cmdline',
                        action='store_false')
    parser.add_argument('-p', '--print-cmd',
                        help='print the commands that are run.',
                        action='store_true')
    parser.add_argument('file', help='wast file.')
    parser.add_argument('--enable-exceptions', action='store_true')
    parser.add_argument('--enable-multi-memory', action='store_true')
    parser.add_argument('--enable-memory64', action='store_true')
    parser.add_argument('--enable-extended-const', action='store_true')
    parser.add_argument('--enable-threads', action='store_true')
    parser.add_argument('--enable-tail-call', action='store_true')
    parser.add_argument('--enable-custom-page-sizes', action='store_true')
    parser.add_argument('--disable-bulk-memory', action='store_true')
    parser.add_argument('--disable-reference-types', action='store_true')
    parser.add_argument('--debug-names', action='store_true')
    parser.add_argument('--num-outputs', metavar='COUNT',
                        help='number of output c files for wasm2c', dest='num_outputs',
                        default=1, type=int, action='store')
    options = parser.parse_args(args)

    with utils.TempDirectory(options.out_dir, 'run-spec-wasm2c-') as out_dir:
        # Parse JSON file and generate main .c file with calls to test functions.
        wast2json = utils.Executable(
            find_exe.GetWast2JsonExecutable(options.bindir),
            error_cmdline=options.error_cmdline)
        wast2json.verbose = options.print_cmd
        wast2json.AppendOptionalArgs({
            '-v': options.verbose,
            '--enable-exceptions': options.enable_exceptions,
            '--enable-memory64': options.enable_memory64,
            '--enable-extended-const': options.enable_extended_const,
            '--enable-threads': options.enable_threads,
            '--enable-tail-call': options.enable_tail_call,
            '--enable-custom-page-sizes': options.enable_custom_page_sizes,
            '--enable-multi-memory': options.enable_multi_memory,
            '--disable-bulk-memory': options.disable_bulk_memory,
            '--disable-reference-types': options.disable_reference_types,
            '--debug-names': options.debug_names})

        json_file_path = utils.ChangeDir(
            utils.ChangeExt(options.file, '.json'), out_dir)
        wast2json.RunWithArgs(options.file, '-o', json_file_path)

        wasm2c = utils.Executable(
            find_exe.GetWasm2CExecutable(options.bindir),
            error_cmdline=options.error_cmdline)
        wasm2c.verbose = options.print_cmd
        wasm2c.AppendOptionalArgs({
            '--enable-exceptions': options.enable_exceptions,
            '--enable-memory64': options.enable_memory64,
            '--enable-extended-const': options.enable_extended_const,
            '--enable-threads': options.enable_threads,
            '--enable-tail-call': options.enable_tail_call,
            '--enable-custom-page-sizes': options.enable_custom_page_sizes,
            '--enable-multi-memory': options.enable_multi_memory})

        options.cflags += shlex.split(os.environ.get('WASM2C_CFLAGS', ''))
        cc = utils.Executable(options.cc, *options.cflags, forward_stderr=True,
                              forward_stdout=False)
        cc.verbose = options.print_cmd

        with open(json_file_path, encoding='utf-8') as json_file:
            spec_json = json.load(json_file)

        prefix = ''
        if options.prefix:
            with open(options.prefix) as prefix_file:
                prefix = prefix_file.read() + '\n'

        output = io.StringIO()
        cwriter = CWriter(spec_json, prefix, output, out_dir)

        o_filenames = []
        cflags = ['-I%s' % options.wasmrt_dir, '-I%s' % options.simde_dir]
        if options.enable_memory64:
            if IS_WINDOWS:
                sys.stderr.write('skipping: wasm2c+memory64 is not yet supported under msvc\n')
                return SKIPPED

        use_c11 = options.enable_threads

        for i, wasm_filename in enumerate(cwriter.GetModuleFilenames()):
            wasm_filename = os.path.join(out_dir, wasm_filename)
            c_filename_input = utils.ChangeExt(wasm_filename, '.c')
            c_filenames = []
            if options.num_outputs > 1:
                base = os.path.splitext(c_filename_input)[0]
                for j in range(options.num_outputs):
                    c_filenames.append(base + '_' + str(j) + '.c')
            else:
                c_filenames.append(utils.ChangeExt(wasm_filename, '.c'))
            args = ['-n', cwriter.GetModulePrefixUnmangled(i), '--num-outputs', str(options.num_outputs)]
            wasm2c.RunWithArgs(wasm_filename, '-o', c_filename_input, *args)
            if options.compile:
                for j, c_filename in enumerate(c_filenames):
                    o_filenames.append(Compile(cc, c_filename, out_dir, use_c11, *cflags))

        cwriter.Write()
        main_filename = utils.ChangeExt(json_file_path, '-main.c')
        with open(main_filename, 'w') as out_main_file:
            out_main_file.write(output.getvalue())

        if options.compile:
            # Compile runtime code
            source_files = [
                main_filename,
                os.path.join(options.wasmrt_dir, 'wasm-rt-impl.c'),
                os.path.join(options.wasmrt_dir, 'wasm-rt-exceptions-impl.c'),
                os.path.join(options.wasmrt_dir, 'wasm-rt-mem-impl.c'),
            ]

            for f in source_files:
                o_filenames.append(Compile(cc, f, out_dir, use_c11, *cflags))

            if IS_WINDOWS:
                exe_ext = '.exe'
                libs = []
            else:
                exe_ext = ''
                libs = ['-lm']
            main_exe = utils.ChangeExt(json_file_path, exe_ext)
            Link(cc, o_filenames, main_exe, *libs)

            # Run the resulting binary
            if options.run:
                return subprocess.run([main_exe]).returncode
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
