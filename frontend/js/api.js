const TOKEN_KEY = "skywardSessionToken";
const USER_KEY = "skywardSessionUser";

let token = localStorage.getItem(TOKEN_KEY) || "";
let user = null;
try {
  user = JSON.parse(localStorage.getItem(USER_KEY) || "null");
} catch {
  localStorage.removeItem(USER_KEY);
}

export function getSession() {
  return token && user ? { token, user } : null;
}

export function setSession(sessionToken, sessionUser) {
  token = sessionToken;
  user = sessionUser;
  localStorage.setItem(TOKEN_KEY, token);
  localStorage.setItem(USER_KEY, JSON.stringify(user));
}

export function clearSession() {
  token = "";
  user = null;
  localStorage.removeItem(TOKEN_KEY);
  localStorage.removeItem(USER_KEY);
}

export async function api(path, options = {}) {
  const headers = new Headers(options.headers || {});
  if (options.body && !headers.has("Content-Type")) {
    headers.set("Content-Type", "application/json");
  }
  if (token) headers.set("Authorization", `Bearer ${token}`);

  let response;
  try {
    response = await fetch(path, { ...options, headers });
  } catch {
    throw new Error("Cannot reach the reservation server. Make sure it is running on localhost:8080.");
  }

  let result;
  const responseText = await response.text();
  try {
    result = responseText ? JSON.parse(responseText) : {};
  } catch {
    result = { error: responseText || "The server returned an unexpected response." };
  }

  if (!response.ok) {
    if (response.status === 401 && token) {
      clearSession();
      window.dispatchEvent(new CustomEvent("skyward-session-expired"));
    }
    throw new Error(result.error || `Request failed (${response.status}).`);
  }
  return result;
}
