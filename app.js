/* NIT Silchar Gymkhana voting - frontend logic (talks to the C backend) */
"use strict";

const $ = (id) => document.getElementById(id);
let voterToken = null;
let adminToken = null;
let pendingScholarId = "";

/* ---------- helpers ---------- */
async function api(path, data) {
  const opts = data
    ? { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" }, body: new URLSearchParams(data) }
    : {};
  try {
    const res = await fetch(path, opts);
    return await res.json();
  } catch (e) {
    return { ok: false, error: "Cannot reach the server. Is server.exe running?" };
  }
}

let toastTimer;
function toast(msg, ok) {
  const t = $("toast");
  t.textContent = msg;
  t.className = "toast " + (ok ? "ok" : "err");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.add("hidden"), 4500);
}

function esc(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

function show(id, on) { $(id).classList.toggle("hidden", !on); }

function setStatus(status) {
  const b = $("statusBadge");
  const label = { NOT_STARTED: "Not started", ACTIVE: "Voting open", CLOSED: "Election closed" }[status] || status;
  b.textContent = label;
  b.className = "badge " + (status === "ACTIVE" ? "active" : status === "CLOSED" ? "closed" : "");
}

/* ---------- tabs ---------- */
document.querySelectorAll(".tab").forEach((btn) => {
  btn.addEventListener("click", () => {
    document.querySelectorAll(".tab").forEach((b) => b.classList.toggle("active", b === btn));
    ["student", "results", "admin"].forEach((t) => show("tab-" + t, t === btn.dataset.tab));
    if (btn.dataset.tab === "results") loadResults();
    if (btn.dataset.tab === "admin" && adminToken) loadTally();
  });
});

async function refreshStatus() {
  const r = await api("/api/candidates");
  if (r.ok) setStatus(r.status);
  return r;
}

/* ================= STUDENT ================= */
$("btnSendOtp").addEventListener("click", async () => {
  const first = $("firstName").value.trim();
  const second = $("secondName").value.trim();
  const sid = $("scholarId").value.trim();
  const email = $("email").value.trim();
  if (!first || !second || !sid || !email) return toast("Please fill all fields.", false);

  const r = await api("/api/login/request", { first, second, scholar_id: sid, email });
  if (!r.ok) return toast(r.error, false);

  pendingScholarId = sid;
  $("otpInfo").textContent = r.message + ". Valid for 5 minutes.";
  $("demoOtp").textContent = r.demo_otp;
  show("demoOtpBox", true);
  show("stepDetails", false);
  show("stepOtp", true);
  $("otpInput").focus();
});

$("btnBack").addEventListener("click", () => { show("stepOtp", false); show("stepDetails", true); });

$("btnVerify").addEventListener("click", async () => {
  const otp = $("otpInput").value.trim();
  if (otp.length !== 6) return toast("Enter the 6-digit OTP.", false);
  const r = await api("/api/login/verify", { scholar_id: pendingScholarId, otp });
  if (!r.ok) return toast(r.error, false);

  voterToken = r.token;
  $("welcomeName").textContent = "Welcome, " + r.name;
  $("welcomeMeta").textContent = "Scholar ID " + r.scholar_id + " · " + r.department.toUpperCase();
  $("otpInput").value = "";
  show("stepOtp", false);
  show("stepBallot", true);
  toast("Login successful.", true);
  loadBallot();
});

$("btnLogout").addEventListener("click", async () => {
  await api("/api/logout", { token: voterToken });
  voterToken = null;
  show("stepBallot", false);
  show("stepDetails", true);
  ["firstName", "secondName", "scholarId", "email"].forEach((i) => ($(i).value = ""));
});

async function loadBallot() {
  const [cands, me] = await Promise.all([api("/api/candidates"), api("/api/me?token=" + voterToken)]);
  if (!me.ok) { toast(me.error, false); return; }
  setStatus(me.status);

  const notice = $("ballotNotice");
  if (me.status === "NOT_STARTED") { notice.textContent = "Voting has not started yet. Please check back later."; show("ballotNotice", true); }
  else if (me.status === "CLOSED") { notice.textContent = "The election is closed. See the Results tab."; show("ballotNotice", true); }
  else show("ballotNotice", false);

  const canVote = me.status === "ACTIVE";
  const root = $("ballot");
  root.innerHTML = "";

  cands.posts.forEach((post) => {
    const voted = me.voted[post.code];
    const card = document.createElement("div");
    card.className = "card";
    let html = `<div class="post-title"><h2>${esc(post.name)}</h2>${voted ? '<span class="voted-tag">✓ Voted</span>' : ""}</div>`;
    if (post.candidates.length === 0) {
      html += '<p class="hint">No candidates for this post.</p>';
    } else {
      post.candidates.forEach((c) => {
        const dis = voted || !canVote;
        html += `<label class="cand ${dis ? "disabled" : ""}">
          <input type="radio" name="post_${post.code}" value="${c.id}" ${dis ? "disabled" : ""}>
          <span><span class="name">${esc(c.name)}</span><br><span class="man">${esc(c.manifesto)}</span></span>
        </label>`;
      });
      if (!voted && canVote) html += `<button class="primary" data-post="${post.code}">Vote for ${post.code}</button>`;
    }
    card.innerHTML = html;
    root.appendChild(card);
  });

  root.querySelectorAll("button[data-post]").forEach((btn) => {
    btn.addEventListener("click", async () => {
      const code = btn.dataset.post;
      const picked = root.querySelector(`input[name="post_${code}"]:checked`);
      if (!picked) return toast("Select a candidate first.", false);
      const name = picked.closest("label").querySelector(".name").textContent;
      if (!confirm(`Confirm your vote for ${name} (${code})? This cannot be changed.`)) return;
      const r = await api("/api/vote", { token: voterToken, post: code, candidate_id: picked.value });
      toast(r.message || r.error, r.ok);
      loadBallot();
    });
  });
}

/* ================= RESULTS ================= */
function renderResults(r, targetId) {
  const root = $(targetId);
  if (!r.ok) { root.innerHTML = `<p class="hint">${esc(r.error)}</p>`; return; }
  root.innerHTML = "";
  r.posts.forEach((p) => {
    const div = document.createElement("div");
    div.className = "result-post";
    let html = `<h3>${esc(p.name)}</h3>`;
    if (p.candidates.length === 0) html += '<p class="hint">No candidates.</p>';
    p.candidates.forEach((c) => {
      const pct = p.total ? Math.round((c.votes / p.total) * 100) : 0;
      html += `<div class="bar-row ${c.winner ? "winner" : ""}">
        <div class="bar-label"><span>${esc(c.name)} ${c.winner ? '<span class="winner-tag">🏆 Winner</span>' : ""}</span><span>${c.votes} votes (${pct}%)</span></div>
        <div class="bar"><div style="width:${pct}%"></div></div></div>`;
    });
    if (p.tie) html += '<p class="tie-tag">Tie: re-election needed.</p>';
    div.innerHTML = html;
    root.appendChild(div);
  });
}

async function loadResults() {
  await refreshStatus();
  renderResults(await api("/api/results"), "resultsBody");
}
$("btnRefreshResults").addEventListener("click", loadResults);

/* ================= ADMIN ================= */
$("btnAdminLogin").addEventListener("click", async () => {
  const r = await api("/api/admin/login", { password: $("adminPw").value });
  if (!r.ok) return toast(r.error, false);
  adminToken = r.token;
  $("adminPw").value = "";
  show("adminLogin", false);
  show("adminPanel", true);
  loadTally();
});

$("btnAdminLogout").addEventListener("click", () => {
  adminToken = null;
  show("adminPanel", false);
  show("adminLogin", true);
});

$("btnAddCand").addEventListener("click", async () => {
  const r = await api("/api/admin/add_candidate", {
    token: adminToken, post: $("candPost").value,
    name: $("candName").value.trim(), manifesto: $("candManifesto").value.trim(),
  });
  toast(r.message || r.error, r.ok);
  if (r.ok) { $("candName").value = ""; $("candManifesto").value = ""; loadTally(); }
});

$("btnStart").addEventListener("click", async () => {
  const r = await api("/api/admin/start", { token: adminToken });
  toast(r.message || r.error, r.ok);
  refreshStatus(); loadTally();
});

$("btnStop").addEventListener("click", async () => {
  if (!confirm("Stop the election? Voting will close permanently.")) return;
  const r = await api("/api/admin/stop", { token: adminToken });
  toast(r.message || r.error, r.ok);
  refreshStatus(); loadTally();
});

async function loadTally() {
  if (!adminToken) return;
  renderResults(await api("/api/admin/tally?token=" + adminToken), "tallyBody");
}
$("btnRefreshTally").addEventListener("click", loadTally);

/* ---------- init ---------- */
refreshStatus();
setInterval(refreshStatus, 10000);
