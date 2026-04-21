class PcmPlayerProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.left = [];
    this.right = [];
    this.readIndex = 0;
    this.bufferedSamples = 0;
    this.targetBufferedSamples = Math.floor(sampleRate * 0.25);
    this.started = false;
    this.underruns = 0;

    this.port.onmessage = (event) => {
      if (event.data?.type !== "pcm") {
        return;
      }

      const pcm = new Int16Array(event.data.buffer);
      const frames = Math.floor(pcm.length / 2);
      const left = new Float32Array(frames);
      const right = new Float32Array(frames);

      for (let i = 0; i < frames; i++) {
        left[i] = pcm[i * 2] / 32768;
        right[i] = pcm[i * 2 + 1] / 32768;
      }

      this.left.push(left);
      this.right.push(right);
      this.bufferedSamples += frames;
    };
  }

  process(_, outputs) {
    const output = outputs[0];
    const outLeft = output[0];
    const outRight = output[1] || output[0];

    if (!this.started && this.bufferedSamples >= this.targetBufferedSamples) {
      this.started = true;
    }

    if (!this.started) {
      outLeft.fill(0);
      outRight.fill(0);
      return true;
    }

    for (let i = 0; i < outLeft.length; i++) {
      if (this.left.length === 0) {
        outLeft[i] = 0;
        outRight[i] = 0;
        this.underruns++;
        continue;
      }

      const currentLeft = this.left[0];
      const currentRight = this.right[0];
      outLeft[i] = currentLeft[this.readIndex];
      outRight[i] = currentRight[this.readIndex];

      this.readIndex++;
      this.bufferedSamples--;

      if (this.readIndex >= currentLeft.length) {
        this.left.shift();
        this.right.shift();
        this.readIndex = 0;
      }
    }

    return true;
  }
}

registerProcessor("pcm-player", PcmPlayerProcessor);
