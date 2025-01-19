[![Github CI Status](https://github.com/WebAssembly/wabt/workflows/CI/badge.svg)](https://github.com/WebAssembly/wabt)

# WABT: The WebAssembly Binary Toolkit

WABT (we pronounce it "wabbit") is a suite of tools for WebAssembly, including:

 - [**wat2wasm**](https://webassembly.github.io/wabt/doc/wat2wasm.1.html): translate from [WebAssembly text format](https://webassembly.github.io/spec/core/text/index.html) to the [WebAssembly binary format](https://webassembly.github.io/spec/core/binary/index.html)
 - [**wasm2wat**](https://webassembly.github.io/wabt/doc/wasm2wat.1.html): the inverse of wat2wasm, translate from the binary format back to the text format (also known as a .wat)
 - [**wasm-objdump**](https://webassembly.github.io/wabt/doc/wasm-objdump.1.html): print information about a wasm binary. Similiar to objdump.
 - [**wasm-interp**](https://webassembly.github.io/wabt/doc/wasm-interp.1.html): decode and run a WebAssembly binary file using a stack-based interpreter
 - [**wasm-decompile**](https://webassembly.github.io/wabt/doc/wasm-decompile.1.html): decompile a wasm binary into readable C-like syntax.
 - [**wat-desugar**](https://webassembly.github.io/wabt/doc/wat-desugar.1.html): parse .wat text form as supported by the spec interpreter (s-expressions, flat syntax, or mixed) and print "canonical" flat format
 - [**wasm2c**](https://webassembly.github.io/wabt/doc/wasm2c.1.html): convert a WebAssembly binary file to a C source and header
 - [**wasm-strip**](https://webassembly.github.io/wabt/doc/wasm-strip.1.html): remove sections of a WebAssembly binary file
 - [**wasm-validate**](https://webassembly.github.io/wabt/doc/wasm-validate.1.html): validate a file in the WebAssembly binary format
 - [**wast2json**](https://webassembly.github.io/wabt/doc/wast2json.1.html): convert a file in the wasm spec test format to a JSON file and associated wasm binary files
 - [**wasm-stats**](https://webassembly.github.io/wabt/doc/wasm-stats.1.html): output stats for a module
 - [**spectest-interp**](https://webassembly.github.io/wabt/doc/spectest-interp.1.html): read a Spectest JSON file, and run its tests in the interpreter

These tools are intended for use in (or for development of) toolchains or other
systems that want to manipulate WebAssembly files. Unlike the WebAssembly spec
interpreter (which is written to be as simple, declarative and "speccy" as
possible), they are written in C/C++ and designed for easier integration into
other systems. Unlike [Binaryen](https://github.com/WebAssembly/binaryen) these
tools do not aim to provide an optimization platform or a higher-level compiler
target; instead they aim for full fidelity and compliance with the spec (e.g.
1:1 round-trips with no changes to instructions).

## Online Demos

Wabt has been compiled to JavaScript via emscripten. Some of the functionality is available in the following demos:

- [index](https://webassembly.github.io/wabt/demo/)
- [wat2wasm](https://webassembly.github.io/wabt/demo/wat2wasm/)
- [wasm2wat](https://webassembly.github.io/wabt/demo/wasm2wat/)

## Supported Proposals

* Proposal: Name and link to the WebAssembly proposal repo
* flag: Flag to pass to the tool to enable/disable support for the feature
* default: Whether the feature is enabled by default
* binary: Whether wabt can read/write the binary format
* text: Whether wabt can read/write the text format
* validate: Whether wabt can validate the syntax
* interpret: Whether wabt can execute these operations in `wasm-interp` or `spectest-interp`
* wasm2c: Whether wasm2c supports these operations

| Proposal   | flag | default | binary | text | validate | interpret | wasm2c |
| --------------------- | --------------------------- | - | - | - | - | - | - |
| [exception handling][]| `--enable-exceptions`       |   | ✓ | ✓ | ✓ | ✓ | ✓ |
| [mutable globals][]   | `--disable-mutable-globals` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| [nontrapping float-to-int conversions][] | `--disable-saturating-float-to-int` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| [sign extension][]    | `--disable-sign-extension`  | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| [simd][]              | `--disable-simd`            | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| [threads][]           | `--enable-threads`          |   | ✓ | ✓ | ✓ | ✓ |   |
| [multi-value][]       | `--disable-multi-value`     | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| [tail-call][]         | `--enable-tail-call`        |   | ✓ | ✓ | ✓ | ✓ | ✓ |
| [bulk memory][]       | `--disable-bulk-memory`     | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| [reference types][]   | `--disable-reference-types` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| [annotations][]       | `--enable-annotations`      |   |   | ✓ |   |   |   |
| [memory64][]          | `--enable-memory64`         |   | ✓ | ✓ | ✓ | ✓ | ✓ |
| [multi-memory][]      | `--enable-multi-memory`     |   | ✓ | ✓ | ✓ | ✓ | ✓ |
| [extended-const][]    | `--enable-extended-const`   |   | ✓ | ✓ | ✓ | ✓ | ✓ |
| [relaxed-simd][]      | `--enable-relaxed-simd`     |   | ✓ | ✓ | ✓ | ✓ |   |
| [custom-page-sizes][] | `--enable-custom-page-sizes`|   | ✓ | ✓ | ✓ | ✓ | ✓ |

[exception handling]: https://github.com/WebAssembly/exception-handling
[mutable globals]: https://github.com/WebAssembly/mutable-global
[nontrapping float-to-int conversions]: https://github.com/WebAssembly/nontrapping-float-to-int-conversions
[sign extension]: https://github.com/WebAssembly/sign-extension-ops
[simd]: https://github.com/WebAssembly/simd
[threads]: https://github.com/WebAssembly/threads
[multi-value]: https://github.com/WebAssembly/multi-value
[tail-call]: https://github.com/WebAssembly/tail-call
[bulk memory]: https://github.com/WebAssembly/bulk-memory-operations
[reference types]: https://github.com/WebAssembly/reference-types
[annotations]: https://github.com/WebAssembly/annotations
[memory64]: https://github.com/WebAssembly/memory64
[multi-memory]: https://github.com/WebAssembly/multi-memory
[extended-const]: https://github.com/WebAssembly/extended-const
[relaxed-simd]: https://github.com/WebAssembly/relaxed-simd
[custom-page-sizes]: https://github.com/WebAssembly/custom-page-sizes

## Cloning

Clone as normal, but don't forget to get the submodules as well:

```console
$ git clone --recursive https://github.com/WebAssembly/wabt
$ cd wabt
$ git submodule update --init
```

This will fetch the testsuite and gtest repos, which are needed for some tests.

## Building using CMake directly (Linux and macOS)

You'll need [CMake](https://cmake.org). You can then run CMake, the normal way:

```console
$ mkdir build
$ cd build
$ cmake ..
$ cmake --build .
```

This will produce build files using CMake's default build generator. Read the
CMake documentation for more information.

**NOTE**: You must create a separate directory for the build artifacts (e.g.
`build` above).  Running `cmake` from the repo root directory will not work
since the build produces an executable called `wasm2c` which conflicts with the
`wasm2c` directory.

## Building using the top-level `Makefile` (Linux and macOS)

**NOTE**: Under the hood, this uses `make` to run CMake, which then calls
`ninja` to perform that actual build.  On some systems (typically macOS), this
doesn't build properly. If you see these errors, you can build using CMake
directly as described above.

You'll need [CMake](https://cmake.org) and [Ninja](https://ninja-build.org). If
you just run `make`, it will run CMake for you, and put the result in
`out/clang/Debug/` by default:

> Note: If you are on macOS, you will need to use CMake version 3.2 or higher

```console
$ make
```

This will build the default version of the tools: a debug build using the Clang
compiler.

There are many make targets available for other configurations as well. They
are generated from every combination of a compiler, build type and
configuration.

 - compilers: `gcc`, `clang`, `gcc-i686`, `emscripten`
 - build types: `debug`, `release`
 - configurations: empty, `asan`, `msan`, `lsan`, `ubsan`, `fuzz`, `no-tests`

They are combined with dashes, for example:

```console
$ make clang-debug
$ make gcc-i686-release
$ make clang-debug-lsan
$ make gcc-debug-no-tests
```

## Building (Windows)

You'll need [CMake](https://cmake.org). You'll also need
[Visual Studio](https://www.visualstudio.com/) (2015 or newer) or
[MinGW](https://www.mingw-w64.org/).

_Note: Visual Studio 2017 and later come with CMake (and the Ninja build system)
out of the box, and should be on your PATH if you open a Developer Command prompt.
See <https://aka.ms/cmake> for more details._

You can run CMake from the command prompt, or use the CMake GUI tool. See
[Running CMake](https://cmake.org/runningcmake/) for more information.

When running from the commandline, create a new directory for the build
artifacts, then run cmake from this directory:

```console
> cd [build dir]
> cmake [wabt project root] -DCMAKE_BUILD_TYPE=[config] -DCMAKE_INSTALL_PREFIX=[install directory] -G [generator]
```

The `[config]` parameter should be a CMake build type, typically `DEBUG` or `RELEASE`.

The `[generator]` parameter should be the type of project you want to generate,
for example `"Visual Studio 14 2015"`. You can see the list of available
generators by running `cmake --help`.

To build the project, you can use Visual Studio, or you can tell CMake to do it:

```console
> cmake --build [wabt project root] --config [config] --target install
```

This will build and install to the installation directory you provided above.

So, for example, if you want to build the debug configuration on Visual Studio 2015:

```console
> mkdir build
> cd build
> cmake .. -DCMAKE_BUILD_TYPE=DEBUG -DCMAKE_INSTALL_PREFIX=..\ -G "Visual Studio 14 2015"
> cmake --build . --config DEBUG --target install
```

## Adding new keywords to the lexer

If you want to add new keywords, you'll need to install
[gperf](https://www.gnu.org/software/gperf/). Before you upload your PR, please
run `make update-gperf` to update the prebuilt C++ sources in `src/prebuilt/`.

## Running wat2wasm

Some examples:

```sh
# parse test.wat and write to .wasm binary file with the same name
$ bin/wat2wasm test.wat

# parse test.wat and write to binary file test.wasm
$ bin/wat2wasm test.wat -o test.wasm

# parse spec-test.wast, and write verbose output to stdout (including the
# meaning of every byte)
$ bin/wat2wasm spec-test.wast -v
```

You can use `--help` to get additional help:

```console
$ bin/wat2wasm --help
```

Or try the [online demo](https://webassembly.github.io/wabt/demo/wat2wasm/).

## Running wasm2wat

Some examples:

```sh
# parse binary file test.wasm and write text file test.wat
$ bin/wasm2wat test.wasm -o test.wat

# parse test.wasm and write test.wat
$ bin/wasm2wat test.wasm -o test.wat
```

You can use `--help` to get additional help:

```console
$ bin/wasm2wat --help
```

Or try the [online demo](https://webassembly.github.io/wabt/demo/wasm2wat/).

## Running wasm-interp

Some examples:

```sh
# parse binary file test.wasm, and type-check it
$ bin/wasm-interp test.wasm

# parse test.wasm and run all its exported functions
$ bin/wasm-interp test.wasm --run-all-exports

# parse test.wasm, run the exported functions and trace the output
$ bin/wasm-interp test.wasm --run-all-exports --trace

# parse test.json and run the spec tests
$ bin/wasm-interp test.json --spec

# parse test.wasm and run all its exported functions, setting the value stack
# size to 100 elements
$ bin/wasm-interp test.wasm -V 100 --run-all-exports
```

You can use `--help` to get additional help:

```console
$ bin/wasm-interp --help
```

## Running wast2json

See [wast2json.md](docs/wast2json.md).

## Running wasm-decompile

For example:

```sh
# parse binary file test.wasm and write text file test.dcmp
$ bin/wasm-decompile test.wasm -o test.dcmp
```

You can use `--help` to get additional help:

```console
$ bin/wasm-decompile --help
```

See [decompiler.md](docs/decompiler.md) for more information on the language
being generated.

## Running wasm2c

See [wasm2c.md](wasm2c/README.md)

## Running the test suite

See [test/README.md](test/README.md).

## Sanitizers

To build with the [LLVM sanitizers](https://github.com/google/sanitizers),
append the sanitizer name to the target:

```console
$ make clang-debug-asan
$ make clang-debug-msan
$ make clang-debug-lsan
$ make clang-debug-ubsan
```

There are configurations for the Address Sanitizer (ASAN), Memory Sanitizer
(MSAN), Leak Sanitizer (LSAN) and Undefined Behavior Sanitizer (UBSAN). You can
read about the behaviors of the sanitizers in the link above, but essentially
the Address Sanitizer finds invalid memory accesses (use after free, access
out-of-bounds, etc.), Memory Sanitizer finds uses of uninitialized memory,
the Leak Sanitizer finds memory leaks, and the Undefined Behavior Sanitizer
finds undefined behavior (surprise!).

Typically, you'll just want to run all the tests for a given sanitizer:

```console
$ make test-asan
```

You can also run the tests for a release build:

```console
$ make test-clang-release-asan
...
```

The GitHub actions bots run all of these tests (and more). Before you land a change,
you should run them too. One easy way is to use the `test-everything` target:

```console
$ make test-everything
```

## Fuzzing

To build using the [LLVM fuzzer support](https://llvm.org/docs/LibFuzzer.html),
append `fuzz` to the target:

```console
$ make clang-debug-fuzz
```

This will produce a `wasm2wat_fuzz` binary. It can be used to fuzz the binary
reader, as well as reproduce fuzzer errors found by
[oss-fuzz](https://github.com/google/oss-fuzz/tree/master/projects/wabt).

```console
$ out/clang/Debug/fuzz/wasm2wat_fuzz ...
```

See the [libFuzzer documentation](https://llvm.org/docs/LibFuzzer.html) for
more information about how to use this tool.

## Installing prebuilt binaries

Wabt is available on many platforms as prepackaged binaries. For example, if
you use Homebrew you can use:
```sh
brew install wabt
```

And you use an apt-based linux distribution you can use:
```sh
sudo apt install wabt
```

You can also download prebuilt binaries for many platforms directly from the
github releases page.
