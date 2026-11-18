import { api } from "./api.js";
import { escapeHtml, formatDate, formatMoney, notify } from "./utils.js";

let flightRecords = [];
let editingFlightNumber = "";

function emptyRow(columns, message) {
  return `<tr><td class="table-empty" colspan="${columns}">${escapeHtml(message)}</td></tr>`;
}

function renderFlights(flights) {
  const body = document.getElementById("adminFlights");
  body.innerHTML = flights.length ? flights.map((flight) => `
    <tr>
      <td><strong>${escapeHtml(flight.number)}</strong><br>${escapeHtml(flight.airline)}</td>
      <td>${escapeHtml(flight.source)} → ${escapeHtml(flight.destination)}</td>
      <td>${escapeHtml(formatDate(flight.date))}<br>${escapeHtml(flight.departureTime)} – ${escapeHtml(flight.arrivalTime)}</td>
      <td>${escapeHtml(flight.availableEconomy ?? flight.economySeats)} E · ${escapeHtml(flight.availableBusiness ?? flight.businessSeats)} B</td>
      <td><div class="table-actions"><button type="button" data-flight-action="edit" data-flight="${escapeHtml(flight.number)}">Edit</button><button type="button" class="delete-action" data-flight-action="delete" data-flight="${escapeHtml(flight.number)}">Delete</button></div></td>
    </tr>`).join("") : emptyRow(5, "No flights yet. Add the first flight to get started.");
}

function renderPassengers(passengers) {
  document.getElementById("passengerTotal").textContent = String(passengers.length);
  document.getElementById("adminPassengers").innerHTML = passengers.length
    ? passengers.map((passenger) => `<tr><td><strong>${escapeHtml(passenger.name)}</strong></td><td>${escapeHtml(passenger.id)}</td><td>${escapeHtml(passenger.contact)}</td><td>${escapeHtml(passenger.eliteTier)} · ${escapeHtml(passenger.loyaltyMiles)} miles</td></tr>`).join("")
    : emptyRow(4, "No registered passengers yet.");
}

function renderBookings(bookings) {
  document.getElementById("bookingTotal").textContent = String(bookings.length);
  document.getElementById("adminBookings").innerHTML = bookings.length
    ? bookings.map((booking) => `<tr>
      <td><strong>${escapeHtml(booking.pnr)}</strong></td>
      <td>${escapeHtml(booking.passengerName)}<br>${escapeHtml(booking.passengerId)}</td>
      <td>${escapeHtml(booking.flight.number)}<br>${escapeHtml(booking.flight.source)} → ${escapeHtml(booking.flight.destination)}</td>
      <td>${escapeHtml((booking.seatNumbers || []).join(", ") || "—")}<br><small>${escapeHtml((booking.addOns || []).join(", ") || "No add-ons")}</small></td>
      <td>${formatMoney(booking.farePaid)}<br><span class="status-pill${booking.cancelled ? " cancelled" : ""}">${escapeHtml(booking.status)}</span></td>
    </tr>`).join("")
    : emptyRow(5, "No bookings have been made yet.");
}

function renderStats(stats) {
  const values = [
    [stats.totalFlights, "Flights"],
    [stats.registeredPassengers, "Passengers"],
    [stats.activeBookings, "Active bookings"],
    [formatMoney(stats.activeRevenue), "Active booking value · demo"]
  ];
  document.getElementById("adminStats").innerHTML = values.map(([value, label]) => `
    <div class="stat-card"><span>${escapeHtml(label)}</span><strong>${escapeHtml(value)}</strong></div>`).join("") +
    `<div class="stat-card"><span>Occupancy</span><strong>${Number(stats.occupancyPercent || 0).toFixed(1)}%</strong></div>
     <div class="stat-card"><span>Reserved seats</span><strong>${escapeHtml(stats.reservedSeats)} / ${escapeHtml(stats.totalSeats)}</strong></div>
     <div class="stat-card"><span>Cancelled bookings</span><strong>${escapeHtml(stats.cancelledBookings)}</strong></div>
     <div class="stat-card"><span>Total reservations</span><strong>${escapeHtml(stats.totalBookings)}</strong></div>`;
  const occupancy = Math.min(100, Math.max(0, Number(stats.occupancyPercent) || 0));
  document.getElementById("occupancyBar").style.width = `${occupancy}%`;
  document.getElementById("occupancyCaption").textContent =
    `${occupancy.toFixed(1)}% · ${stats.reservedSeats} of ${stats.totalSeats} seats reserved`;
  document.getElementById("reservationCaption").textContent =
    `${stats.activeBookings} active · ${stats.cancelledBookings} cancelled`;
  document.getElementById("revenueCaption").textContent =
    `${formatMoney(stats.activeRevenue)} · demo value`;
}

function resetFlightForm() {
  const form = document.getElementById("flightForm");
  form.reset();
  form.hidden = true;
  editingFlightNumber = "";
  form.elements.number.disabled = false;
  document.getElementById("saveFlightButton").textContent = "Save flight";
}

function fillFlightForm(flight) {
  const form = document.getElementById("flightForm");
  editingFlightNumber = flight.number;
  form.hidden = false;
  form.elements.number.value = flight.number;
  form.elements.number.disabled = true;
  form.elements.airline.value = flight.airline;
  form.elements.source.value = flight.source;
  form.elements.destination.value = flight.destination;
  form.elements.date.value = flight.date;
  form.elements.departureTime.value = flight.departureTime;
  form.elements.arrivalTime.value = flight.arrivalTime;
  form.elements.baseFare.value = flight.baseFare;
  form.elements.economySeats.value = flight.economySeats;
  form.elements.businessSeats.value = flight.businessSeats;
  document.getElementById("saveFlightButton").textContent = "Update flight";
  form.scrollIntoView({ behavior: "smooth", block: "center" });
}

function flightFormData(form) {
  return {
    number: form.elements.number.value.trim(),
    airline: form.elements.airline.value.trim(),
    source: form.elements.source.value.trim(),
    destination: form.elements.destination.value.trim(),
    date: form.elements.date.value,
    departureTime: form.elements.departureTime.value,
    arrivalTime: form.elements.arrivalTime.value,
    baseFare: Number(form.elements.baseFare.value),
    economySeats: Number(form.elements.economySeats.value),
    businessSeats: Number(form.elements.businessSeats.value)
  };
}

export async function loadAdminDashboard() {
  const [flightData, passengerData, bookingData, stats] = await Promise.all([
    api("/api/admin/flights"),
    api("/api/admin/passengers"),
    api("/api/admin/bookings"),
    api("/api/admin/stats")
  ]);
  flightRecords = flightData.flights || [];
  renderFlights(flightRecords);
  renderPassengers(passengerData.passengers || []);
  renderBookings(bookingData.bookings || []);
  renderStats(stats);
}

export function setupAdmin() {
  document.getElementById("newFlightButton").addEventListener("click", () => {
    resetFlightForm();
    const form = document.getElementById("flightForm");
    form.hidden = false;
    form.elements.number.focus();
  });

  document.getElementById("cancelFlightEdit").addEventListener("click", resetFlightForm);

  document.getElementById("flightForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    const button = document.getElementById("saveFlightButton");
    button.disabled = true;
    try {
      const flight = flightFormData(form);
      const isEditing = Boolean(editingFlightNumber);
      const url = isEditing
        ? `/api/admin/flights/${encodeURIComponent(editingFlightNumber)}`
        : "/api/admin/flights";
      await api(url, {
        method: isEditing ? "PUT" : "POST",
        body: JSON.stringify(flight)
      });
      notify(isEditing ? "Flight details updated." : "Flight added to the schedule.");
      resetFlightForm();
      await loadAdminDashboard();
    } catch (error) {
      notify(error.message, "error");
    } finally {
      button.disabled = false;
    }
  });

  document.getElementById("adminFlights").addEventListener("click", async (event) => {
    const button = event.target.closest("[data-flight-action]");
    if (!button) return;
    const flight = flightRecords.find((item) => item.number === button.dataset.flight);
    if (!flight) return;
    if (button.dataset.flightAction === "edit") {
      fillFlightForm(flight);
      return;
    }
    if (!window.confirm(`Delete flight ${flight.number}?`)) return;
    button.disabled = true;
    try {
      await api(`/api/admin/flights/${encodeURIComponent(flight.number)}`, { method: "DELETE" });
      notify("Flight removed from the schedule.");
      await loadAdminDashboard();
    } catch (error) {
      notify(error.message, "error");
      button.disabled = false;
    }
  });
}
