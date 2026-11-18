# Feature Guide

This guide describes what the current application actually does and separates
working application behavior from demonstration-only values and integrations.

## Find flights

1. Enter a source, destination, departure date, and passenger count on the home
   page.
2. Search results come from `GET /api/flights`; each result shows the scheduled
   airline, flight number, times, Economy and Business fares, and available
   seats.
3. Filter the returned schedules by airline, available cabin, departure-time
   window, or fare-per-traveller range. Sort by fare, departure, arrival, or
   airline. These are client-side filters over the real API response.
4. The fare calendar is calculated from flights in the current schedule. It is
   not a third-party fare search.

Flight fares are calculated from the stored schedule: Economy uses the base
fare and Business uses 1.8 times that fare. The browser's displayed availability
is a snapshot; the C++ backend checks seats again when confirming a booking.

## Passenger account and reservation

- Register with a unique passenger ID, name, contact, and a password of at
  least 12 characters. Passwords are stored as salted PBKDF2 hashes.
- Sign in to view the passenger profile and existing bookings. Bearer sessions
  expire after 30 minutes idle or when the server restarts.
- Choose a cabin, passenger count, and one currently available seat per
  traveller. The backend verifies all selections as part of reservation
  creation, preventing overbooking through concurrent requests.
- The displayed fare includes any selected demo seat charges, extras, and
  applicable `SKYWARD10` discount. Booking creation returns the assigned PNR.
- Open the booking in the dashboard to view its ticket or cancel it. A
  successful cancellation is recorded once and releases the seats in future
  availability checks.

Passengers can edit their profile and preferences. Loyalty miles and demo
tiers are derived from active bookings; they are not a separately audited
rewards ledger.

## Administrator

After administrator sign-in, the dashboard lists schedules, passengers, and
bookings; it also displays seat occupancy and a simulated active booking-value
total. Administrators may add, edit, and delete flights. A flight with active
bookings cannot be deleted, and seat capacity cannot be reduced below its
active reservations.

The local development account is `admin` / `admin123`. It is only a local
development default. Configure a unique administrator username and strong
password for a deployment. The login screen intentionally does not display
credentials.

## What this demonstration does not do

- It does not collect, authorize, capture, or refund money. Booking fare and
  cancellation refund amounts are simulated values.
- It does not create reservations with an airline or guarantee a real seat.
- Airline status is projected only from the stored flight date; gate, terminal,
  delay, and cancellation information is not connected to an operations feed.
- Add-ons, promotional savings, and rewards are not sent to ancillary-service,
  insurance, loyalty, or payment providers.
- There are no email/SMS confirmations or production passenger identity
  checks.

Use synthetic passenger information and demo schedules. For local builds,
deployment, and API details, see [BUILD.md](BUILD.md),
[DEPLOYMENT.md](DEPLOYMENT.md), and [API_DOCUMENTATION.md](API_DOCUMENTATION.md).
