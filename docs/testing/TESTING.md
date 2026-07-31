# Testing and runtime verification

## Reading goal and design rationale

This document explains how to turn the framework's architectural claims into
repeatable evidence: unit tests cover protocol and component boundaries,
black-box tests cover observable HTTP behavior, sanitizers cover lifetime and
concurrency defects, and fuzzing stresses the incremental parser. The layers
are intentionally complementary; no single green test run proves the whole
server correct.

Use [../architecture/ARCHITECTURE.md](../architecture/ARCHITECTURE.md) to identify the invariants being tested,
then use [../performance/BENCHMARK.md](../performance/BENCHMARK.md) only after correctness checks pass.
This ordering helps close the Web project around stable behavior instead of
adding features faster than they can be verified. It also establishes a
portable workflow for the robotics roadmap: ROS 2 communication, device
protocols, and real-time paths will likewise need component tests, end-to-end
fault cases, runtime instrumentation, and explicit environment records.

The server and its tests target Linux. A C++20 compiler, CMake 3.16 or newer,
Python 3, and a system installation of GoogleTest are required. On Debian or
Ubuntu:

```sh
sudo apt update
sudo apt install build-essential cmake python3 libgtest-dev
```

The build intentionally does not use `FetchContent` or download dependencies.
When `BUILD_TESTING=ON` and GoogleTest is unavailable, CMake stops with an
installation hint. Production-only builds can use `-DBUILD_TESTING=OFF`.

## Unit and integration tests

Run the complete suite:

```sh
bash scripts/run_tests.sh
```

Equivalent manual commands:

```sh
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-tests --parallel
ctest --test-dir build-tests --output-on-failure
```

CTest registers the GoogleTest cases and, on Linux with Python 3 available, the
`http_blackbox` integration test. The integration script starts the built
server in its own process group, waits for port 8080, checks the HTTP behavior,
and always terminates the process group. Port 8080 must be free.

To run only the black-box test against an existing build:

```sh
python3 tests/integration/http_blackbox.py --server ./build-tests/webserver
```

The black-box checks cover the root and dynamic user routes, authorization
short-circuiting, connection reuse, a chunked response, malformed protocol
input, and the 1 MiB pending-input limit.

## Sanitizers

Use a separate build directory for each sanitizer configuration:

```sh
# AddressSanitizer plus UndefinedBehaviorSanitizer
bash scripts/run_tests.sh -DWEBSERVER_ENABLE_ASAN=ON -DWEBSERVER_ENABLE_UBSAN=ON

# ThreadSanitizer (must not be combined with ASan or UBSan)
BUILD_DIR=build-tsan bash scripts/run_tests.sh -DWEBSERVER_ENABLE_TSAN=ON
```

The options instrument the reusable server core, executable, and unit tests.
Clang or GCC with the corresponding sanitizer runtime is required.

## HttpParser fuzzing

The libFuzzer target is disabled by default and requires Clang:

```sh
CC=clang CXX=clang++ cmake -S . -B build-fuzz \
  -DBUILD_TESTING=OFF \
  -DWEBSERVER_BUILD_FUZZER=ON \
  -DWEBSERVER_ENABLE_ASAN=ON \
  -DWEBSERVER_ENABLE_UBSAN=ON
cmake --build build-fuzz --target http_parser_fuzz --parallel
mkdir -p fuzz-corpus
./build-fuzz/http_parser_fuzz fuzz-corpus -max_len=2097152
```

The harness feeds input incrementally to exercise streaming request parsing,
resets after complete pipelined requests, and retains parser state across
partial inputs.
