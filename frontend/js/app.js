import { api, clearSession, getSession, setSession } from "./api.js";
import { setupAuth, selectAuthTab } from "./auth.js";
import {
  loadPassengerDashboard,
  requestFlightBooking,
  resumePendingBooking,
  setupBooking
} from "./booking.js";
import { loadAdminDashboard, setupAdmin } from "./admin.js";
import { escapeHtml, formatDate, formatMoney, notify } from "./utils.js";

const pageElements = {
  home: document.getElementById("homeView"),
  results: document.getElementById("resultsView"),
  auth: document.getElementById("authView"),
  booking: document.getElementById("bookingView"),
  dashboard: document.getElementById("dashboardView"),
  admin: document.getElementById("adminView"),
  ticket: document.getElementById("ticketView")
};
const flightsByNumber = new Map();
let activeView = "home";
let currentSearchFlights = [];
let currentSearchPassengerCount = 1;

function showView(name, shouldScroll = true) {
  const view = name === "destinations" ? "home" : name;
  if (!pageElements[view]) return;
  activeView = view;
  Object.entries(pageElements).forEach(([key, element]) => {
    const isActive = key === view;
    element.hidden = !isActive;
    element.classList.toggle("active", isActive);
  });
  document.querySelectorAll(".main-nav [data-view]").forEach((link) => {
    link.classList.toggle("active", link.dataset.view === view);
  });
  document.getElementById("mainNav").classList.remove("open");
  document.getElementById("mobileMenu").setAttribute("aria-expanded", "false");
  if (shouldScroll) window.scrollTo({ top: 0, behavior: "smooth" });
  if (name === "destinations") {
    window.setTimeout(() => document.getElementById("destinations").scrollIntoView({
      behavior: "smooth",
      block: "start"
    }), 50);
  }
}

function updateNavigation() {
  const session = getSession();
  const login = document.getElementById("navLogin");
  const register = document.getElementById("navRegister");
  const logout = document.getElementById("navLogout");
  const greeting = document.getElementById("userGreeting");
  const adminLink = document.querySelector(".admin-nav");
  const tripsLink = document.querySelector('.main-nav [data-view="dashboard"]');
  login.hidden = Boolean(session);
  register.hidden = Boolean(session);
  logout.hidden = !session;
  greeting.hidden = !session;
  adminLink.hidden = !session || session.user.role !== "admin";
  tripsLink.hidden = !session || session.user.role !== "passenger";
  greeting.textContent = session
    ? `Hello, ${session.user.name || session.user.id}`
    : "";
}

function renderFlightCard(flight, passengerCount = 1, cabinFilter = "any") {
  flightsByNumber.set(flight.number, flight);
  const economy = Number(flight.economyFare ?? flight.baseFare);
  const business = Number(flight.businessFare ?? Number(flight.baseFare) * 1.8);
  const availableEconomy = Number(flight.availableEconomy ?? flight.economySeats);
  const availableBusiness = Number(flight.availableBusiness ?? flight.businessSeats);
  const canBookEconomy = availableEconomy >= passengerCount;
  const canBookBusiness = availableBusiness >= passengerCount;
  const bookingClass = cabinFilter === "B" ? "B"
    : cabinFilter === "E" ? "E"
    : canBookEconomy ? "E" : "B";
  const enoughSeats = bookingClass === "E" ? canBookEconomy : canBookBusiness;
  return `
    <article class="flight-card">
      <div class="flight-card-top">
        <div class="airline-mark"><span class="airline-icon">✈</span><span class="airline-meta"><strong>${escapeHtml(flight.number)}</strong><small>${escapeHtml(flight.airline)}</small></span></div>
        <span class="date-pill">${escapeHtml(formatDate(flight.date))}</span>
      </div>
      <div class="flight-route">
        <div class="route-point"><strong>${escapeHtml(flight.departureTime)}</strong><small>${escapeHtml(flight.source)}</small></div>
        <div class="route-line"><span></span><b>✈</b><span></span></div>
        <div class="route-point"><strong>${escapeHtml(flight.arrivalTime)}</strong><small>${escapeHtml(flight.destination)}</small></div>
      </div>
      <div class="flight-fares">
        <div class="fare-option"><small>ECONOMY · FROM</small><strong>${formatMoney(economy)} <span>/ person</span></strong></div>
        <div class="fare-option"><small>BUSINESS · FROM</small><strong>${formatMoney(business)} <span>/ person</span></strong></div>
      </div>
      <div class="flight-card-bottom">
        <span class="seat-note"><b>${escapeHtml(availableEconomy)}</b> Economy · <b>${escapeHtml(availableBusiness)}</b> Business</span>
        <button class="button button-primary flight-book" type="button" data-book-flight="${escapeHtml(flight.number)}" data-class="${bookingClass}" data-count="${passengerCount}" ${enoughSeats ? "" : "disabled"}>${enoughSeats ? "Choose flight →" : "Sold out"}</button>
      </div>
    </article>`;
}

function renderFlightList(element, flights, passengerCount = 1, cabinFilter = "any") {
  if (!flights.length) {
    element.replaceChildren();
    return;
  }
  element.innerHTML = flights.map((flight) =>
    renderFlightCard(flight, passengerCount, cabinFilter)
  ).join("");
}

function updateAirlineFilter() {
  const select = document.getElementById("airlineFilter");
  const options = [...new Set(currentSearchFlights.map((flight) => flight.airline))]
    .sort((left, right) => left.localeCompare(right));
  select.replaceChildren(new Option("All airlines", ""));
  for (const airline of options) select.add(new Option(airline, airline));
}

function resetSearchFilters() {
  document.getElementById("airlineFilter").value = "";
  document.getElementById("cabinFilter").value = "any";
  document.getElementById("departureWindowFilter").value = "any";
  document.getElementById("minimumFareFilter").value = "";
  document.getElementById("maximumFareFilter").value = "";
  document.getElementById("flightSort").value = "recommended";
  sortSearchFlights();
}

function lowestAvailableFare(flight, cabin, passengerCount) {
  const fares = [];
  if (cabin !== "B" && flight.availableEconomy >= passengerCount) {
    fares.push(Number(flight.economyFare ?? flight.baseFare));
  }
  if (cabin !== "E" && flight.availableBusiness >= passengerCount) {
    fares.push(Number(flight.businessFare ?? Number(flight.baseFare) * 1.8));
  }
  return fares.length ? Math.min(...fares) : Number.POSITIVE_INFINITY;
}

function sortSearchFlights() {
  const sortBy = document.getElementById("flightSort").value;
  const airline = document.getElementById("airlineFilter").value;
  const cabin = document.getElementById("cabinFilter").value;
  const departureWindow = document.getElementById("departureWindowFilter").value;
  const minimumFare = Number(document.getElementById("minimumFareFilter").value);
  const maximumFare = Number(document.getElementById("maximumFareFilter").value);
  const hasMinimumFare = document.getElementById("minimumFareFilter").value !== "";
  const hasMaximumFare = document.getElementById("maximumFareFilter").value !== "";
  const flights = currentSearchFlights.filter((flight) => {
    if (airline && flight.airline !== airline) return false;
    if (cabin === "E" && flight.availableEconomy < currentSearchPassengerCount) return false;
    if (cabin === "B" && flight.availableBusiness < currentSearchPassengerCount) return false;

    if (departureWindow !== "any") {
      const hour = Number(flight.departureTime.slice(0, 2));
      const matchesWindow = departureWindow === "early" ? hour < 6
        : departureWindow === "morning" ? hour >= 6 && hour < 12
        : departureWindow === "afternoon" ? hour >= 12 && hour < 18
        : hour >= 18 && hour < 24;
      if (!matchesWindow) return false;
    }

    const fare = lowestAvailableFare(flight, cabin, currentSearchPassengerCount);
    if (!Number.isFinite(fare)) return false;
    if (hasMinimumFare && fare < minimumFare) return false;
    if (hasMaximumFare && fare > maximumFare) return false;
    return true;
  });
  if (sortBy === "fare") {
    flights.sort((a, b) =>
      lowestAvailableFare(a, cabin, currentSearchPassengerCount) -
      lowestAvailableFare(b, cabin, currentSearchPassengerCount)
    );
  } else if (sortBy === "departure") {
    flights.sort((a, b) => a.departureTime.localeCompare(b.departureTime));
  } else if (sortBy === "arrival") {
    flights.sort((a, b) => a.arrivalTime.localeCompare(b.arrivalTime));
  } else if (sortBy === "airline") {
    flights.sort((a, b) => a.airline.localeCompare(b.airline) ||
      a.departureTime.localeCompare(b.departureTime));
  } else {
    flights.sort((a, b) => a.date.localeCompare(b.date) ||
      a.departureTime.localeCompare(b.departureTime));
  }
  const count = document.getElementById("resultsCount");
  count.textContent = flights.length === currentSearchFlights.length
    ? `${flights.length} ${flights.length === 1 ? "flight" : "flights"} found`
    : `${flights.length} of ${currentSearchFlights.length} flights match`;
  const emptyState = document.getElementById("noResults");
  emptyState.hidden = flights.length !== 0;
  emptyState.querySelector("h2").textContent = currentSearchFlights.length
    ? "No flights match these filters"
    : "No flights on this route yet";
  emptyState.querySelector("p").textContent = currentSearchFlights.length
    ? "Adjust or clear your filters to see more scheduled flights."
    : "Try a different city or date. New journeys are added by our flight team.";
  renderFlightList(
    document.getElementById("searchResults"),
    flights,
    currentSearchPassengerCount,
    cabin
  );
}

function renderFareCalendar(fares) {
  const panel = document.getElementById("fareCalendar");
  panel.hidden = !fares.length;
  if (!fares.length) {
    panel.replaceChildren();
    return;
  }
  panel.innerHTML = `<div><div class="eyebrow">Plan around the fare</div><strong>Lowest fare by date</strong><small>Based on flights currently in this schedule</small></div>
    <div class="fare-date-list">${fares.map((item) => `
      <button type="button" class="fare-date" data-fare-date="${escapeHtml(item.date)}">
        <small>${escapeHtml(formatDate(item.date))}</small><strong>${formatMoney(item.economyFare)}</strong>
      </button>`).join("")}</div>`;
}

async function loadFeaturedFlights() {
  const container = document.getElementById("featuredFlights");
  try {
    const response = await api("/api/flights?passengers=1");
    const flights = (response.flights || []).sort((a, b) =>
      a.date.localeCompare(b.date) || a.departureTime.localeCompare(b.departureTime)
    );
    if (!flights.length) {
      container.innerHTML = '<div class="loading-card">Our next flights are being planned. Check back soon.</div>';
      return;
    }
    renderFlightList(container, flights.slice(0, 3));
    const dateInput = document.getElementById("dateInput");
    const today = new Date();
    const localToday = `${today.getFullYear()}-${String(today.getMonth() + 1).padStart(2, "0")}-${String(today.getDate()).padStart(2, "0")}`;
    dateInput.min = localToday;
    const firstFutureFlight = flights.find((flight) => flight.date >= localToday);
    if (dateInput.value < localToday && firstFutureFlight) dateInput.value = firstFutureFlight.date;
  } catch (error) {
    container.innerHTML = `<div class="loading-card">${escapeHtml(error.message)}</div>`;
  }
}

async function searchFlights(event) {
  event.preventDefault();
  const form = event.currentTarget;
  const source = form.elements.source.value.trim();
  const destination = form.elements.destination.value.trim();
  const date = form.elements.date.value;
  const passengerCount = Number(form.elements.passengers.value);
  if (source.toLowerCase() === destination.toLowerCase()) {
    notify("Choose different departure and arrival cities.", "error");
    return;
  }

  const query = new URLSearchParams({ source, destination, date, passengers: String(passengerCount) });
  const results = document.getElementById("searchResults");
  document.getElementById("noResults").hidden = true;
  results.innerHTML = '<div class="loading-card"><span class="loader"></span><span>Looking for the right flight…</span></div>';
  document.getElementById("resultsTitle").textContent = `${source} → ${destination}`;
  document.getElementById("resultsSubtitle").textContent = `${formatDate(date)} · ${passengerCount} passenger${passengerCount === 1 ? "" : "s"}`;
  showView("results");
  try {
    const response = await api(`/api/flights?${query.toString()}`);
    currentSearchFlights = response.flights || [];
    currentSearchPassengerCount = passengerCount;
    updateAirlineFilter();
    resetSearchFilters();
    try {
      const calendarResponse = await api(`/api/fares/calendar?${new URLSearchParams({ source, destination })}`);
      renderFareCalendar(calendarResponse.fares || []);
    } catch (calendarError) {
      renderFareCalendar([]);
      notify(calendarError.message, "error");
    }
  } catch (error) {
    results.replaceChildren();
    document.getElementById("fareCalendar").hidden = true;
    document.getElementById("noResults").hidden = false;
    notify(error.message, "error");
  }
}

function updateAuthFromServer(user, role) {
  const session = getSession();
  if (session) setSession(session.token, { ...user, role });
  updateNavigation();
}

async function openPassengerDashboard() {
  const session = getSession();
  if (!session) {
    showView("auth");
    selectAuthTab("login");
    notify("Sign in to see your trips.");
    return;
  }
  if (session.user.role !== "passenger") {
    notify("This area is for passenger accounts.", "error");
    return;
  }
  showView("dashboard");
  try {
    await loadPassengerDashboard(session.user);
  } catch (error) {
    notify(error.message, "error");
  }
}

async function openAdminDashboard() {
  const session = getSession();
  if (!session || session.user.role !== "admin") {
    showView("auth");
    selectAuthTab("admin");
    notify("Administrator sign-in is required.");
    return;
  }
  showView("admin");
  try {
    await loadAdminDashboard();
  } catch (error) {
    notify(error.message, "error");
  }
}

function handleAuthenticated(user, role) {
  updateAuthFromServer(user, role);
  notify(role === "admin" ? "Administrator access granted." : `Welcome, ${user.name}.`);
  if (role === "admin") {
    openAdminDashboard();
    return;
  }
  if (resumePendingBooking()) return;
  openPassengerDashboard();
}

function navigateTo(name) {
  if (name === "dashboard") {
    openPassengerDashboard();
  } else if (name === "admin") {
    openAdminDashboard();
  } else if (name === "auth") {
    showView("auth");
  } else {
    showView(name);
  }
}

document.getElementById("searchForm").addEventListener("submit", searchFlights);
document.getElementById("swapCities").addEventListener("click", () => {
  const source = document.getElementById("sourceInput");
  const destination = document.getElementById("destinationInput");
  [source.value, destination.value] = [destination.value, source.value];
});

document.getElementById("flightSort").addEventListener("change", sortSearchFlights);
for (const filterId of [
  "airlineFilter",
  "cabinFilter",
  "departureWindowFilter",
  "minimumFareFilter",
  "maximumFareFilter"
]) {
  document.getElementById(filterId).addEventListener("change", sortSearchFlights);
}
for (const filterId of ["minimumFareFilter", "maximumFareFilter"]) {
  document.getElementById(filterId).addEventListener("input", sortSearchFlights);
}
document.getElementById("resetFlightFilters").addEventListener("click", resetSearchFilters);
document.getElementById("fareCalendar").addEventListener("click", (event) => {
  const button = event.target.closest("[data-fare-date]");
  if (!button) return;
  document.getElementById("dateInput").value = button.dataset.fareDate;
  document.getElementById("searchForm").requestSubmit();
});

document.addEventListener("click", (event) => {
  const authTabButton = event.target.closest("[data-auth-tab]");
  if (authTabButton) {
    event.preventDefault();
    showView("auth");
    selectAuthTab(authTabButton.dataset.authTab);
    return;
  }
  const navigationButton = event.target.closest("[data-view]");
  if (navigationButton) {
    event.preventDefault();
    navigateTo(navigationButton.dataset.view);
    return;
  }
  const destinationButton = event.target.closest("[data-destination]");
  if (destinationButton) {
    document.getElementById("sourceInput").value = destinationButton.dataset.destinationSource;
    document.getElementById("destinationInput").value = destinationButton.dataset.destination;
    document.getElementById("dateInput").value ||= "2026-12-15";
    document.getElementById("searchForm").requestSubmit();
  }
});

for (const containerId of ["featuredFlights", "searchResults"]) {
  document.getElementById(containerId).addEventListener("click", (event) => {
    const button = event.target.closest("[data-book-flight]");
    if (!button || button.disabled) return;
    const flight = flightsByNumber.get(button.dataset.bookFlight);
    if (!flight) {
      notify("Flight details are no longer available. Search again.", "error");
      return;
    }
    requestFlightBooking(flight, button.dataset.class, Number(button.dataset.count));
  });
}

document.getElementById("mobileMenu").addEventListener("click", (event) => {
  const menu = document.getElementById("mainNav");
  const isOpen = menu.classList.toggle("open");
  event.currentTarget.setAttribute("aria-expanded", String(isOpen));
});

document.getElementById("navLogout").addEventListener("click", async () => {
  try {
    await api("/api/auth/logout", { method: "POST" });
  } catch {
    // A server restart may have already cleared the in-memory session.
  }
  clearSession();
  updateNavigation();
  showView("home");
  notify("You have signed out.");
});

setupAuth(handleAuthenticated);
setupBooking({
  navigate: showView,
  selectAuthTab,
  onBookingComplete: () => {
    const session = getSession();
    if (session?.user.role === "passenger") {
      loadPassengerDashboard(session.user).catch((error) => notify(error.message, "error"));
    }
  }
});
setupAdmin();

window.addEventListener("skyward-session-expired", () => {
  updateNavigation();
  if (activeView === "dashboard" || activeView === "admin" || activeView === "booking") {
    showView("auth");
    notify("Your session expired. Please sign in again.", "error");
  }
});

async function restoreSession() {
  const previous = getSession();
  if (!previous) return;
  try {
    const result = await api("/api/auth/me");
    setSession(previous.token, { ...result.user, role: result.role });
    updateNavigation();
  } catch (error) {
    updateNavigation();
    if (error.message.includes("Cannot reach")) {
      notify(error.message, "error");
    }
  }
}

updateNavigation();
loadFeaturedFlights();
restoreSession();
