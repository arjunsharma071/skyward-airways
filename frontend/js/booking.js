import { api, getSession, setSession } from "./api.js";
import { escapeHtml, formatDate, formatMoney, notify } from "./utils.js";

let callbacks = null;
let selectedFlight = null;
let requestedPassengerCount = 1;
let passengerBookings = [];
let seatInventory = [];
let selectedSeats = [];
let seatMapRequestId = 0;
let appliedPromo = "";

const addOnPrices = {
  "extra-bag": 1500,
  priority: 750,
  lounge: 2500,
  meal: 550,
  insurance: 900
};

function selectedClass() {
  return document.querySelector('input[name="travelClass"]:checked')?.value || "E";
}

function selectedAddOns() {
  return [...document.querySelectorAll('input[name="addOns"]:checked')]
    .map((input) => input.value);
}

function renderFareBreakdown() {
  if (!selectedFlight) return;
  const count = Number(document.getElementById("bookingPassengerCount").value) || 0;
  const cabin = selectedClass();
  const perPassenger = cabin === "B"
    ? Number(selectedFlight.businessFare ?? Number(selectedFlight.baseFare) * 1.8)
    : Number(selectedFlight.economyFare ?? selectedFlight.baseFare);
  const baseFare = perPassenger * count;
  const seatCharges = seatInventory
    .filter((seat) => selectedSeats.includes(seat.number))
    .reduce((total, seat) => total + Number(seat.surcharge || 0), 0);
  const addOnCharges = selectedAddOns()
    .reduce((total, code) => total + addOnPrices[code] * count, 0);
  const discount = appliedPromo === "SKYWARD10" ? baseFare * 0.1 : 0;
  const total = baseFare + seatCharges + addOnCharges - discount;
  const rows = [
    ["Flight fare", baseFare],
    ["Seat selection", seatCharges],
    ["Selected add-ons", addOnCharges],
    ...(discount ? [["SKYWARD10 savings", -discount]] : [])
  ];
  document.getElementById("bookingFareBreakdown").innerHTML = rows
    .map(([label, amount]) => `<div><span>${escapeHtml(label)}</span><strong>${formatMoney(amount)}</strong></div>`)
    .join("");
  document.getElementById("bookingTotal").textContent = count ? formatMoney(total) : "Unavailable";
}

function renderSeatMap() {
  const container = document.getElementById("seatMap");
  const count = Number(document.getElementById("bookingPassengerCount").value) || 0;
  document.getElementById("seatSelectionCount").textContent =
    `${selectedSeats.length} of ${count} selected`;
  if (!seatInventory.length) {
    container.innerHTML = '<p class="seat-map-message">No seats are available in this cabin.</p>';
    renderFareBreakdown();
    return;
  }
  container.innerHTML = seatInventory.map((seat) => {
    const selected = selectedSeats.includes(seat.number);
    const classes = ["seat", selected ? "is-selected" : "", seat.occupied ? "is-occupied" : ""]
      .filter(Boolean).join(" ");
    const surcharge = Number(seat.surcharge || 0);
    const description = `${seat.number}, ${seat.type}${surcharge ? `, ${formatMoney(surcharge)} surcharge` : ""}${seat.occupied ? ", occupied" : selected ? ", selected" : ", available"}`;
    return `<button type="button" class="${classes}" data-seat-number="${escapeHtml(seat.number)}"
      aria-label="${escapeHtml(description)}" aria-pressed="${selected}" ${seat.occupied ? "disabled" : ""}>
      <span>${escapeHtml(seat.number)}</span>${surcharge ? `<small>+${formatMoney(surcharge)}</small>` : ""}
    </button>`;
  }).join("");
  renderFareBreakdown();
}

async function loadSeatMap() {
  if (!selectedFlight) return;
  const requestId = ++seatMapRequestId;
  const cabin = selectedClass();
  const count = Number(document.getElementById("bookingPassengerCount").value) || 0;
  const container = document.getElementById("seatMap");
  container.innerHTML = '<p class="seat-map-message">Loading live seat availability…</p>';
  try {
    const response = await api(`/api/flights/${encodeURIComponent(selectedFlight.number)}/seats?travelClass=${cabin}`);
    if (requestId !== seatMapRequestId) return;
    seatInventory = response.seats || [];
    selectedSeats = selectedSeats.filter((number) =>
      seatInventory.some((seat) => seat.number === number && !seat.occupied)
    ).slice(0, count);
    for (const seat of seatInventory) {
      if (selectedSeats.length >= count) break;
      if (!seat.occupied && !selectedSeats.includes(seat.number)) selectedSeats.push(seat.number);
    }
    renderSeatMap();
  } catch (error) {
    if (requestId !== seatMapRequestId) return;
    seatInventory = [];
    selectedSeats = [];
    container.innerHTML = `<p class="seat-map-message">${escapeHtml(error.message)}</p>`;
    renderFareBreakdown();
  }
}

function refreshFareOptions() {
  if (!selectedFlight) return;
  const cabin = selectedClass();
  const countSelect = document.getElementById("bookingPassengerCount");
  const previousCount = Number(countSelect.value) || requestedPassengerCount;
  const available = cabin === "B"
    ? Number(selectedFlight.availableBusiness ?? selectedFlight.businessSeats)
    : Number(selectedFlight.availableEconomy ?? selectedFlight.economySeats);
  const capacity = Math.min(9, Math.max(0, available));
  countSelect.replaceChildren();
  for (let count = 1; count <= capacity; count += 1) {
    countSelect.add(new Option(`${count} ${count === 1 ? "passenger" : "passengers"}`, String(count)));
  }
  countSelect.disabled = capacity === 0;
  if (capacity > 0) {
    countSelect.value = String(Math.min(previousCount, capacity));
    requestedPassengerCount = Number(countSelect.value);
  } else {
    selectedSeats = [];
  }

  const economyPrice = Number(selectedFlight.economyFare ?? selectedFlight.baseFare);
  const businessPrice = Number(selectedFlight.businessFare ?? Number(selectedFlight.baseFare) * 1.8);
  document.getElementById("economyPrice").textContent = formatMoney(economyPrice);
  document.getElementById("businessPrice").textContent = formatMoney(businessPrice);
  document.querySelector('#bookingForm button[type="submit"]').disabled = capacity === 0;
  loadSeatMap();
}

function renderFlightSummary(flight) {
  document.getElementById("bookingFlightSummary").innerHTML = `
    <div class="summary-airline">
      <span class="airline-icon">✈</span>
      <div><strong>${escapeHtml(flight.airline)}</strong><small>${escapeHtml(flight.number)}</small></div>
    </div>
    <div class="summary-route">
      <div class="route-point"><strong>${escapeHtml(flight.departureTime)}</strong><small>${escapeHtml(flight.source)}</small></div>
      <div class="route-line"><span></span><b>✈</b><span></span></div>
      <div class="route-point"><strong>${escapeHtml(flight.arrivalTime)}</strong><small>${escapeHtml(flight.destination)}</small></div>
    </div>
    <div class="summary-date"><span>Departure date</span><strong>${escapeHtml(formatDate(flight.date))}</strong></div>`;
}

function renderPassengerSummary(user) {
  document.getElementById("bookingPassenger").innerHTML = `
    <span class="avatar">${escapeHtml((user.name || "P").slice(0, 1).toUpperCase())}</span>
    <div><strong>${escapeHtml(user.name)}</strong><small>${escapeHtml(user.id)} · ${escapeHtml(user.contact || "")}</small></div>`;
}

export function requestFlightBooking(flight, travelClass = "E", passengerCount = 1) {
  const session = getSession();
  if (!session) {
    sessionStorage.setItem("skywardPendingBooking", JSON.stringify({
      flight,
      travelClass,
      passengerCount
    }));
    notify("Sign in or create an account to book this flight.");
    callbacks.navigate("auth");
    callbacks.selectAuthTab("login");
    return;
  }
  if (session.user.role !== "passenger") {
    notify("Passenger accounts are required to make a booking.", "error");
    return;
  }
  selectedFlight = flight;
  requestedPassengerCount = Math.max(1, Math.min(9, Number(passengerCount) || 1));
  seatInventory = [];
  selectedSeats = [];
  appliedPromo = "";
  document.getElementById("promoCode").value = "";
  document.getElementById("promoFeedback").textContent = "";
  document.querySelectorAll('input[name="addOns"]').forEach((input) => { input.checked = false; });
  document.querySelectorAll('input[name="travelClass"]').forEach((input) => {
    input.checked = input.value === (travelClass === "B" ? "B" : "E");
  });
  renderPassengerSummary(session.user);
  renderFlightSummary(flight);
  callbacks.navigate("booking");
  refreshFareOptions();
}

export function resumePendingBooking() {
  const pending = sessionStorage.getItem("skywardPendingBooking");
  if (!pending) return false;
  sessionStorage.removeItem("skywardPendingBooking");
  try {
    const selection = JSON.parse(pending);
    if (!selection.flight || !selection.flight.number) return false;
    requestFlightBooking(selection.flight, selection.travelClass, selection.passengerCount);
    return true;
  } catch {
    return false;
  }
}

function renderBookingRows(bookings) {
  const list = document.getElementById("passengerBookings");
  list.replaceChildren();
  document.getElementById("noBookings").hidden = bookings.length !== 0;
  if (bookings.length === 0) return;

  const ordered = [...bookings].sort((a, b) => b.flight.date.localeCompare(a.flight.date));
  for (const booking of ordered) {
    const row = document.createElement("article");
    row.className = "booking-row";
    row.innerHTML = `
      <div class="booking-row-main"><strong>PNR ${escapeHtml(booking.pnr)}</strong><small>${escapeHtml(formatDate(booking.flight.date))} · ${escapeHtml(booking.passengerCount)} passenger${booking.passengerCount === 1 ? "" : "s"}</small></div>
      <div class="booking-route"><strong>${escapeHtml(booking.flight.source)} <span aria-hidden="true">→</span> ${escapeHtml(booking.flight.destination)}</strong><small>${escapeHtml(booking.flight.number)} · ${escapeHtml(booking.flight.departureTime)}${booking.seatNumbers?.length ? ` · Seats ${escapeHtml(booking.seatNumbers.join(", "))}` : ""}</small></div>
      <div class="booking-price"><strong>${formatMoney(booking.farePaid)}</strong><small>Demo fare · ${escapeHtml(booking.travelClass)} · <span class="status-pill${booking.cancelled ? " cancelled" : ""}">${escapeHtml(booking.status)}</span></small></div>
      <div class="booking-actions"><button class="button button-outline" data-booking-action="ticket" data-pnr="${escapeHtml(booking.pnr)}" type="button">View ticket</button>${booking.cancelled ? "" : `<button class="button button-outline" data-booking-action="cancel" data-pnr="${escapeHtml(booking.pnr)}" type="button">Cancel</button>`}</div>`;
    list.append(row);
  }
}

function renderProfile(user) {
  const profile = document.getElementById("profilePanel");
  profile.innerHTML = `
    <div class="profile-overview">
      <div class="profile-details"><span class="avatar">${escapeHtml((user.name || "P").slice(0, 1).toUpperCase())}</span>
        <div><div class="eyebrow">Passenger profile</div><strong>${escapeHtml(user.name)}</strong>
        <small>${escapeHtml(user.id)} · ${escapeHtml(user.contact || "")}</small></div></div>
      <div class="loyalty-card"><span>SKYWARD REWARDS · ${escapeHtml(user.eliteTier || "Silver")}</span><strong>${escapeHtml(user.loyaltyMiles || 0)} <small>miles</small></strong><small>Based on confirmed bookings in this demo</small></div>
    </div>
    <form id="profileEditForm" class="profile-edit form-grid">
      <label>Full name<input name="name" required maxlength="100" value="${escapeHtml(user.name)}"></label>
      <label>Email or phone<input name="contact" required maxlength="120" value="${escapeHtml(user.contact)}"></label>
      <label>Preferred cabin<select name="preferredTravelClass"><option value="Economy" ${user.preferredTravelClass !== "Business" ? "selected" : ""}>Economy</option><option value="Business" ${user.preferredTravelClass === "Business" ? "selected" : ""}>Business</option></select></label>
      <label>Dietary preference<input name="dietaryPreference" maxlength="120" value="${escapeHtml(user.dietaryPreference || "")}" placeholder="Optional"></label>
      <label>Accessibility preference<input name="accessibilityPreference" maxlength="120" value="${escapeHtml(user.accessibilityPreference || "")}" placeholder="Optional"></label>
      <div class="profile-form-action"><button class="button button-outline" type="submit">Save profile</button></div>
    </form>`;
}

export async function loadPassengerDashboard(user) {
  const [sessionData, response] = await Promise.all([
    api("/api/auth/me"),
    api("/api/bookings/mine")
  ]);
  const profileUser = { ...sessionData.user, role: "passenger" };
  const currentSession = getSession();
  if (currentSession) setSession(currentSession.token, profileUser);
  renderProfile(profileUser);
  passengerBookings = response.bookings || [];
  renderBookingRows(passengerBookings);
  if (!sessionData.user) renderProfile(user);
}

async function renderTicket(booking) {
  const flight = booking.flight;
  let scheduleStatus = "Schedule status unavailable";
  try {
    const result = await api(`/api/flights/${encodeURIComponent(flight.number)}/status`);
    scheduleStatus = `${result.status} · schedule-based demo, not live airline data`;
  } catch (error) {
    notify(error.message, "error");
  }
  const seats = booking.seatNumbers?.length ? booking.seatNumbers.join(", ") : "Assigned at confirmation";
  const addOns = booking.addOns?.length ? booking.addOns.join(", ") : "None";
  document.getElementById("ticketContent").innerHTML = `
    <article class="ticket-card${booking.cancelled ? " cancelled-ticket" : ""}">
      <header class="ticket-banner">
        <div class="ticket-brand"><span class="brand-mark"><svg viewBox="0 0 42 42" aria-hidden="true"><path d="M7 25.5 35 9l-10 23-5-8-13 1.5Z"/><path d="m20 24 6 8 3-13"/></svg></span><span><strong>SKYWARD</strong><small> AIRWAYS</small></span></div>
        <span class="ticket-banner-label">ELECTRONIC TICKET</span>
      </header>
      <div class="ticket-content">
        <div class="ticket-status">
          <div><small>Booking reference / PNR</small><strong>${escapeHtml(booking.pnr)}</strong></div>
          <span class="status-pill${booking.cancelled ? " cancelled" : ""}">${escapeHtml(booking.status)}</span>
        </div>
        <div class="ticket-route">
          <div class="ticket-end"><small>FROM · ${escapeHtml(flight.departureTime)}</small><strong>${escapeHtml(flight.source)}</strong><span>${escapeHtml(formatDate(flight.date))}</span></div>
          <div class="ticket-flight-line">✈<span></span><small>${escapeHtml(flight.number)}</small></div>
          <div class="ticket-end"><small>TO · ${escapeHtml(flight.arrivalTime)}</small><strong>${escapeHtml(flight.destination)}</strong><span>${escapeHtml(flight.airline)}</span></div>
        </div>
        <div class="ticket-details">
          <div class="ticket-detail"><small>Passenger</small><strong>${escapeHtml(booking.passengerName)}</strong></div>
          <div class="ticket-detail"><small>Travel class</small><strong>${escapeHtml(booking.travelClass)}</strong></div>
          <div class="ticket-detail"><small>Seat${booking.seatNumbers?.length === 1 ? "" : "s"}</small><strong>${escapeHtml(seats)}</strong></div>
          <div class="ticket-detail"><small>Passengers</small><strong>${escapeHtml(booking.passengerCount)}</strong></div>
          <div class="ticket-detail"><small>Total fare</small><strong>${formatMoney(booking.farePaid)}</strong></div>
          ${booking.discountAmount ? `<div class="ticket-detail"><small>Promo savings</small><strong>${formatMoney(booking.discountAmount)}</strong></div>` : ""}
          ${booking.cancelled ? `<div class="ticket-detail"><small>Simulated refund</small><strong>${formatMoney(booking.refundAmount)}</strong></div>` : ""}
          <div class="ticket-detail"><small>Schedule status</small><strong>${escapeHtml(scheduleStatus)}</strong></div>
        </div>
        <div class="ticket-addons"><small>Add-ons</small><strong>${escapeHtml(addOns)}</strong></div>
        <div class="ticket-footer">${booking.cancelled ? "Demo reservation cancelled; its seats have been released. The displayed refund is simulated." : "Demonstration reservation only. No payment was collected. Please arrive at the airport with valid identification; gate and terminal details are not provided."}</div>
      </div>
    </article>`;
  callbacks.navigate("ticket");
}

async function refreshSelectedFlightAvailability() {
  if (!selectedFlight) return;
  const query = new URLSearchParams({
    source: selectedFlight.source,
    destination: selectedFlight.destination,
    date: selectedFlight.date,
    passengers: String(requestedPassengerCount)
  });
  try {
    const response = await api(`/api/flights?${query}`);
    const updated = (response.flights || []).find((flight) => flight.number === selectedFlight.number);
    if (updated) selectedFlight = updated;
    refreshFareOptions();
  } catch (error) {
    notify(error.message, "error");
  }
}

export function setupBooking(configuration) {
  callbacks = configuration;
  document.querySelectorAll('input[name="travelClass"]').forEach((input) => {
    input.addEventListener("change", () => {
      selectedSeats = [];
      refreshFareOptions();
    });
  });
  document.getElementById("bookingPassengerCount").addEventListener("change", () => {
    requestedPassengerCount = Number(document.getElementById("bookingPassengerCount").value) || 1;
    selectedSeats = selectedSeats.slice(0, requestedPassengerCount);
    loadSeatMap();
  });
  document.getElementById("seatMap").addEventListener("click", (event) => {
    const button = event.target.closest("[data-seat-number]");
    if (!button || button.disabled) return;
    const number = button.dataset.seatNumber;
    if (selectedSeats.includes(number)) {
      selectedSeats = selectedSeats.filter((seat) => seat !== number);
    } else if (selectedSeats.length < Number(document.getElementById("bookingPassengerCount").value)) {
      selectedSeats.push(number);
    } else {
      notify("Select one seat per passenger. Remove a seat to choose another.", "error");
      return;
    }
    renderSeatMap();
  });
  document.querySelectorAll('input[name="addOns"]').forEach((input) => {
    input.addEventListener("change", renderFareBreakdown);
  });
  document.getElementById("applyPromo").addEventListener("click", () => {
    const code = document.getElementById("promoCode").value.trim().toUpperCase();
    const feedback = document.getElementById("promoFeedback");
    if (!code) {
      appliedPromo = "";
      feedback.textContent = "Enter a code to apply.";
      feedback.className = "promo-feedback is-error";
    } else if (code === "SKYWARD10") {
      appliedPromo = code;
      feedback.textContent = "10% flight-fare saving applied.";
      feedback.className = "promo-feedback is-success";
    } else {
      appliedPromo = "";
      feedback.textContent = "This code is not valid.";
      feedback.className = "promo-feedback is-error";
    }
    renderFareBreakdown();
  });

  document.getElementById("bookingForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!selectedFlight) {
      notify("Choose a flight before booking.", "error");
      return;
    }
    const passengerCount = Number(document.getElementById("bookingPassengerCount").value);
    if (selectedSeats.length !== passengerCount) {
      notify("Select one available seat for each passenger.", "error");
      return;
    }
    const button = event.currentTarget.querySelector('button[type="submit"]');
    button.disabled = true;
    try {
      const result = await api("/api/bookings", {
        method: "POST",
        body: JSON.stringify({
          flightNumber: selectedFlight.number,
          travelClass: selectedClass(),
          passengerCount,
          seatNumbers: selectedSeats,
          addOns: selectedAddOns(),
          promoCode: appliedPromo
        })
      });
      notify(`Booking confirmed. Your PNR is ${result.booking.pnr}.`);
      await renderTicket(result.booking);
      callbacks.onBookingComplete();
    } catch (error) {
      notify(error.message, "error");
      await refreshSelectedFlightAvailability();
    } finally {
      if (document.getElementById("bookingView").contains(button)) {
        button.disabled = false;
        if (selectedFlight) renderFareBreakdown();
      }
    }
  });

  document.getElementById("profilePanel").addEventListener("submit", async (event) => {
    if (event.target.id !== "profileEditForm") return;
    event.preventDefault();
    const form = event.target;
    const button = form.querySelector('button[type="submit"]');
    button.disabled = true;
    try {
      const values = Object.fromEntries(new FormData(form).entries());
      const result = await api("/api/passengers/me", {
        method: "PUT",
        body: JSON.stringify(values)
      });
      const currentSession = getSession();
      if (currentSession) setSession(currentSession.token, { ...result.user, role: "passenger" });
      renderProfile(result.user);
      notify("Your passenger profile and preferences have been saved.");
    } catch (error) {
      notify(error.message, "error");
      button.disabled = false;
    }
  });

  document.getElementById("passengerBookings").addEventListener("click", async (event) => {
    const button = event.target.closest("[data-booking-action]");
    if (!button) return;
    const booking = passengerBookings.find((item) => item.pnr === button.dataset.pnr);
    if (!booking) return;
    if (button.dataset.bookingAction === "ticket") {
      await renderTicket(booking);
      return;
    }
    if (button.dataset.bookingAction !== "cancel") return;
    if (!window.confirm(`Cancel booking ${booking.pnr}?`)) return;
    button.disabled = true;
    try {
      const result = await api(`/api/bookings/${encodeURIComponent(booking.pnr)}`, {
        method: "DELETE"
      });
      passengerBookings = passengerBookings.map((item) =>
        item.pnr === result.booking.pnr ? result.booking : item
      );
      renderBookingRows(passengerBookings);
      notify("Your booking has been cancelled and the seats released.");
    } catch (error) {
      notify(error.message, "error");
      button.disabled = false;
    }
  });

  document.getElementById("printTicket").addEventListener("click", () => window.print());
}
