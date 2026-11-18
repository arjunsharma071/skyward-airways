# Airline Reservation System

A full-stack airline reservation website. The backend is C++17 with
cpp-httplib and nlohmann/json; the responsive frontend uses HTML, CSS, and
vanilla JavaScript. Local development can use separate pipe-delimited files;
Docker and deployed configurations use PostgreSQL, with a non-destructive
one-time import of existing files.

## Features

- Search flights by source, destination, date, and party size.
- Filter results by airline, cabin availability, departure window, and fare;
  sort by fare, departure, arrival, or airline.
- Passenger registration and sign-in, booking history, e-tickets, and booking
  cancellation.
- Passenger passwords are stored as salted PBKDF2-HMAC-SHA256 hashes using
  Windows CNG or OpenSSL; legacy plaintext passenger records are migrated on
  startup.
- Session tokens are generated with the platform cryptographic RNG.
- Economy and Business fares, live seat maps, seat checks, and seat release on
  cancellation.
- Optional passenger profile preferences, loyalty miles, and Silver/Gold/
  Platinum demo tiers.
- C++-priced extras, promo validation, a fare-by-date calendar based on actual
  scheduled flights, and schedule-derived (non-live) flight status.
- Admin sign-in, flight add/edit/delete, passenger and booking lists, and
  booking, occupancy, and simulated booking-value reports.
- Persistent file records for local development and PostgreSQL records for
  Docker/cloud deployments; original import files are mounted read-only.
- Passenger and administrator routes are protected by in-memory bearer-token
  sessions with a sliding idle timeout; a server restart requires signing in
  again.
- Booking totals, cancellations/refunds, add-ons, rewards, and booking-value
  reports are demonstrations only. No payment is collected.
- Windows development startup attempts to open `http://localhost:8080/` in
  Chrome (and falls back to the default browser if Chrome cannot be launched).

## Project structure

```text
backend/
  AirlineReservationSystem.cpp
  AirlineReservationSystem.hpp
  PostgresStore.cpp
  PostgresStore.hpp
  httplib.h
  json.hpp
  main.cpp
database/
  schema.sql
frontend/
  index.html
  css/style.css
  assets/
    skyward-hero.jpg
    dubai.jpg
    coast.jpg
    london.jpg
    singapore.jpg
  js/
    admin.js
    api.js
    app.js
    auth.js
    booking.js
    utils.js
data/
  flights.txt
  passengers.txt
  bookings.txt
CMakeLists.txt
Dockerfile
docker-compose.yml
.dockerignore
.env.example
DEPLOYMENT.md
ARCHITECTURE.md
FEATURE_GUIDE.md
.github/workflows/ci.yml
tests/
README.md
BUILD.md
TESTING.md
API_DOCUMENTATION.md
```

`backend/httplib.h` is cpp-httplib v0.18.3 and `backend/json.hpp` is
nlohmann/json v3.11.3. Both are vendored header-only dependencies; no Node.js,
Python, or frontend package installation is needed.

## Local development

See [BUILD.md](BUILD.md) for CMake build instructions on Windows, Linux/WSL,
and macOS. Local development defaults to file persistence and the loopback
address. The default development admin is `admin` / `admin123`; do not use
these credentials in a deployment.

For Docker/PostgreSQL and HTTPS/cloud deployment, see [DEPLOYMENT.md](DEPLOYMENT.md).

See [TESTING.md](TESTING.md) for a synthetic-data test workflow and current
verification limits. See
[API_DOCUMENTATION.md](API_DOCUMENTATION.md) for endpoint payloads.
See [ARCHITECTURE.md](ARCHITECTURE.md) for the application boundaries and
deployment limits, and [FEATURE_GUIDE.md](FEATURE_GUIDE.md) for the workflow
and demonstration-only integrations.

## Demonstration accounts and workflow

Default administrator:

- Username: `admin`
- Password: `admin123`

Passenger demonstration:

1. Start the server and choose **Create account**. Register a passenger such as
   `P1001`, with a name, contact, and password.
2. On the home page search **Delhi → Mumbai**, **2026-12-15**, for one passenger.
   The seeded flight `AI204` should appear if the original sample schedule is
   present.
3. Choose the flight, select Economy or Business, and confirm the booking. The
   app displays a ticket with its unique PNR and assigned seat numbers.
4. On the booking page, choose one seat per traveller, optionally add services,
    and apply `SKYWARD10` for 10% off the flight fare. The C++ service validates
    the seat, add-ons, and code and calculates the final amount. Seat fees are
    ₹500 for Economy front-row seats and ₹900 for Economy exit-row seats.
    Extras are priced per traveller: checked bag ₹1,500, priority boarding ₹750,
    lounge ₹2,500, meal ₹550, or trip protection ₹900.
5. Open **My trips** to review the ticket, edit profile/preferences, and see
    loyalty miles. The demo awards one mile per ₹100 of confirmed fare and
    computes tier from accumulated miles. Search the route again to see
    availability decrease.
6. Cancel the ticket from the trip list. Its status changes to Cancelled, the
    full displayed fare is recorded as a simulated refund, and the seat becomes
    available again. A cancelled booking cannot be cancelled a second time.
7. Sign out, choose the **Admin** sign-in tab, and log in with `admin` /
   `admin123` in local development. Review the flight schedule, passenger
   list, booking report, simulated booking value, and occupancy. Add, edit, or
   delete an unbooked flight to demonstrate flight management.
8. Restart the local server and sign in again; the flight, passenger, booking,
   and cancellation records remain in `data\`.

The initial flight schedule is seeded only when `data\flights.txt` does not
contain records. Existing nine-field flight records are read without losing
their data; on the next save, they are written with the additional arrival-time
field. Passenger records in the original four-field format and booking records
in the original nine-/17-field formats remain readable. Passenger records
containing legacy plaintext passwords are validated, hashed, and atomically
replaced on startup; new saves never write passenger passwords in plaintext.
Local flight, passenger, and booking updates use temporary files and replace
the target only after the new file is fully written. PostgreSQL uses
parameterized statements and transactions. New booking saves include
profile preferences, seat assignments, add-ons, discounts, and refund amounts.
New passenger registrations require passwords of at least 12 characters.
Image files in `frontend/assets/` are local copies of Unsplash photography,
downloaded from the Unsplash CDN and used under the
[Unsplash License](https://unsplash.com/license); image use is illustrative,
and the project does not claim ownership.

## API overview

| Method | Endpoint | Access |
| --- | --- | --- |
| `GET` | `/api/health` | Public |
| `GET` | `/api/flights?source=Delhi&destination=Mumbai&date=2026-12-15&passengers=1` | Public |
| `GET` | `/api/flights/{flightNumber}/seats?travelClass=E` | Public |
| `GET` | `/api/flights/{flightNumber}/status` | Public; schedule-based demo status |
| `GET` | `/api/fares/calendar?source=Delhi&destination=Mumbai` | Public |
| `POST` | `/api/auth/register` | Public |
| `POST` | `/api/auth/passenger/login` | Public |
| `POST` | `/api/auth/admin/login` | Public |
| `GET` | `/api/auth/me` | Signed in |
| `POST` | `/api/auth/logout` | Signed in |
| `PUT` | `/api/passengers/me` | Passenger |
| `POST` | `/api/bookings` | Passenger |
| `GET` | `/api/bookings/mine` | Passenger |
| `GET`, `DELETE` | `/api/bookings/{pnr}` | Owner / admin for GET; owner for DELETE |
| `GET`, `POST` | `/api/admin/flights` | Admin |
| `PUT`, `DELETE` | `/api/admin/flights/{flightNumber}` | Admin |
| `GET` | `/api/admin/passengers`, `/api/admin/bookings`, `/api/admin/stats` | Admin |

JSON errors use an `error` field with an appropriate HTTP status. Booking and
flight updates are synchronized in the C++ service so simultaneous requests
cannot reserve the same last seat. The status route only compares a scheduled
flight date with the server date; it is not connected to an airline operations
feed and does not provide live gate, terminal, or delay information.

## OOP concepts

The C++ backend implements `User -> Passenger/Admin` and
`Ticket -> EconomyTicket/BusinessTicket`, encapsulated data, constructors and
destructors, virtual fare calculation, overloaded flight-search functions,
stream operator overloads, a friend ticket printer, a static PNR counter,
exception handling, STL vectors, a map-backed HTTP session registry, and
`fstream`-based persistence.

The web app is a demonstration, not a live airline booking or payment service.
Fare/refund amounts, rewards, and schedule-derived flight status are
simulations; no payment processor, airline reservation system, or live flight
feed is connected. Development credentials are not production credentials.
Sessions are held in memory, and this persistence adapter is designed for one
active backend instance; do not horizontally scale it without replacing the
whole-record persistence strategy with concurrency-safe transactions. The C++
service does not provide TLS; use a reverse proxy or managed HTTPS termination
and restrict network access. Do not use real passenger information in the
demonstration.
