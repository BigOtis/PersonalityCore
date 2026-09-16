export async function listDevices() {
  const devices = await navigator.mediaDevices.enumerateDevices();
  return { inputs: devices.filter(d => d.kind === "audioinput"), outputs: devices.filter(d => d.kind === "audiooutput") };
}
export function pcm16ToBase64(buffer: ArrayBuffer): string {
  let binary = "";
  for (const byte of new Uint8Array(buffer)) binary += String.fromCharCode(byte);
  return btoa(binary);
}
export function base64ToInt16(payload: string): Int16Array {
  return new Int16Array(Uint8Array.from(atob(payload), c => c.charCodeAt(0)).buffer);
}
export class PlaybackQueue {
  private context: AudioContext | null = null;
  private nextTime = 0;
  private sources = new Set<AudioBufferSourceNode>();
  private outputId = "";
  private generation = 0;
  private pending: Promise<void> = Promise.resolve();
  playing = false;
  onPlaying: (playing: boolean) => void = () => {};
  onFirstAudio: () => void = () => {};
  private idleWaiters: Array<() => void> = [];
  private notifyIdle() {
    if (this.playing || this.sources.size) return;
    const waiters = this.idleWaiters.splice(0);
    waiters.forEach(done => done());
  }
  async waitUntilIdle() {
    await this.pending.catch(() => {});
    if (!this.playing && !this.sources.size) return;
    await new Promise<void>(resolve => this.idleWaiters.push(resolve));
  }
  async unlock() {
    if (!this.context) this.context = new AudioContext();
    await this.context.resume();
  }
  async setOutput(deviceId = "") {
    this.outputId = deviceId;
    const ctx = this.context as (AudioContext & { setSinkId?: (id: string) => Promise<void> }) | null;
    if (ctx?.setSinkId) await ctx.setSinkId(deviceId);
  }
  enqueue(pcm16: Int16Array, sampleRate: number) {
    const generation = this.generation;
    const task = this.pending.catch(() => {}).then(async () => {
      if (generation !== this.generation || !pcm16.length) return;
      await this.unlock();
      await this.setOutput(this.outputId);
      if (generation !== this.generation) return;
      const ctx = this.context!;
      const audio = ctx.createBuffer(1, pcm16.length, sampleRate);
      const channel = audio.getChannelData(0);
      for (let i = 0; i < pcm16.length; i++) channel[i] = pcm16[i] / 32768;
      const source = ctx.createBufferSource();
      source.buffer = audio;
      source.connect(ctx.destination);
      this.sources.add(source);
      const startAt = Math.max(ctx.currentTime + 0.015, this.nextTime);
      source.start(startAt);
      this.nextTime = startAt + audio.duration;
      if (!this.playing) { this.onFirstAudio(); this.onPlaying(true); }
      this.playing = true;
      source.onended = () => {
        this.sources.delete(source);
        source.disconnect();
        if (!this.sources.size) { this.playing = false; this.onPlaying(false); this.notifyIdle(); }
      };
    });
    this.pending = task;
    return task;
  }
  stop() {
    this.generation++;
    for (const source of this.sources) { source.onended = null; source.stop(); source.disconnect(); }
    this.sources.clear(); this.nextTime = 0; this.playing = false; this.onPlaying(false); this.notifyIdle();
  }
  dispose() { this.stop(); void this.context?.close(); this.context = null; }
}
export class MicCapture {
  private stream: MediaStream | null = null;
  private context: AudioContext | null = null;
  private processor: AudioWorkletNode | null = null;
  private chunks: Int16Array[] = [];
  private rms = 0;
  private generation = 0;
  recording = false;
  async start(deviceId?: string) {
    this.stop();
    const generation = this.generation;
    const stream = await navigator.mediaDevices.getUserMedia({ audio: {
      deviceId: deviceId ? { exact: deviceId } : undefined,
      echoCancellation: true, noiseSuppression: true, channelCount: 1,
    }});
    if (generation !== this.generation) { stream.getTracks().forEach(t => t.stop()); return; }
    this.stream = stream;
    try {
      const ctx = new AudioContext({ sampleRate: 16000 });
      this.context = ctx;
      await ctx.audioWorklet.addModule("/pcm-worklet.js");
      if (generation !== this.generation) return;
      this.processor = new AudioWorkletNode(ctx, "pcm-capture");
      this.processor.port.onmessage = event => {
        if (!this.recording) return;
        this.chunks.push(new Int16Array(event.data.pcm)); this.rms = event.data.level;
      };
      ctx.createMediaStreamSource(stream).connect(this.processor);
      this.processor.connect(ctx.destination);
      await ctx.resume();
      this.recording = true;
    } catch (error) { this.stop(); throw error; }
  }
  level() { return this.rms; }
  stop(): { pcm: ArrayBuffer; sampleRate: number } {
    this.generation++; this.recording = false;
    const rate = this.context?.sampleRate || 16000;
    const merged = new Int16Array(this.chunks.reduce((n, c) => n + c.length, 0));
    let offset = 0;
    for (const chunk of this.chunks) { merged.set(chunk, offset); offset += chunk.length; }
    this.processor?.disconnect(); this.stream?.getTracks().forEach(track => track.stop());
    void this.context?.close();
    this.processor = null; this.stream = null; this.context = null; this.chunks = []; this.rms = 0;
    return { pcm: merged.buffer, sampleRate: rate };
  }
}
