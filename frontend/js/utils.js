export function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, (character) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#39;"
  })[character]);
}

export function formatMoney(value) {
  const amount = Number(value);
  if (!Number.isFinite(amount)) return "—";
  return new Intl.NumberFormat("en-IN", {
    style: "currency",
    currency: "INR",
    maximumFractionDigits: 0
  }).format(amount);
}

export function formatDate(value) {
  if (!value) return "Date to be confirmed";
  const date = new Date(`${value}T00:00:00`);
  if (Number.isNaN(date.getTime())) return value;
  return new Intl.DateTimeFormat("en-IN", {
    day: "numeric",
    month: "short",
    year: "numeric"
  }).format(date);
}

export function notify(message, type = "success") {
  const region = document.getElementById("toastRegion");
  if (!region) return;
  const toast = document.createElement("div");
  toast.className = `toast${type === "error" ? " error" : ""}`;
  const mark = document.createElement("span");
  mark.className = "toast-mark";
  mark.textContent = type === "error" ? "!" : "✓";
  const text = document.createElement("span");
  text.textContent = message;
  toast.append(mark, text);
  region.append(toast);
  window.setTimeout(() => toast.remove(), 4200);
}
