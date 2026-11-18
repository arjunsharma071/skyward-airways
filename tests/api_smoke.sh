#!/usr/bin/env bash
set -euo pipefail

base_url="${BASE_URL:-http://127.0.0.1:8080}"
passenger_id="CI-PASSENGER"
passenger_password="CI-Test-Password-2031"

wait_for_health() {
  for _ in $(seq 1 60); do
    if curl --fail --silent "${base_url}/api/health" >/dev/null; then
      return
    fi
    sleep 1
  done
  echo "Backend did not become healthy at ${base_url}" >&2
  return 1
}

wait_for_health
curl --fail --silent "${base_url}/api/health" | jq -e '.status == "ok"' >/dev/null
curl --fail --silent "${base_url}/" | grep -qi '<!doctype html>'
curl --fail --silent "${base_url}/css/style.css" | grep -q 'font-family'
curl --fail --silent "${base_url}/js/app.js" | grep -q 'addEventListener'

curl --fail --silent --get "${base_url}/api/flights" \
  --data-urlencode "source=DemoCity" \
  --data-urlencode "destination=SampleCity" \
  --data-urlencode "date=2031-02-03" \
  --data-urlencode "passengers=1" |
  jq -e '(.flights | length) == 1 and .flights[0].number == "CI100"' >/dev/null

registration="$(curl --fail --silent --show-error \
  -H 'Content-Type: application/json' \
  -d "{\"id\":\"${passenger_id}\",\"name\":\"CI Synthetic Passenger\",\"contact\":\"ci@example.invalid\",\"password\":\"${passenger_password}\"}" \
  "${base_url}/api/auth/register")"
passenger_token="$(jq -er '.token' <<<"${registration}")"

duplicate_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
  -H 'Content-Type: application/json' \
  -d "{\"id\":\"${passenger_id}\",\"name\":\"Duplicate\",\"contact\":\"duplicate@example.invalid\",\"password\":\"${passenger_password}\"}" \
  "${base_url}/api/auth/register")"
test "${duplicate_status}" = "409"

wrong_password_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
  -H 'Content-Type: application/json' \
  -d "{\"id\":\"${passenger_id}\",\"password\":\"Incorrect-Password\"}" \
  "${base_url}/api/auth/passenger/login")"
test "${wrong_password_status}" = "401"

booking_response="$(curl --fail --silent --show-error \
  -H "Authorization: Bearer ${passenger_token}" \
  -H 'Content-Type: application/json' \
  -d '{"flightNumber":"CI100","travelClass":"E","passengerCount":1,"seatNumbers":["1A"]}' \
  "${base_url}/api/bookings")"
pnr="$(jq -er '.booking.pnr' <<<"${booking_response}")"
jq -e '.booking.cancelled == false and .booking.seatNumbers == ["1A"]' \
  <<<"${booking_response}" >/dev/null

curl --fail --silent --show-error \
  -H "Authorization: Bearer ${passenger_token}" \
  "${base_url}/api/bookings/${pnr}" |
  jq -e --arg pnr "${pnr}" '.booking.pnr == $pnr' >/dev/null

seat_state="$(curl --fail --silent \
  "${base_url}/api/flights/CI100/seats?travelClass=E" |
  jq -er '.seats[] | select(.number == "1A") | .occupied')"
test "${seat_state}" = "true"

curl --fail --silent --show-error -X DELETE \
  -H "Authorization: Bearer ${passenger_token}" \
  "${base_url}/api/bookings/${pnr}" |
  jq -e '.booking.cancelled == true and .booking.status == "Cancelled"' >/dev/null

seat_state="$(curl --fail --silent \
  "${base_url}/api/flights/CI100/seats?travelClass=E" |
  jq -er '.seats[] | select(.number == "1A") | .occupied')"
test "${seat_state}" = "false"

repeat_cancel_status="$(curl --silent --output /dev/null --write-out '%{http_code}' \
  -X DELETE \
  -H "Authorization: Bearer ${passenger_token}" \
  "${base_url}/api/bookings/${pnr}")"
test "${repeat_cancel_status}" = "409"

admin_login="$(curl --fail --silent --show-error \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"${CI_ADMIN_USERNAME}\",\"password\":\"${CI_ADMIN_PASSWORD}\"}" \
  "${base_url}/api/auth/admin/login")"
admin_token="$(jq -er '.token' <<<"${admin_login}")"
admin_header="Authorization: Bearer ${admin_token}"
curl --fail --silent -H "${admin_header}" "${base_url}/api/admin/stats" |
  jq -e '.registeredPassengers == 1 and .totalBookings == 1' >/dev/null

new_flight='{"number":"CI200","airline":"CI Airways","source":"OtherCity","destination":"DemoCity","date":"2031-02-04","departureTime":"12:00","arrivalTime":"14:00","baseFare":1500,"economySeats":2,"businessSeats":1}'
curl --fail --silent -H "${admin_header}" -H 'Content-Type: application/json' \
  -d "${new_flight}" "${base_url}/api/admin/flights" |
  jq -e '.flight.number == "CI200"' >/dev/null
updated_flight='{"number":"CI200","airline":"CI Airways Updated","source":"OtherCity","destination":"DemoCity","date":"2031-02-04","departureTime":"12:30","arrivalTime":"14:30","baseFare":1600,"economySeats":2,"businessSeats":1}'
curl --fail --silent -X PUT -H "${admin_header}" -H 'Content-Type: application/json' \
  -d "${updated_flight}" "${base_url}/api/admin/flights/CI200" |
  jq -e '.flight.airline == "CI Airways Updated"' >/dev/null
curl --fail --silent -X DELETE -H "${admin_header}" \
  "${base_url}/api/admin/flights/CI200" |
  jq -e '.message' >/dev/null

docker compose restart postgres
for _ in $(seq 1 60); do
  if docker compose exec -T postgres sh -c 'pg_isready -U "$POSTGRES_USER" -d "$POSTGRES_DB"' >/dev/null 2>&1; then
    break
  fi
  sleep 1
done
docker compose restart backend
wait_for_health

new_login="$(curl --fail --silent --show-error \
  -H 'Content-Type: application/json' \
  -d "{\"id\":\"${passenger_id}\",\"password\":\"${passenger_password}\"}" \
  "${base_url}/api/auth/passenger/login")"
new_passenger_token="$(jq -er '.token' <<<"${new_login}")"
curl --fail --silent -H "Authorization: Bearer ${new_passenger_token}" \
  "${base_url}/api/bookings/mine" |
  jq -e --arg pnr "${pnr}" \
    '(.bookings | length) == 1 and .bookings[0].pnr == $pnr and .bookings[0].cancelled == true' >/dev/null

echo "[pass] health, static assets, flight search, registration/login, duplicate and invalid login, booking, ticket retrieval, seat release, cancellation, admin flight CRUD/reports, and database/container restart persistence"
