# Architecture

Skyward Airways is a single-process C++17 web application. One executable
serves both the REST API and the static single-page frontend. The backend owns
authentication, validation, fares, seat assignment, booking rules, and
persistence; browser code does not create simulated API responses.

## Request path

```text
Browser
  ├── HTML, CSS, JavaScript, and local images
  └── JSON REST requests
        ↓
cpp-httplib routes in backend/main.cpp
        ↓
AirlineReservationSystem domain service
  ├── synchronized in-process flight/passenger/booking state
  ├── file persistence for local development
  └── PostgreSQL adapter for deployment
```

`backend/main.cpp` reads environment configuration, creates the domain service,
registers API handlers, mounts `frontend/`, and starts the HTTP listener. Run it
from the project root so the relative frontend and schema paths resolve. API
responses use JSON; errors contain an `error` string and an HTTP status.
`backend/httplib.h` and `backend/json.hpp` are vendored header-only
dependencies.

## Domain and persistence

`AirlineReservationSystem` keeps flights, passengers, and bookings in STL
vectors, protects shared operations with a mutex, and performs validation and
business calculations in C++. `User` is the base class for `Passenger` and
`Admin`; `Ticket` is the polymorphic base for `EconomyTicket` and
`BusinessTicket`. Booking persistence is updated only after the C++ service
checks party size, current seat availability, selected seats, add-ons, and the
promotion code.

Local development uses `data/flights.txt`, `data/passengers.txt`, and
`data/bookings.txt`. File writes use a temporary file before replacing a data
file. The parser accepts both LF and CRLF records while retaining strict field
validation. Existing passenger and booking layouts are read for compatibility;
legacy plaintext passenger passwords are migrated to PBKDF2 hashes on startup.
Back up the data directory before running a build against real records.

With `DATABASE_URL` and PostgreSQL support enabled, the application initializes
the idempotent schema in `database/schema.sql` and uses the PostgreSQL record
store. Legacy file imports are tracked in `skyward_legacy_imports`; the source
files are not modified by the import. See [DEPLOYMENT.md](DEPLOYMENT.md) for
backup, deployment, and restore guidance.

## Authentication and configuration

Passenger passwords are salted PBKDF2-HMAC-SHA256 hashes; Windows uses CNG and
Linux/macOS use OpenSSL. Secure session tokens come from the platform
cryptographic random generator. Bearer sessions are stored in memory, expire
after 30 minutes without authenticated activity, and are limited to 10,000
entries per process. Restarting the process invalidates them.

`APP_ENV=development` uses the documented local-development admin defaults and
file persistence. Production requires `DATABASE_URL`, explicit admin
credentials, and an admin password of at least 16 characters. Configuration
must be supplied by the deployment environment or secret manager, never by
frontend assets. The C++ service does not terminate TLS; use a reverse proxy or
managed HTTPS load balancer.

## Scaling and integration limits

The database adapter currently loads an application snapshot and replaces a
record type when saving. It is intended for a single active backend process;
multiple writers can overwrite each other's stale snapshots. PostgreSQL alone
does not make the application multi-instance safe. Sessions are process-local.
Horizontal scaling requires a concurrency-safe data model and shared session
storage.

The application has no payment processor, airline/GDS, live inventory,
operations, email/SMS, or real ancillary-service integration. Displayed fares,
refunds, rewards, add-ons, and flight status are demonstrations. Do not treat
these values as financial transactions, airline-confirmed reservations, or
operational flight information.
