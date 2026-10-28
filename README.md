# BufPlaySelebi

Author: Aleksandar Koruga

Made with Claude.

A SuperCollider UGen that plays a buffer with time stretching and pitch shifting that keeps drum hits sharp. It runs in real time and follows SELEBI, a percussion aware phase vocoder by Natsuki Akaishi, Nicki Holighaus and Kohei Yatabe:

> *SELEBI: Percussion-aware Time Stretching via Selective Magnitude Spectrogram Compression by Nonstationary Gabor Transform*, [arXiv:2602.16421](https://arxiv.org/abs/2602.16421)

The paper describes an offline method. This UGen is a streaming adaptation of it, see [Differences from the paper](#differences-from-the-paper).

### How it works

A phase vocoder stretches sound by cutting it into overlapping windows and spacing them further apart. Long windows keep tones clean, but a drum hit gets spread over the whole window, so stretched drums sound soft and blurred.

SELEBI looks for percussive hits and shortens the analysis windows around each one. A hit is then seen by a few short windows only and stays sharp, while tonal material keeps the long windows. The stronger the hit and the larger the stretch, the shorter the windows get.

Hits are found from the phase of the spectrum. For each frequency, the mixed partial derivative of the phase is close to 0 for a steady tone and close to 1 for an impulse. The share of the energy that sits in impulse like frequencies gives a percussiveness curve over time. Its peaks are the hits, and the height of a peak sets how short the windows around it become. Because the UGen plays from a buffer, it reads ahead in the buffer to find hits before it reaches them.

The phases of the stretched sound are rebuilt with phase gradient integration, as in *Phase vocoder done right* (Holighaus and Průša, 2017), which keeps tones and hits coherent.

### Usage

```supercollider
BufPlaySelebi.ar(bufNum, speed = 1, pitch = 1, position = 0, trigPos = 0, posChangeAlgo = 0)
```

- **bufNum**: a mono buffer. Other channel counts output silence. Use `Buffer.readChannel` to load one channel of a stereo file.
- **speed**: playback speed, independent of pitch. 1 is normal, 0.5 is half speed, negative values play backwards. Speeds close to 0 turn into a frozen texture (see below).
- **pitch**: pitch ratio, independent of speed, from 0.25 to 4. 2 is an octave up.
- **position**: start position as a fraction of the buffer, 0 to 1.
- **trigPos**: a rise above 0.5 jumps to `position`. While it stays above 0.5 the playhead is pinned there.
- **posChangeAlgo**: how jumps are handled, see [Position changes](#position-changes).
  - `0` (default): rebuild on jump. The jump is heard at once, with a 4 ms crossfade.
  - `1`: shorter long window, for slower computers. No extra work at a jump, heard about 11 ms late.
  - `2`: jump at the frontier. No extra work at a jump, heard about 45 ms late.

The playhead loops over the buffer. The buffer's sample rate is taken into account, so a 44.1 kHz file plays at the right speed and pitch on a 48 kHz server.

```supercollider
b = Buffer.readChannel(s, "path/to/drums.wav", channels: [0]);

{ BufPlaySelebi.ar(b, speed: 0.5) }.play;            // half speed, same pitch
{ BufPlaySelebi.ar(b, speed: 1, pitch: 1.5) }.play;  // same speed, a fifth up
{ BufPlaySelebi.ar(b, speed: -0.5) }.play;           // half speed, backwards
{ BufPlaySelebi.ar(b, speed: MouseX.kr(0, 1)) }.play; // left edge freezes

// jump to a random position twice a second
{ BufPlaySelebi.ar(b, 0.5, 1, LFNoise0.kr(2).range(0, 1), Impulse.kr(2)) }.play;

// stutter: retrigger the same spot 16 times a second
{ BufPlaySelebi.ar(b, 0.5, 1, 0.25, Impulse.kr(16)) }.play;
```

**Freeze.** Below a speed of pitch / 32, the windows grow back to full length and the phases are progressively randomised. At speed 0 the result is a stationary texture built from the sound under the playhead, in the spirit of Paulstretch.

**Speeding up.** When the speed is at least the pitch ratio (speed 2 at pitch 1, or speed 1 at pitch 1), nothing gets stretched, so all windows stay long and it works as a plain phase vocoder.

**Memory.** Each instance takes about 450 KB of real time memory for 44.1 and 48 kHz buffers, about twice that for 88.2 and 96 kHz buffers, and about a quarter with `posChangeAlgo` 1. The server's default pool of 8 MB holds roughly 17 instances. Raise `s.options.memSize` before booting for more. If memory runs out, the UGen posts a message and outputs silence.

### Position changes

To play a sample, every window that overlaps it has to be finished, so the UGen builds its output half a long window ahead of what you hear. How a jump deals with that lead is set by `posChangeAlgo`. The delays below are for pitch 1, they get shorter at higher pitch and longer at lower pitch.

- **0, rebuild on jump** (default). The output that is not yet played is dropped and the stream restarts at the playhead from the new position, the same way a synth starts. The old sound fades out over 4 ms while the new one fades in, so the jump is heard at once and fast stutters work. The cost is a burst of about 10 windows (some 40 FFTs) computed in the block of the jump.
- **1, shorter long window.** The long window is a quarter of the usual length, about 21 ms, so the lead and the delay of a jump shrink to about 11 ms, with no burst. Tones are a little less clean, especially low ones. The steady CPU load is about the same as the other modes, this mode only avoids the burst at each jump.
- **2, jump at the frontier.** The jump only changes where the next window is read. It is heard about 45 ms late and crossfades over the window overlap. No burst.

Changing `posChangeAlgo` to or from 1 resizes the UGen and restarts playback.

### Known limitations

- **Hits right after a jump to a new region are not sharpened.** After a jump or at synth start, the look ahead analysis needs roughly 25 ms to catch up. Windows produced before that stay long, so a hit at the jump point itself is played like a plain phase vocoder would. Jumping to a spot played or analysed in the last moments, as in a stutter, reuses the analysis and is sharp at once.
- **Mono only.**
- **The long window is the paper's 93 ms rounded to a power of two** at the buffer's sample rate, so 85 to 93 ms for 44.1, 48, 88.2 and 96 kHz buffers, and further off at other rates. `posChangeAlgo` 1 divides it by 4.

### Differences from the paper

The paper processes a whole file offline. Running from a buffer in real time changes some parts, and a few values are not given in the paper.

- Hits are searched on a fixed grid of 1/32 of the long window. In the paper the grid follows the stretch factor, so it is finer at large stretches.
- A peak's prominence is judged within one long window on each side, not over the whole signal.
- Window lengths and hop sizes follow a continuous rule derived from Algorithm 3 and Section IV-D, instead of the paper's region types and transition table. When two hits are close, the shorter window wins, which is simpler than the paper's handling.
- Phase integration uses the previous window only. The paper's offline version integrates over the whole spectrogram.
- Values the paper leaves open or prints ambiguously: a median filter of 5 frames, the impulse band of the phase derivative read as 0.5 to 1.75, and a magnitude floor scaled from the paper's 0.01. All windows are Hann.
- The shortest window formula (Eq. 10) uses a stretch of at most 8. The paper tests stretches up to 4.
- Added beyond the paper: pitch shifting, backwards playback, the freeze, speeding up, and the position change modes.

### Requirements

- CMake >= 3.5
- SuperCollider source code
- No other dependencies. The FFT comes from SuperCollider's plugin interface.

Built and tested with SuperCollider 3.14.1 on Windows, with scsynth. A supernova build is produced but has not been tested, and neither have macOS or Linux.

### Building

Clone the repository, then from its folder:

    mkdir build
    cd build

Configure and build:

    cmake .. -DCMAKE_BUILD_TYPE=Release
    cmake --build . --config Release
    cmake --build . --config Release --target install

You may want to specify the install location in the first step to point it at your SuperCollider extensions directory (`Platform.userExtensionDir`): add the option `-DCMAKE_INSTALL_PREFIX=/path/to/extensions`.

It's expected that the SuperCollider repo is cloned at `../supercollider` relative to this repo. If it's not: add the option `-DSC_PATH=/path/to/sc/source`. Building the supernova plugin needs SuperCollider's `nova-tt` submodule. Add `-DSUPERNOVA=OFF` to build for scsynth only.

### License

MIT, see [LICENSE](LICENSE).
