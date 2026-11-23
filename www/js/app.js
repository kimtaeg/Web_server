async function refreshStatus() {
  const el = document.getElementById('status');
  try {
    const res = await fetch('/__status', { cache: 'no-store' });
    el.textContent = JSON.stringify(await res.json(), null, 2);
  } catch (e) {
    el.textContent = '상태를 불러오지 못했습니다: ' + e;
  }
}

async function callApi() {
  const out = document.getElementById('api-out');
  out.innerHTML = '';
  for (let i = 0; i < 4; i++) {
    const li = document.createElement('li');
    try {
      const res = await fetch('/api/hello?i=' + i, { cache: 'no-store' });
      const text = await res.text();
      let who = res.status + ' ' + res.statusText;
      try {
        const j = JSON.parse(text);
        who = `${res.status} ← ${j.backend} (:${j.port}) saw ${j.target}`;
      } catch (_) { /* error page, not JSON */ }
      li.textContent = who;
    } catch (e) {
      li.textContent = 'error: ' + e;
    }
    out.appendChild(li);
  }
  refreshStatus();
}

document.getElementById('call-api').addEventListener('click', callApi);
refreshStatus();
