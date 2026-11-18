# Testing and Verification

## Current verification boundary

The earlier Windows version compiled, but its launch was blocked by Windows
Code Integrity. The current portability work adds automated C++ and container
API tests; they must still be run in an approved Linux/Docker environment.
Do not treat static inspection or compilation as a runtime or browser test.

## Automated C++ tests

The CMake test executable uses temporary synthetic data and checks passenger
registration/authentication, duplicate IDs, flight search, unique PNRs,
overbooking rejection, cancellation, seat release, ticket lookup, and
file-backed persistence after restart:

```bash
cmake -S . -B build -DSKYWARD_WITH_POSTGRES=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

PostgreSQL integration tests exercise one-time import from synthetic legacy
files, confirm the source files remain unchanged, and check database-backed
booking and persistence after restart. Run them only against a disposable test
database:

```bash
cmake -S . -B build-pg -DSKYWARD_WITH_POSTGRES=ON
cmake --build build-pg --parallel
TEST_DATABASE_URL='postgresql://USER:PASSWORD@127.0.0.1:5432/EMPTY_TEST_DB' \
  ctest --test-dir build-pg --output-on-failure
```

## Docker/API integration tests

The GitHub Actions workflow builds the Linux backend, runs PostgreSQL-enabled
C++ tests, starts Compose with synthetic fixtures, exercises the real REST
API and static assets, restarts the database/backend containers, and checks
AMD64/ARM64 image builds. To run the same API workflow locally with Docker
Compose v2, `curl`, `jq`, and Bash:

```bash
cp .env.example .env
# Replace all placeholders; use a test-only admin password and database secret.
# Set LEGACY_DATA_PATH=./tests/fixtures/data and HTTP_PORT=18080 in .env.
docker compose up --build -d
BASE_URL=http://127.0.0.1:18080 \
CI_ADMIN_USERNAME=your-test-admin \
CI_ADMIN_PASSWORD=your-test-admin-password \
bash tests/api_smoke.sh
docker compose down
```

This workflow uses the Compose PostgreSQL volume. If rerunning it against an
existing test volume, use a fresh disposable database/volume and synthetic
fixture path; do not point it at production data. `docker compose down --volumes`
removes the database volume and must not be used when preserving records.

The Windows source can be checked without executing an unsigned executable:

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DSKYWARD_WITH_POSTGRES=OFF
cmake --build build --config Release
```

Do not launch the executable until the device administrator has explicitly
approved the exact build and Windows permits it. Linux, macOS, container
runtime, and browser results must be reported separately and only after they
have actually been run.

## Manual test workflow after approval

Use synthetic passenger details only. Registration, bookings, cancellations,
and any admin flight changes persist under `data\`; inspect and preserve the
existing records rather than resetting them.

### Passenger and booking flow

1. Load the homepage and verify the hero/local images, navigation, search form,
   featured flights, and responsive layout.
2. Register a synthetic passenger. Verify a successful response and that the
   passenger file contains a `pbkdf2-sha256$...` password hash rather than the
   submitted password.
3. Attempt registration with the same passenger ID; confirm it is rejected.
   Also verify passwords shorter than 12 characters are rejected before
   registration.
4. Sign out and sign in with the correct password; then try a wrong password
   and confirm authentication is rejected.
5. Search an existing seeded route/date. Verify flight number, airline,
   schedule, fares, and available seats. Exercise passenger count and empty
   results.
6. Select a class and seats, review the server-calculated fare, extras, and
   promo-code result, then confirm a booking and unique PNR.
7. Fetch the ticket and inspect its passenger, flight, class, fare, status, and
   seat details. Confirm another booking cannot take an occupied seat or
   exceed remaining capacity.
8. Cancel the ticket as its owner. Confirm the status/refund result, verify the
   seat is released, and confirm a second cancellation is rejected. Try viewing
   or cancelling another passenger's booking and confirm access is denied.
9. Update the profile and verify the preferences persist.

### Administrator flow

1. Sign in with the documented demonstration administrator account.
2. Add a flight with synthetic schedule and capacity values; verify it appears
   in search and admin listings.
3. Edit its details, then delete it only after confirming it has no bookings.
4. Inspect passenger/booking reports and verify totals, occupancy, and revenue.
5. Try admin routes without a token and with a passenger token; confirm
   unauthenticated and wrong-role requests are rejected.

### Persistence, migration, and browser checks

1. Stop and restart the server normally. Sign in again, then confirm passenger,
   flight, booking, and cancellation data remain.
2. Test legacy passenger-file migration only on a separate copy of the project
   data, never on the user's only copy. Confirm the record survives and its
   password becomes a salted hash after the first startup.
3. In browser developer tools, inspect the Console and Network tabs for failed
   assets, JavaScript exceptions, incorrect API status codes, and failed
   requests. Exercise navigation, forms, seat map, ticket print, and mobile
   viewport layouts.
4. Confirm status messages identify flight tracking and any payment interaction
   as demonstration-only; no live airline or payment service is connected.

Record each result as passed, failed, or not run with the date and build hash.
Do not mark any live or browser test as passed until it has actually been run.
