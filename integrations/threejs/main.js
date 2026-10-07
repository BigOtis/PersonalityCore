import * as THREE from 'three';

const $ = id => document.getElementById(id);
const scene = new THREE.Scene();
scene.background = new THREE.Color('#1c3630');
const camera = new THREE.PerspectiveCamera(40, 1, .1, 100);
camera.position.set(0, 1.8, 6);
camera.lookAt(0, 1, 0);
const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
$('viewport').append(renderer.domElement);
scene.add(new THREE.HemisphereLight(0xe6fff5, 0x385149, 3));
const key = new THREE.DirectionalLight(0xffffff, 3);
key.position.set(3, 5, 4); scene.add(key);
const actor = new THREE.Group(); actor.position.y = 1; scene.add(actor);
const material = new THREE.MeshStandardMaterial({ color: '#c5e5b2', roughness: .45 });
actor.add(new THREE.Mesh(new THREE.CapsuleGeometry(.35, .55, 8, 24), material));
for (const x of [-.13, .13]) {
  const eye = new THREE.Mesh(new THREE.SphereGeometry(.045, 12, 8), new THREE.MeshBasicMaterial({ color: '#14352c' }));
  eye.position.set(x, .25, .34); actor.add(eye);
}
const ground = new THREE.Mesh(new THREE.CircleGeometry(2, 48), new THREE.MeshStandardMaterial({ color: '#304e44' }));
ground.rotation.x = -Math.PI / 2; scene.add(ground);
new ResizeObserver(() => {
  const { clientWidth: w, clientHeight: h } = $('viewport');
  camera.aspect = w / h; camera.updateProjectionMatrix(); renderer.setSize(w, h);
}).observe($('viewport'));

let socket, session, audio, panner, nextAudio = 0;
let generating = false, acceptingAudio = false, connecting = false;
const sources = new Set();
function controls() {
  const connected = socket?.readyState === WebSocket.OPEN;
  $('send').disabled = !connected || generating || sources.size > 0;
  $('interrupt').disabled = !connected;
  $('connect').disabled = connecting || connected;
  $('character').disabled = connecting || connected;
}
function stopAudio() {
  for (const source of sources) { source.onended = null; source.stop(); source.disconnect(); }
  sources.clear(); nextAudio = audio?.currentTime || 0;
}
function playPCM(event) {
  if (!acceptingAudio) return;
  const bytes = Uint8Array.from(atob(event.pcm16_b64), ch => ch.charCodeAt(0));
  const samples = new DataView(bytes.buffer);
  const buffer = audio.createBuffer(1, bytes.length / 2, event.sample_rate);
  const channel = buffer.getChannelData(0);
  for (let i = 0; i < channel.length; i++) channel[i] = samples.getInt16(i * 2, true) / 32768;
  const source = audio.createBufferSource(); source.buffer = buffer; source.connect(panner);
  sources.add(source);
  source.onended = () => { sources.delete(source); source.disconnect(); controls(); };
  nextAudio = Math.max(nextAudio, audio.currentTime + .03);
  source.start(nextAudio); nextAudio += buffer.duration;
}
async function api(path, body, method = 'POST') {
  const response = await fetch(path, body === undefined ? undefined : { method, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  if (!response.ok) throw new Error(await response.text());
  return response.json();
}
function fail(error) { $('status').textContent = error.message; controls(); }
async function removeSession() {
  const id = session; session = undefined;
  if (id) await fetch(`/v1/sessions/${id}`, { method: 'DELETE' });
}
$('connect').onclick = async () => {
  if (connecting) return;
  connecting = true; controls();
  try {
    // Audio permission is acquired inside a user gesture.
    audio ||= new AudioContext(); await audio.resume();
    if (!panner) { panner = audio.createPanner(); panner.positionY.value = 1; panner.positionZ.value = -2; panner.connect(audio.destination); }
    await removeSession();
    const result = await api('/v1/sessions', { character_id: $('character').value, game_context: { location: 'browser studio', host: 'threejs', available_actions: [] } });
    session = result.id;
    socket = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/v1/sessions/${session}/live`);
    socket.onmessage = ({ data }) => {
      const event = JSON.parse(data);
      if (event.type === 'ready') { $('status').textContent = 'Ready'; connecting = false; }
      if (event.type === 'dialogue') $('dialogue').textContent = event.text;
      if (event.type === 'audio') playPCM(event);
      if (event.type === 'reply') { $('dialogue').textContent = event.reply.dialogue; $('proposals').textContent = JSON.stringify(event.reply.actions || [], null, 2); }
      if (event.type === 'state') { generating = event.state !== 'idle'; $('status').textContent = event.state; }
      if (event.type === 'cancelled') { acceptingAudio = false; generating = false; stopAudio(); }
      if (event.type === 'error') { generating = false; acceptingAudio = false; stopAudio(); fail(new Error(event.error)); }
      controls();
    };
    socket.onclose = () => { connecting = false; generating = false; acceptingAudio = false; stopAudio(); $('status').textContent = 'Disconnected. Reconnect to start a new session.'; controls(); };
    socket.onerror = () => fail(new Error('Cannot connect to PersonalityCore. Check the runtime URL.'));
  } catch (error) { connecting = false; fail(error); }
};
$('message').onsubmit = event => {
  event.preventDefault();
  const text = $('words').value.trim();
  if (!text || $('send').disabled) return;
  generating = true; acceptingAudio = true; $('dialogue').textContent = ''; $('proposals').textContent = '[]'; controls();
  socket.send(JSON.stringify({ type: 'set_context', game_context: { location: 'browser studio', host: 'threejs', available_actions: [] } }));
  socket.send(JSON.stringify({ type: 'text', text }));
  $('words').value = '';
};
$('interrupt').onclick = () => { acceptingAudio = false; generating = true; stopAudio(); controls(); socket.send(JSON.stringify({ type: 'interrupt' })); };
window.addEventListener('pagehide', () => { acceptingAudio = false; stopAudio(); socket?.close(); });
renderer.setAnimationLoop(time => {
  actor.position.y = 1 + (sources.size ? Math.sin(time / 70) * .025 : 0);
  renderer.render(scene, camera);
});
async function loadCharacters() { try {
  const characters = await api('/v1/characters');
  for (const character of characters) { const option = document.createElement('option'); option.value = character.id; option.textContent = character.name; $('character').append(option); }
  if (!characters.length) throw new Error('Create a character in the PersonalityCore studio first.');
  $('status').textContent = 'Choose a character and connect.';
} catch (error) { $('connect').disabled = true; fail(error); $('connect').disabled = true; } }
loadCharacters();
