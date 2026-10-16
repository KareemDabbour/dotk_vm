'use strict';

const canvas = document.querySelector('#maze');
const context = canvas.getContext('2d');
const elements = Object.fromEntries(['connection', 'level', 'score', 'best', 'lives', 'pellets', 'overlay', 'overlay-status', 'play', 'play-label', 'pause', 'restart', 'sound', 'game-status', 'power', 'session', 'footer-status'].map(name => [name, document.getElementById(name)]));
let state = null;
let previousState = null;
let updatedAt = performance.now();
let online = false;
let audio = null;
let pendingAction = null;
let actionBusy = false;
let generation = 0;
let hiddenPause = false;
const cell = 32;
const ghostColors = ['#ff6972', '#74e5cd', '#f19dd0'];
const directions = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right', w: 'up', s: 'down', a: 'left', d: 'right' };

function tone(frequency, duration, shape = 'sine') {
  if (!elements.sound.checked) return;
  audio ??= new AudioContext();
  if (audio.state === 'suspended') audio.resume();
  const oscillator = audio.createOscillator();
  const gain = audio.createGain();
  oscillator.type = shape;
  oscillator.frequency.value = frequency;
  gain.gain.setValueAtTime(0.045, audio.currentTime);
  gain.gain.exponentialRampToValueAtTime(0.001, audio.currentTime + duration);
  oscillator.connect(gain).connect(audio.destination);
  oscillator.start();
  oscillator.stop(audio.currentTime + duration);
}

function connection(connected) {
  online = connected;
  elements.connection.textContent = connected ? 'Connected' : 'Reconnecting';
  elements.connection.classList.toggle('offline', !connected);
  elements.play.disabled = !connected;
  elements.restart.disabled = !connected;
  elements.pause.disabled = !connected || !state || !['playing', 'paused'].includes(state.status);
  for (const button of document.querySelectorAll('[data-direction]')) button.disabled = !connected;
}

function update(next) {
  if (state && next.lives < state.lives) tone(110, 0.35, 'sawtooth');
  else if (state && next.score > state.score) tone(next.power > state.power ? 660 : 330, 0.055, 'square');
  previousState = state;
  state = next;
  updatedAt = performance.now();
  elements.score.textContent = String(state.score).padStart(6, '0');
  elements.best.textContent = String(state.best).padStart(6, '0');
  elements.lives.textContent = String(state.lives);
  elements.pellets.textContent = String(state.pellets);
  elements.level.textContent = String(state.level).padStart(2, '0');
  const labels = { ready: 'READY', playing: 'IN PLAY', paused: 'PAUSED', won: 'MAZE CLEARED', gameOver: 'GAME OVER' };
  elements['game-status'].textContent = labels[state.status];
  elements['footer-status'].textContent = labels[state.status];
  elements.power.textContent = state.power ? `POWER ${Math.ceil(state.power * 0.13)}s` : '';
  elements.session.textContent = `${state.pellets} REMAINING`;
  elements.overlay.hidden = state.status === 'playing';
  elements['overlay-status'].textContent = labels[state.status];
  elements['play-label'].textContent = { ready: 'Play', paused: 'Resume', won: 'Next maze', gameOver: 'Play again' }[state.status] || 'Play';
  elements.pause.innerHTML = `<i data-lucide="${state.status === 'paused' ? 'play' : 'pause'}"></i>`;
  lucide.createIcons();
  connection(true);
}

async function request(path, options = {}) {
  const response = await fetch(path, { cache: 'no-store', ...options });
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  return response.json();
}

async function poll() {
  const requestedGeneration = generation;
  try {
    if (!actionBusy) {
      const next = await request('/api/state');
      if (!actionBusy && generation === requestedGeneration) update(next);
    }
  }
  catch { connection(false); }
  setTimeout(poll, online ? 110 : 1000);
}

async function action(command) {
  generation++;
  pendingAction = command;
  if (actionBusy) return;
  actionBusy = true;
  try {
    while (pendingAction) {
      const next = pendingAction;
      pendingAction = null;
      update(await request('/api/action', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ action: next }) }));
    }
  } catch { pendingAction = null; connection(false); }
  finally { actionBusy = false; }
}

function position(index, previous, progress) {
  let column = index % state.width;
  let row = Math.floor(index / state.width);
  if (previous != null && Math.abs(index % state.width - previous % state.width) + Math.abs(Math.floor(index / state.width) - Math.floor(previous / state.width)) === 1) {
    column = previous % state.width + (column - previous % state.width) * progress;
    row = Math.floor(previous / state.width) + (row - Math.floor(previous / state.width)) * progress;
  }
  return [(column + 0.5) * cell, (row + 0.5) * cell];
}

function render(now) {
  context.fillStyle = '#0c0d12';
  context.fillRect(0, 0, canvas.width, canvas.height);
  if (state) {
    const progress = Math.min(1, (now - updatedAt) / 100);
    for (let index = 0; index < state.board.length; index++) {
      const column = index % state.width;
      const row = Math.floor(index / state.width);
      const tile = state.board[index];
      if (tile === '#') {
        context.fillStyle = '#101b35';
        context.strokeStyle = '#507cf5';
        context.lineWidth = 2;
        context.beginPath();
        context.roundRect(column * cell + 3, row * cell + 3, cell - 6, cell - 6, 5);
        context.fill(); context.stroke();
      } else if (tile === '.' || tile === 'o') {
        context.fillStyle = tile === 'o' ? '#ffe469' : '#e7dbc0';
        context.beginPath();
        const radius = tile === 'o' ? 6 + Math.sin(now / 160) : 2.1;
        context.arc((column + 0.5) * cell, (row + 0.5) * cell, radius, 0, Math.PI * 2);
        context.fill();
      }
    }
    state.ghosts.forEach((ghost, index) => {
      const [horizontal, vertical] = position(ghost.position, previousState?.ghosts[index]?.position, progress);
      context.globalAlpha = ghost.cooldown ? 0.3 : 1;
      context.fillStyle = state.power ? '#91aaff' : ghostColors[index];
      context.beginPath(); context.arc(horizontal, vertical - 1, 12, Math.PI, 0);
      context.lineTo(horizontal + 12, vertical + 12);
      for (let tooth = 3; tooth >= 0; tooth--) context.lineTo(horizontal - 12 + tooth * 8, vertical + (tooth % 2 ? 9 : 13));
      context.closePath(); context.fill();
      context.fillStyle = '#fff';
      for (const offset of [-5, 5]) { context.beginPath(); context.ellipse(horizontal + offset, vertical - 2, 3.5, 4.5, 0, 0, Math.PI * 2); context.fill(); }
      context.fillStyle = '#202332';
      for (const offset of [-4, 6]) { context.beginPath(); context.arc(horizontal + offset, vertical - 1, 1.7, 0, Math.PI * 2); context.fill(); }
      context.globalAlpha = 1;
    });
    const [horizontal, vertical] = position(state.player, previousState?.player, progress);
    const angle = state.headingY === -1 ? -Math.PI / 2 : state.headingY === 1 ? Math.PI / 2 : state.headingX === -1 ? Math.PI : 0;
    const mouth = state.status === 'playing' ? 0.16 + Math.abs(Math.sin(now / 75)) * 0.48 : 0.28;
    context.globalAlpha = state.invulnerable && Math.floor(now / 100) % 2 ? 0.45 : 1;
    context.fillStyle = '#ffe257'; context.beginPath(); context.moveTo(horizontal, vertical);
    context.arc(horizontal, vertical, 12.5, angle + mouth, angle + Math.PI * 2 - mouth); context.closePath(); context.fill(); context.globalAlpha = 1;
  }
  requestAnimationFrame(render);
}

elements.play.addEventListener('click', async () => {
  if (state?.status === 'gameOver') { await action('restart'); await action('start'); }
  else action(state?.status === 'won' ? 'next' : 'start');
  canvas.focus({ preventScroll: true });
});
elements.pause.addEventListener('click', () => action('pause'));
elements.restart.addEventListener('click', () => action('restart'));
elements.sound.addEventListener('change', () => { if (elements.sound.checked) tone(440, 0.08); });
for (const button of document.querySelectorAll('[data-direction]')) button.addEventListener('pointerdown', event => { event.preventDefault(); action(button.dataset.direction); });
document.addEventListener('keydown', event => {
  if (event.target instanceof HTMLInputElement || event.metaKey || event.ctrlKey || event.altKey) return;
  const key = event.key.length === 1 ? event.key.toLowerCase() : event.key;
  if (directions[key]) { event.preventDefault(); action(directions[key]); }
  else if (key === ' ' || key === 'p' || key === 'Escape') { event.preventDefault(); if (!event.repeat) action(state?.status === 'ready' ? 'start' : 'pause'); }
  else if (key === 'r' && !event.repeat) action('restart');
});
let touch = null;
canvas.addEventListener('pointerdown', event => { touch = [event.clientX, event.clientY]; canvas.setPointerCapture(event.pointerId); });
canvas.addEventListener('pointerup', event => {
  if (!touch) return;
  const horizontal = event.clientX - touch[0];
  const vertical = event.clientY - touch[1];
  if (Math.max(Math.abs(horizontal), Math.abs(vertical)) > 15) action(Math.abs(horizontal) > Math.abs(vertical) ? horizontal > 0 ? 'right' : 'left' : vertical > 0 ? 'down' : 'up');
  touch = null;
});
document.addEventListener('visibilitychange', () => {
  if (document.hidden && state?.status === 'playing') { hiddenPause = true; action('pause'); }
  else if (!document.hidden && hiddenPause) { hiddenPause = false; }
});
connection(false);
lucide.createIcons();
poll();
requestAnimationFrame(render);