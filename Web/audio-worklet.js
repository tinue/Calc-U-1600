// The buzzer's output: mono int16 PCM from the engine worker (via the page)
// into a ring buffer, played at the context's rate (linear resampling from
// the emulation's rate). Silence when the buffer runs dry.
class CalcUBuzzer extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ring = new Float32Array(1 << 16);
    this.read = 0;   // fractional read position, in source samples
    this.write = 0;  // source samples written
    this.step = 48000 / sampleRate;
    this.port.onmessage = (e) => {
      const m = e.data;
      if (m.rate) this.step = m.rate / sampleRate;
      if (m.pcm) this.push(m.pcm);
    };
  }

  push(pcm) {
    const ring = this.ring, mask = ring.length - 1;
    // Never queue more than the ring holds: drop the oldest.
    if (this.write + pcm.length - this.read > ring.length - 2) this.read = this.write + pcm.length - (ring.length - 2);
    for (let i = 0; i < pcm.length; i++) ring[(this.write + i) & mask] = pcm[i] / 32768;
    this.write += pcm.length;
  }

  process(inputs, outputs) {
    const out = outputs[0][0];
    const ring = this.ring, mask = ring.length - 1;
    for (let i = 0; i < out.length; i++) {
      if (this.read + 1 >= this.write) {
        out[i] = 0;
        continue;
      }
      const k = Math.floor(this.read);
      const t = this.read - k;
      out[i] = ring[k & mask] * (1 - t) + ring[(k + 1) & mask] * t;
      this.read += this.step;
    }
    return true;
  }
}

registerProcessor("calcu-buzzer", CalcUBuzzer);
