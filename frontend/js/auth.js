import { api, setSession } from "./api.js";
import { notify } from "./utils.js";

const forms = {
  login: document.getElementById("loginForm"),
  register: document.getElementById("registerForm"),
  admin: document.getElementById("adminLoginForm")
};

const copy = {
  login: ["Good to have you back.", "Sign in to see your upcoming journeys."],
  register: ["A little closer to takeoff.", "Create your passenger account to start planning."],
  admin: ["Welcome back, team.", "Sign in to manage flights and reservations."]
};

function selectTab(tab) {
  const selected = forms[tab] ? tab : "login";
  Object.entries(forms).forEach(([name, form]) => {
    form.hidden = name !== selected;
  });
  document.querySelectorAll(".auth-tab").forEach((button) => {
    const active = button.dataset.authTab === selected;
    button.classList.toggle("active", active);
    button.setAttribute("aria-selected", String(active));
  });
  document.getElementById("authTitle").textContent = copy[selected][0];
  document.getElementById("authDescription").textContent = copy[selected][1];
}

function formValues(form) {
  return Object.fromEntries(new FormData(form).entries());
}

function busy(form, state) {
  const button = form.querySelector('button[type="submit"]');
  if (!button) return;
  button.disabled = state;
  button.dataset.originalText ||= button.textContent;
  button.textContent = state ? "Please wait…" : button.dataset.originalText;
}

export function selectAuthTab(tab) {
  selectTab(tab);
}

export function setupAuth(onAuthenticated) {
  document.querySelectorAll(".auth-tab").forEach((button) => {
    button.addEventListener("click", () => selectTab(button.dataset.authTab));
  });

  forms.login.addEventListener("submit", async (event) => {
    event.preventDefault();
    busy(forms.login, true);
    try {
      const result = await api("/api/auth/passenger/login", {
        method: "POST",
        body: JSON.stringify(formValues(forms.login))
      });
      setSession(result.token, { ...result.user, role: result.role });
      forms.login.reset();
      onAuthenticated(result.user, result.role);
    } catch (error) {
      notify(error.message, "error");
    } finally {
      busy(forms.login, false);
    }
  });

  forms.register.addEventListener("submit", async (event) => {
    event.preventDefault();
    busy(forms.register, true);
    try {
      const result = await api("/api/auth/register", {
        method: "POST",
        body: JSON.stringify(formValues(forms.register))
      });
      setSession(result.token, { ...result.user, role: result.role });
      forms.register.reset();
      notify("Your passenger account is ready.");
      onAuthenticated(result.user, result.role);
    } catch (error) {
      notify(error.message, "error");
    } finally {
      busy(forms.register, false);
    }
  });

  forms.admin.addEventListener("submit", async (event) => {
    event.preventDefault();
    busy(forms.admin, true);
    try {
      const result = await api("/api/auth/admin/login", {
        method: "POST",
        body: JSON.stringify(formValues(forms.admin))
      });
      setSession(result.token, { ...result.user, role: result.role });
      forms.admin.reset();
      onAuthenticated(result.user, result.role);
    } catch (error) {
      notify(error.message, "error");
    } finally {
      busy(forms.admin, false);
    }
  });

  selectTab("login");
}
