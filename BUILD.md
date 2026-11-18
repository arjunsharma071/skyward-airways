# Local Build Guide

The project is C++17. CMake builds the same backend sources on Windows, Linux,
and macOS; Windows uses native CNG, while Linux/macOS use OpenSSL for PBKDF2
password hashes and secure random tokens. The HTTP server and static frontend
are served by the C++ application.

## Windows with MinGW-w64

Requirements: CMake 3.20+, GCC/G++ with POSIX threads, and the MinGW Windows
SDK import libraries (`bcrypt`, `ws2_32`, and `shell32`). No PostgreSQL client
is needed for local file-based development.

From PowerShell in the project root:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DSKYWARD_WITH_POSTGRES=OFF
cmake --build build --config Release
```

If CMake is not installed but MinGW GCC is available, the equivalent server
link command is:

```powershell
New-Item -ItemType Directory -Path build -Force | Out-Null
g++ -std=c++17 -Wall -Wextra -pedantic -Ibackend `
  backend\main.cpp backend\AirlineReservationSystem.cpp backend\PostgresStore.cpp `
  -lws2_32 -lbcrypt -lshell32 -pthread -o build\airline_reservation_server.exe
```

The executable is `build\airline_reservation_server.exe`. Windows Code
Integrity previously blocked unsigned local builds on this device. Compilation
does not authorize execution. Do not run it unless the device policy
administrator has explicitly approved this exact build and Windows permits
execution; do not bypass the policy.

After authorized approval only, start it from the project root so relative
frontend/data paths resolve:

```powershell
.\build\airline_reservation_server.exe
```

The local server defaults to `127.0.0.1:8080`. Use `Ctrl+C` to stop it.

## Linux / Ubuntu on WSL 2

On a Windows host without an installed Linux distribution, an administrator
must install the supported Ubuntu environment and complete its first-run setup:

```powershell
wsl --install -d Ubuntu-24.04
```

Restart Windows if requested, then finish the Ubuntu username/password setup.
This is a standard Linux development environment; it does not authorize or
bypass execution blocked by Windows Code Integrity.

Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential cmake libssl-dev
```

For PostgreSQL persistence and the database integration tests, also install
`libpq-dev` and make a disposable test database available.

Build the local file-backed server and test executable:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSKYWARD_WITH_POSTGRES=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run the C++ server from the project root:

```bash
./build/airline_reservation_server
```

To build PostgreSQL support:

```bash
sudo apt install -y libpq-dev
cmake -S . -B build-pg -DCMAKE_BUILD_TYPE=Release \
  -DSKYWARD_WITH_POSTGRES=ON -DSKYWARD_BUILD_TESTS=ON
cmake --build build-pg --parallel
TEST_DATABASE_URL='postgresql://USER:PASSWORD@127.0.0.1:5432/EMPTY_TEST_DB' \
  ctest --test-dir build-pg --output-on-failure
```

`TEST_DATABASE_URL` must refer to a disposable database: the integration test
creates the schema and synthetic records. The test only runs the PostgreSQL
case when this variable is set.

## macOS

The source contains OpenSSL, POSIX atomic-file replacement, and secure-token
support for macOS. This platform path has not yet been built or runtime-tested.
Install dependencies with Homebrew:

```bash
brew install cmake openssl@3 libpq
```

Then configure CMake with Homebrew's dependency prefixes if CMake does not find
them automatically:

```bash
cmake -S . -B build \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" \
  -DPostgreSQL_ROOT="$(brew --prefix libpq)" \
  -DSKYWARD_WITH_POSTGRES=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Omit `-DSKYWARD_WITH_POSTGRES=ON` for local file-based development. Confirm the
build and runtime behavior on each macOS release and CPU architecture before
claiming support for it.

## Runtime configuration for local development

The default `APP_ENV=development` uses file persistence in `data/`, binds to
`127.0.0.1:8080`, and provides the demonstration administrator `admin` /
`admin123`. Set `DATA_DIRECTORY`, `SERVER_HOST`, and `SERVER_PORT` to override
those defaults. Never use development credentials or real passenger data in a
deployed environment. Production environment requirements and database setup
are in [DEPLOYMENT.md](DEPLOYMENT.md).

Before any first run that can migrate legacy passenger files, make and verify a
separate recoverable copy of `data/`. The project includes no command that
deletes or replaces that backup.
