// The game's mixer output (48 kHz stereo 16-bit) from the worker's ring (worker.js host_sound_write):
// header [write, read] in frames, then the interleaved samples. Silence when the ring runs dry.
class Ring extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const sab = options.processorOptions.ring;
    this.head = new Int32Array(sab, 0, 2); this.data = new Int16Array(sab, 8); this.frames = (sab.byteLength - 8) / 4;
  }
  process(_inputs, outputs) {
    const [left, right] = outputs[0], h = this.head, cap = this.frames;
    let r = Atomics.load(h, 1); const w = Atomics.load(h, 0);
    for (let i = 0; i < left.length; i++) {
      if (r === w) { left[i] = right[i] = 0; continue; }
      left[i] = this.data[r * 2] / 32768; right[i] = this.data[r * 2 + 1] / 32768; r = (r + 1) % cap;
    }
    Atomics.store(h, 1, r);
    return true;
  }
}
registerProcessor('dhd-ring', Ring);

// Voice chat's microphone (main.js, finding 131): its first channel, 48 kHz (the context's rate),
// into the worker's ring as 16-bit samples: header [write, read] in samples, then the samples. A
// full ring drops what doesn't fit (the worker reads every 10 ms).
class Mic extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const sab = options.processorOptions.ring;
    this.head = new Int32Array(sab, 0, 2); this.data = new Int16Array(sab, 8); this.cap = (sab.byteLength - 8) / 2;
  }
  process(inputs) {
    const input = inputs[0]?.[0];
    if (!input) return true;
    const h = this.head, cap = this.cap, r = Atomics.load(h, 1);
    let w = Atomics.load(h, 0);
    for (let i = 0; i < input.length; i++) {
      if ((w + 1) % cap === r) break;
      const v = Math.max(-1, Math.min(1, input[i]));
      this.data[w] = v < 0 ? v * 32768 : v * 32767; w = (w + 1) % cap;
    }
    Atomics.store(h, 0, w);
    return true;
  }
}
registerProcessor('dhd-mic', Mic);
