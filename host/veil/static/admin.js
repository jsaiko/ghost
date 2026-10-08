// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The admin UI's only script besides htmx, kept out of the pages so the
// Content-Security-Policy can forbid inline script.
document.addEventListener("submit", (e) => {
  const message = e.target.dataset && e.target.dataset.confirm;
  if (message && !confirm(message)) {
    e.preventDefault();
  }
});
document.addEventListener("click", (e) => {
  const id = e.target.dataset && e.target.dataset.copy;
  if (!id) return;
  const text = document.getElementById(id).textContent;
  navigator.clipboard.writeText(text).then(() => {
    e.target.textContent = "Copied";
    setTimeout(() => { e.target.textContent = "Copy"; }, 1500);
  });
});

// Live updates: the server nudges over /admin/events when what a page
// shows has changed, and the page re-fetches itself and swaps <main>.
// Not while someone is typing in it or has changed a field, so a form
// is never rewritten under them; the swap waits for the next nudge.
(() => {
  let socket;
  let delay = 1000;
  let stale = false;

  const editing = () => {
    const a = document.activeElement;
    if (a && a.closest("main") && /^(INPUT|TEXTAREA|SELECT)$/.test(a.tagName)) return true;
    return [...document.querySelectorAll("main input, main textarea, main select")].some((f) => {
      if (f.type === "hidden") return false;
      if (f.type === "checkbox" || f.type === "radio") return f.checked !== f.defaultChecked;
      if (f.tagName === "SELECT") return [...f.options].some((o) => o.selected !== o.defaultSelected);
      return f.value !== f.defaultValue;
    });
  };

  const refresh = async () => {
    if (document.hidden || editing()) {
      stale = true;
      return;
    }
    stale = false;
    try {
      const r = await fetch(location.href, { headers: { Accept: "text/html" }, credentials: "same-origin" });
      if (!r.ok || r.redirected) return;
      const doc = new DOMParser().parseFromString(await r.text(), "text/html");
      const next = doc.querySelector("main");
      const main = document.querySelector("main");
      if (next && main && next.innerHTML !== main.innerHTML) main.innerHTML = next.innerHTML;
    } catch (_) {
      // Offline for a moment; the next nudge tries again.
    }
  };

  const connect = () => {
    socket = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/admin/events`);
    socket.onopen = () => {
      delay = 1000;
      if (stale) refresh();
    };
    socket.onmessage = refresh;
    socket.onclose = () => {
      setTimeout(connect, delay);
      delay = Math.min(delay * 2, 30000);
    };
  };

  if (document.querySelector("main") && !location.pathname.endsWith("/login")) connect();
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden && stale) refresh();
  });
  document.addEventListener("focusout", () => {
    if (stale) setTimeout(refresh, 0);
  });
})();
