# REST API

The C++ server exposes JSON endpoints on `http://127.0.0.1:8080`. Successful
responses use JSON. Errors have the form `{"error":"..."}`. Passenger/admin
endpoints require the bearer token returned by the corresponding login route:

```http
Authorization: Bearer <token>
```

Tokens are in-memory, expire after 30 minutes without authenticated activity,
and are cleared when the server stops. The service listens only on loopback;
it is a demonstration service, not a production airline API.

## Public endpoints

| Method and path | Request | Success |
| --- | --- | --- |
| `GET /api/health` | None | `{"status":"ok","service":"Airline Reservation System"}` |
| `GET /api/flights?source=Delhi&destination=Mumbai&date=2026-12-15&passengers=1` | Optional query parameters | `{"flights":[...]}` |
| `GET /api/fares/calendar?source=Delhi&destination=Mumbai` | Route query parameters | `{"fares":[{"date":"...","economyFare":...}]}` |
| `GET /api/flights/{flightNumber}/seats?travelClass=E` | `travelClass` is `E` or `B` | `{"seats":[{"number":"...","type":"...","surcharge":0,"occupied":false}]}` |
| `GET /api/flights/{flightNumber}/status` | None | Schedule-based demo status; not a live airline feed |
| `POST /api/auth/register` | `{"id":"...","name":"...","contact":"...","password":"..."}` | `201` with token, passenger profile, and role |
| `POST /api/auth/passenger/login` | `{"id":"...","password":"..."}` | `200` with token, profile, and role |
| `POST /api/auth/admin/login` | `{"username":"admin","password":"admin123"}` | `200` with token, admin profile, and role |

Flight search results include flight number, airline, source, destination,
date, departure/arrival times, economy/business fares and capacities, and
available seats. Search accepts `source`, `destination`, `date`, and
`passengers`. The passenger count must be between 1 and 9.
New passenger passwords must contain 12–200 characters. Existing legacy
accounts remain able to authenticate after their stored password is migrated.

## Signed-in passenger endpoints

| Method and path | Request | Access / success |
| --- | --- | --- |
| `GET /api/auth/me` | None | Signed-in passenger or administrator |
| `POST /api/auth/logout` | None | Signed-in user; invalidates bearer token |
| `PUT /api/passengers/me` | `{"name":"...","contact":"...","preferredTravelClass":"E","dietaryPreference":"","accessibilityPreference":""}` | Passenger; returns updated profile |
| `POST /api/bookings` | `{"flightNumber":"...","travelClass":"E","passengerCount":1,"seatNumbers":[],"addOns":[],"promoCode":""}` | Passenger; `201` and confirmed booking |
| `GET /api/bookings/mine` | None | Passenger's bookings |
| `GET /api/bookings/{pnr}` | None | Booking owner or administrator |
| `DELETE /api/bookings/{pnr}` | None | Booking owner; cancels once and releases seats |

`travelClass` is `E` for Economy or `B` for Business. Supported add-on identifiers
and promo rules are validated by the C++ backend. The backend, not the browser,
calculates the final fare. Seat selection and promo values are validated again
at booking time.

## Administrator endpoints

All `/api/admin/*` routes require an administrator bearer token.

| Method and path | Request | Success |
| --- | --- | --- |
| `GET /api/admin/flights` | None | `{"flights":[...]}` |
| `POST /api/admin/flights` | Flight object: `number`, `airline`, `source`, `destination`, `date`, `departureTime`, `arrivalTime`, `baseFare`, `economySeats`, `businessSeats` | `201` and created flight |
| `PUT /api/admin/flights/{flightNumber}` | Same flight object | Updated flight |
| `DELETE /api/admin/flights/{flightNumber}` | None | Confirmation; flights with bookings cannot be removed |
| `GET /api/admin/passengers` | None | Passenger profiles (never password hashes) |
| `GET /api/admin/bookings` | None | Booking records |
| `GET /api/admin/stats` | None | Flight, passenger, booking, occupancy, and simulated booking-value totals |

`activeRevenue` is retained as a legacy JSON field name; it is the sum of
active demo booking fares, not collected revenue. No payment is processed.

## Error status codes

- `400 Bad Request`: invalid JSON or validation failure.
- `401 Unauthorized`: missing/invalid session or credentials.
- `403 Forbidden`: signed-in user lacks the required role or owns no such ticket.
- `404 Not Found`: requested record or endpoint does not exist.
- `409 Conflict`: duplicate ID, unavailable seat, or other business-rule conflict.
- `500 Internal Server Error`: unexpected server failure; the response does not
  expose internal exception details.

The server logs HTTP method, path, status, and unexpected error messages. It
does not intentionally log request bodies, passwords, bearer tokens, or
passenger password hashes.
