// BufPlaySelebi.cpp
// Aleksandar Koruga (aleksandar.koruga@gmail.com)

#include "BufPlaySelebi.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

static InterfaceTable* ft;

namespace BufPlaySelebi {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kHalfPi = 1.57079632679490f;
constexpr float kTwoPi = 6.28318530717959f;

// SELEBI parameters, Sec. IV and VI of the paper
constexpr float kBeta = 4.f; // synthesis overlap of the shortest windows
constexpr float kPeakProminence = 0.1f;
constexpr float kMpdLow = 0.5f; // Eq. 9 band read as 1 - 0.5 < MPD < 1 + 0.75
constexpr float kMpdHigh = 1.75f;

constexpr float kMaxAlpha = 8.f; // stretch used by Eq. 10 at slower speeds
constexpr float kFreezeSpeed = 1.f / 32.f; // below this internal speed the phases diffuse into a freeze
constexpr float kMinPitch = 0.25f;
constexpr float kMaxPitch = 4.f;
constexpr int kCatchUpFrames = 4; // extra analysis frames per 64 output samples
constexpr float kPhaseTolerance = 1e-5f;
constexpr float kDenFloor = 1e-3f;
constexpr float kTaperBins = 32.f;

// posChangeAlgo
constexpr int kRebuildOnJump = 0;
constexpr int kShortWindow = 1; // long window divided by kShortWindowDivisor
constexpr int kJumpAtFrontier = 2;
constexpr int kShortWindowDivisor = 4;
constexpr double kJumpFadeSeconds = 0.004;

// V = 2^11 at 22.05 kHz in the paper, the same duration rounded to a power of two here
int longWindowLength(double sampleRate) {
    const int log2Len = (int)std::lround(std::log2(2048.0 * sampleRate / 22050.0));
    return 1 << sc_clip(log2Len, 8, 15);
}

// Hann window of length len centred on index len / 2, with t * h, h' and t * h'
void makeWindows(int len, float* h, float* th, float* dh, float* tdh) {
    const int half = len / 2;
    const double step = 6.283185307179586 / len;
    for (int i = 0; i < len; ++i) {
        const int j = i - half;
        h[i] = (float)(0.5 + 0.5 * std::cos(step * j));
        th[i] = j * h[i];
        dh[i] = (float)(-0.5 * step * std::sin(step * j));
        if (tdh)
            tdh[i] = j * dh[i];
    }
}

// SC packed spectrum: DC and Nyquist real parts first, then interleaved bins
inline std::complex<float> getBin(const float* spectrum, int bin, int nyquist) {
    if (bin == 0)
        return { spectrum[0], 0.f };
    if (bin == nyquist)
        return { spectrum[1], 0.f };
    return { spectrum[2 * bin], spectrum[2 * bin + 1] };
}

inline float wrapPhase(float phase) { return phase - kTwoPi * std::floor(phase / kTwoPi + 0.5f); }

template <typename T> void take(char* base, size_t& used, T*& ptr, size_t count) {
    if (base)
        ptr = reinterpret_cast<T*>(base + used);
    used += (count * sizeof(T) + 63) & ~size_t(63);
}

} // namespace

BufPlaySelebi::BufPlaySelebi():
    m_fbufnum(-1.f),
    m_buf(nullptr),
    m_ok(false),
    m_started(false),
    m_bufData(nullptr),
    m_bufFrames(0),
    m_winLen(0),
    m_mem(nullptr),
    m_fwd(nullptr),
    m_inv(nullptr),
    m_nextCenter(0.0),
    m_internalSpeed(1.f),
    m_pitch(1.f),
    m_prevTrig(0.f) {
    mCalcFunc = make_calc_function<BufPlaySelebi, &BufPlaySelebi::next>();
    next(1);
}

BufPlaySelebi::~BufPlaySelebi() { release(); }

// Sizes everything from the long window, which follows the buffer's sample rate because
// the frames slice buffer samples. The stream restarts, the playhead position is kept.
bool BufPlaySelebi::prepare(int winLen) {
    release();
    m_winLen = winLen;
    m_longHop = winLen / 16;
    m_anHop = winLen / 32;
    m_promReach = winLen / m_anHop;
    m_anSide = 2 * winLen / m_anHop;
    m_anRing = 4 * m_anSide;
    m_olaRing = 2 * winLen;
    m_magFloor = 0.01f * winLen / 2048.f;
    m_fadeLen = sc_max(1, (int)std::lround(kJumpFadeSeconds * sampleRate()));

    m_anLo = 0;
    m_anHi = -1;
    m_prevValid = false;
    m_prevTime = 0;
    m_nextTime = 0;
    m_readPos = 0.0;
    // the first window starts half a window before time 0, nothing earlier is ever written
    m_clearTime = -winLen / 2;
    m_fadePos = m_fadeLen;

    const size_t bytes = layoutMemory(nullptr);
    m_mem = (char*)RTAlloc(mWorld, bytes);
    if (!m_mem) {
        Print("BufPlaySelebi: not enough real time memory for %d KB, raise s.options.memSize\n", (int)(bytes / 1024));
        return false;
    }
    layoutMemory(m_mem);
    std::memset(m_mem, 0, bytes);
    makeWindows(winLen, m_tabH, m_tabTH, m_tabDH, m_tabTDH);
    SCWorld_Allocator alloc(ft, mWorld);
    m_fwd = scfft_create(winLen, winLen, kRectWindow, m_work, m_spec, kForward, alloc);
    m_inv = scfft_create(winLen, winLen, kRectWindow, m_spec, m_work, kBackward, alloc);
    return m_fwd && m_inv;
}

void BufPlaySelebi::release() {
    SCWorld_Allocator alloc(ft, mWorld);
    if (m_fwd)
        scfft_destroy(m_fwd, alloc);
    if (m_inv)
        scfft_destroy(m_inv, alloc);
    if (m_mem)
        RTFree(mWorld, m_mem);
    m_fwd = nullptr;
    m_inv = nullptr;
    m_mem = nullptr;
}

size_t BufPlaySelebi::layoutMemory(char* base) {
    const size_t len = m_winLen;
    const size_t bins = len / 2 + 1;
    size_t used = 0;
    take(base, used, m_work, len);
    take(base, used, m_spec, len);
    take(base, used, m_grain, len);
    take(base, used, m_tabH, len);
    take(base, used, m_tabTH, len);
    take(base, used, m_tabDH, len);
    take(base, used, m_tabTDH, len);
    take(base, used, m_winH, len);
    take(base, used, m_winTH, len);
    take(base, used, m_winDH, len);
    take(base, used, m_specH, len);
    take(base, used, m_specTH, len);
    take(base, used, m_specDH, len);
    take(base, used, m_specTDH, len);
    take(base, used, m_anRatio, m_anRing);
    take(base, used, m_curMag, bins);
    take(base, used, m_curNorm, bins);
    take(base, used, m_curDt, bins);
    take(base, used, m_curDw, bins);
    take(base, used, m_curPhase, bins);
    take(base, used, m_prevNorm, bins);
    take(base, used, m_prevDt, bins);
    take(base, used, m_prevPhase, bins);
    take(base, used, m_done, bins);
    take(base, used, m_heap, 2 * bins);
    take(base, used, m_olaNum, m_olaRing);
    take(base, used, m_olaDen, m_olaRing);
    take(base, used, m_fadeBuf, m_fadeLen);
    return used;
}

// grain[i] is the buffer sample at center + dir * (i - half), wrapped around the buffer
void BufPlaySelebi::readGrain(const SndBuf* buf, int64_t center, int half, int dir) {
    const int frames = buf->frames;
    const float* data = buf->data;
    int index = (int)(((center - dir * half) % frames + frames) % frames);
    for (int i = 0; i < 2 * half; ++i) {
        m_grain[i] = data[index];
        index += dir;
        if (index >= frames)
            index -= frames;
        else if (index < 0)
            index += frames;
    }
}

// FFT of the windowed grain with its centre at index 0 (the phase convention of Eq. 1)
void BufPlaySelebi::transform(int half, const float* window, float* spectrum) {
    const int mask = m_winLen - 1;
    std::fill(m_work, m_work + m_winLen, 0.f);
    for (int i = 0; i < 2 * half; ++i)
        m_work[(i - half) & mask] = m_grain[i] * window[i];
    scfft_dofft(m_fwd);
    std::copy(m_spec, m_spec + m_winLen, spectrum);
}

// Alg. 2 lines 1 to 6 for one frame: share of the magnitude in bins whose mixed partial
// derivative of the phase marks them as impulsive. MPD = 1 + Re(X_th' / X_h - X_th X_h' / X_h^2),
// 0 for a sinusoid and 1 for an impulse.
float BufPlaySelebi::analyseFrame(const SndBuf* buf, int64_t frame) {
    const int nyquist = m_winLen / 2;
    readGrain(buf, frame * m_anHop, nyquist, 1);
    transform(nyquist, m_tabH, m_specH);
    transform(nyquist, m_tabTH, m_specTH);
    transform(nyquist, m_tabDH, m_specDH);
    transform(nyquist, m_tabTDH, m_specTDH);

    float total = 0.f;
    float percussive = 0.f;
    for (int bin = 0; bin <= nyquist; ++bin) {
        const std::complex<float> xh = getBin(m_specH, bin, nyquist);
        const float power = std::norm(xh);
        const float mag = std::sqrt(power);
        total += mag;
        if (mag <= m_magFloor)
            continue;
        const std::complex<float> inv = std::conj(xh) / power;
        const float mpd = 1.f
            + std::real(getBin(m_specTDH, bin, nyquist) * inv
                        - getBin(m_specTH, bin, nyquist) * getBin(m_specDH, bin, nyquist) * inv * inv);
        if (mpd > kMpdLow && mpd < kMpdHigh)
            percussive += mag;
    }
    return total > 0.f ? percussive / total : 0.f;
}

// Keeps the frames within m_anSide of the playhead analysed, growing the range by at most
// budget frames per call. A jump outside the range restarts it at the playhead.
void BufPlaySelebi::updateAnalysis(const SndBuf* buf, int budget) {
    const int64_t center = (int64_t)std::floor(m_nextCenter / m_anHop + 0.5);
    const int64_t low = center - m_anSide;
    const int64_t high = center + m_anSide;
    if (m_anHi < m_anLo || m_anHi < low || m_anLo > high) {
        m_anLo = center;
        m_anHi = center - 1;
    }

    for (; budget > 0; --budget) {
        int64_t frame;
        if (m_anHi < m_anLo) {
            frame = center;
            m_anLo = m_anHi = center;
        } else {
            const int64_t front = high - m_anHi;
            const int64_t back = m_anLo - low;
            if (front <= 0 && back <= 0)
                break;
            if (front > back || (front == back && m_internalSpeed >= 0.f)) {
                frame = ++m_anHi;
                if (m_anHi - m_anLo >= m_anRing)
                    ++m_anLo;
            } else {
                frame = --m_anLo;
                if (m_anHi - m_anLo >= m_anRing)
                    --m_anHi;
            }
        }
        m_anRatio[frame & (m_anRing - 1)] = analyseFrame(buf, frame);
    }
}

bool BufPlaySelebi::analysed(int64_t first, int64_t last) const {
    return m_anLo <= m_anHi && m_anLo <= first && last <= m_anHi;
}

// Alg. 2 line 6, the paper gives no median length
float BufPlaySelebi::medianRatio(int64_t frame) const {
    float values[5];
    for (int i = 0; i < 5; ++i)
        values[i] = m_anRatio[(frame + i - 2) & (m_anRing - 1)];
    std::nth_element(values, values + 2, values + 5);
    return values[2];
}

// Alg. 2 line 8, prominence measured within m_promReach frames on each side
bool BufPlaySelebi::percussivePeak(int64_t frame, float& height) const {
    height = medianRatio(frame);
    if (!(height > medianRatio(frame - 1) && height >= medianRatio(frame + 1)))
        return false;

    float leftMin = height;
    for (int64_t other = frame - 1; other >= frame - m_promReach; --other) {
        const float value = medianRatio(other);
        if (value > height)
            break;
        leftMin = sc_min(leftMin, value);
    }
    float rightMin = height;
    for (int64_t other = frame + 1; other <= frame + m_promReach; ++other) {
        const float value = medianRatio(other);
        if (value > height)
            break;
        rightMin = sc_min(rightMin, value);
    }
    return height - sc_max(leftMin, rightMin) >= kPeakProminence;
}

// Real time phase gradient heap integration with unscaled gradients (PVDR): the time
// gradient is integrated over the synthesis hop, the frequency gradient keeps transients
// at their analysed length.
void BufPlaySelebi::integratePhase(int bins, int hop, RGen& rgen) {
    float maxNorm = 0.f;
    for (int bin = 0; bin < bins; ++bin) {
        maxNorm = sc_max(maxNorm, m_curNorm[bin]);
        if (m_prevValid)
            maxNorm = sc_max(maxNorm, m_prevNorm[bin]);
    }
    const float floorNorm = maxNorm * kPhaseTolerance;

    int remaining = 0;
    for (int bin = 0; bin < bins; ++bin) {
        if (m_curNorm[bin] > floorNorm) {
            m_done[bin] = 0;
            ++remaining;
        } else {
            m_done[bin] = 1;
            m_curPhase[bin] = kPi * rgen.frand2();
        }
    }

    const auto byKey = [](const HeapEntry& a, const HeapEntry& b) { return a.key < b.key; };
    int size = 0;
    const auto push = [&](float key, int bin, bool current) {
        m_heap[size++] = { key, bin, current };
        std::push_heap(m_heap, m_heap + size, byKey);
    };
    const auto fromPrevious = [&](int bin) {
        return wrapPhase(m_prevPhase[bin] + 0.5f * hop * (m_prevDt[bin] + m_curDt[bin]));
    };

    if (m_prevValid) {
        for (int bin = 0; bin < bins; ++bin)
            if (m_prevNorm[bin] > floorNorm)
                push(m_prevNorm[bin], bin, false);
    }

    const float halfStep = 0.5f * kPi / (bins - 1);
    while (remaining > 0) {
        if (size == 0) {
            int best = -1;
            for (int bin = 0; bin < bins; ++bin)
                if (!m_done[bin] && (best < 0 || m_curNorm[bin] > m_curNorm[best]))
                    best = bin;
            m_curPhase[best] = m_prevValid ? fromPrevious(best) : 0.f;
            m_done[best] = 1;
            --remaining;
            push(m_curNorm[best], best, true);
            continue;
        }

        std::pop_heap(m_heap, m_heap + size, byKey);
        const HeapEntry top = m_heap[--size];
        const int bin = top.bin;
        if (!top.current) {
            if (!m_done[bin]) {
                m_curPhase[bin] = fromPrevious(bin);
                m_done[bin] = 1;
                --remaining;
                push(m_curNorm[bin], bin, true);
            }
            continue;
        }
        if (bin + 1 < bins && !m_done[bin + 1]) {
            m_curPhase[bin + 1] = wrapPhase(m_curPhase[bin] + halfStep * (m_curDw[bin] + m_curDw[bin + 1]));
            m_done[bin + 1] = 1;
            --remaining;
            push(m_curNorm[bin + 1], bin + 1, true);
        }
        if (bin > 0 && !m_done[bin - 1]) {
            m_curPhase[bin - 1] = wrapPhase(m_curPhase[bin] - halfStep * (m_curDw[bin] + m_curDw[bin - 1]));
            m_done[bin - 1] = 1;
            --remaining;
            push(m_curNorm[bin - 1], bin - 1, true);
        }
    }
}

void BufPlaySelebi::makeFrame(const SndBuf* buf) {
    const int len = m_winLen;
    const double center = m_nextCenter;
    const int64_t time = m_nextTime;
    const float speed = m_internalSpeed;
    const float absSpeed = std::fabs(speed);
    const float alpha = absSpeed > 1.f / kMaxAlpha ? 1.f / absSpeed : kMaxAlpha;
    const float freeze = sc_clip(1.f - absSpeed / kFreezeSpeed, 0.f, 1.f);

    // Alg. 3 as a continuous rule: a window reaches an event centre only once it is S_k long,
    // S_k from Eq. 10. The windows lengthen again as the speed approaches a freeze.
    int winLen = len;
    int shortLen = len;
    if (alpha > 1.f && freeze < 1.f) {
        const int64_t first = (int64_t)std::ceil((center - len / 2) / m_anHop);
        const int64_t last = (int64_t)std::floor((center + len / 2) / m_anHop);
        for (int64_t frame = first; frame <= last; ++frame) {
            float height;
            if (!analysed(frame - m_promReach - 2, frame + m_promReach + 2) || !percussivePeak(frame, height))
                continue;
            float minLen = std::floor(len - height * height * (1.f - 1.f / alpha) * len);
            minLen += freeze * (len - minLen);
            const float reach = (float)(2.0 * std::fabs(center - (double)frame * m_anHop));
            const int frameLen = (int)sc_clip(reach, minLen, (float)len);
            if (frameLen < winLen) {
                winLen = frameLen;
                shortLen = (int)minLen;
            }
        }
    }
    winLen = sc_max(winLen & ~1, 2);

    // Sec. IV-D: the hop goes linearly from the long hop to S_k / beta as the window shrinks
    int hop = m_longHop;
    if (shortLen < len) {
        const float depth = sc_min(1.f, (float)(len - winLen) / (float)(len - shortLen));
        hop = (int)std::lround(m_longHop + (shortLen / kBeta - m_longHop) * depth);
    }
    hop = sc_max(hop, 1);

    const int half = winLen / 2;
    const float* winH = m_tabH;
    const float* winTH = m_tabTH;
    const float* winDH = m_tabDH;
    if (winLen < len) {
        makeWindows(winLen, m_winH, m_winTH, m_winDH, nullptr);
        winH = m_winH;
        winTH = m_winTH;
        winDH = m_winDH;
    }

    // a negative speed reads the grain backwards, which plays it reversed
    readGrain(buf, (int64_t)std::llround(center), half, speed < 0.f ? -1 : 1);
    transform(half, winH, m_specH);
    transform(half, winTH, m_specTH);
    transform(half, winDH, m_specDH);

    const int nyquist = len / 2;
    const int bins = nyquist + 1;
    const float binStep = kTwoPi / len;
    const float normScale = 1.f / half; // the Hann window sums to half
    for (int bin = 0; bin < bins; ++bin) {
        const std::complex<float> xh = getBin(m_specH, bin, nyquist);
        const float power = std::norm(xh);
        const float mag = std::sqrt(power);
        m_curMag[bin] = mag;
        m_curNorm[bin] = mag * normScale;
        if (power > 0.f) {
            const std::complex<float> inv = std::conj(xh) / power;
            m_curDt[bin] = bin * binStep - std::imag(getBin(m_specDH, bin, nyquist) * inv);
            m_curDw[bin] = -std::real(getBin(m_specTH, bin, nyquist) * inv);
        } else {
            m_curDt[bin] = bin * binStep;
            m_curDw[bin] = 0.f;
        }
    }

    RGen& rgen = *mParent->mRGen;
    integratePhase(bins, (int)(time - m_prevTime), rgen);

    // pitch above 1 reads the stretched stream faster, so bins above its new Nyquist are removed
    const float cutoff = m_pitch > 1.f ? nyquist / m_pitch : nyquist + kTaperBins;
    for (int bin = 0; bin < bins; ++bin) {
        if (freeze > 0.f)
            m_curPhase[bin] = wrapPhase(m_curPhase[bin] + freeze * kPi * rgen.frand2());
        const float amp = m_curMag[bin] * sc_clip((cutoff - bin) / kTaperBins, 0.f, 1.f);
        const float re = amp * std::cos(m_curPhase[bin]);
        const float im = amp * std::sin(m_curPhase[bin]);
        if (bin == 0) {
            m_spec[0] = re;
        } else if (bin == nyquist) {
            m_spec[1] = re;
        } else {
            m_spec[2 * bin] = re;
            m_spec[2 * bin + 1] = im;
        }
    }
    scfft_doifft(m_inv);

    // overlap-add divided by the summed squared windows, the dual window of Eq. 3 on the synthesis grid
    const int mask = len - 1;
    const int64_t ringMask = m_olaRing - 1;
    for (int i = 0; i < winLen; ++i) {
        const int offset = i - half;
        const float weight = winH[i];
        const int64_t at = (time + offset) & ringMask;
        m_olaNum[at] += weight * m_work[offset & mask];
        m_olaDen[at] += weight * weight;
    }

    std::swap(m_prevPhase, m_curPhase);
    std::swap(m_prevDt, m_curDt);
    std::swap(m_prevNorm, m_curNorm);
    m_prevValid = true;
    m_prevTime = time;
    m_nextTime = time + hop;
    m_nextCenter = center + hop * (double)speed;
}

float BufPlaySelebi::olaSample(int64_t time) const {
    const int64_t at = time & (m_olaRing - 1);
    return m_olaNum[at] / sc_max(m_olaDen[at], kDenFloor);
}

float BufPlaySelebi::streamSample(double pos) const {
    const int64_t base = (int64_t)std::floor(pos);
    return cubicinterp((float)(pos - base), olaSample(base - 1), olaSample(base), olaSample(base + 1),
                       olaSample(base + 2));
}

// equal power crossfade from the captured old output to the new stream
float BufPlaySelebi::mixFade(float incoming, float outgoing, int index) const {
    const float progress = (index + 0.5f) / m_fadeLen;
    return incoming * std::sin(kHalfPi * progress) + outgoing * std::cos(kHalfPi * progress);
}

// What the old stream would play next, a crossfade still running included, becomes the fade out.
// Samples beyond the frames already made come from fewer windows, which the normalisation absorbs.
void BufPlaySelebi::captureFade() {
    for (int i = 0; i < m_fadeLen; ++i) {
        float value = streamSample(m_readPos + i * (double)m_pitch);
        const int running = m_fadePos + i;
        if (running < m_fadeLen)
            value = mixFade(value, m_fadeBuf[running], running);
        m_fadeBuf[i] = value;
    }
    m_fadePos = 0;
}

// Restarts the stream at the playhead from target, the same way a synth starts.
void BufPlaySelebi::rebuildAt(double target) {
    captureFade();
    std::fill(m_olaNum, m_olaNum + m_olaRing, 0.f);
    std::fill(m_olaDen, m_olaDen + m_olaRing, 0.f);
    const int64_t base = (int64_t)std::floor(m_readPos);
    m_nextTime = base;
    m_nextCenter = target;
    m_prevValid = false;
    m_clearTime = base - m_winLen / 2;
}

void BufPlaySelebi::next(int nSamples) {
    auto* unit = this;
    SIMPLE_GET_BUF_SHARED

    if (!m_buf || !m_buf->data || m_buf->frames <= 0 || m_buf->channels != 1) {
        ClearUnitOutputs(this, nSamples);
        return;
    }

    const int mode = numInputs() > 5 ? sc_clip((int)in0(5), kRebuildOnJump, kJumpAtFrontier) : kRebuildOnJump;
    const double bufRate = m_buf->samplerate > 0.0 ? m_buf->samplerate : sampleRate();
    int winLen = longWindowLength(bufRate);
    if (mode == kShortWindow)
        winLen = sc_max(winLen / kShortWindowDivisor, 256);
    if (winLen != m_winLen)
        m_ok = prepare(winLen);
    if (!m_ok) {
        ClearUnitOutputs(this, nSamples);
        return;
    }

    const int frames = m_buf->frames;
    if (m_buf->data != m_bufData || frames != m_bufFrames) {
        m_bufData = m_buf->data;
        m_bufFrames = frames;
        m_anLo = 0;
        m_anHi = -1;
    }

    // the stretched stream runs at the buffer rate, reading it at pitch * rateScale per output
    // sample keeps both duration and pitch right when the buffer and engine rates differ
    const float speed = in0(1);
    const float pitch = sc_clip(in0(2), kMinPitch, kMaxPitch);
    const float rateScale = (float)(bufRate / sampleRate());
    m_pitch = pitch * rateScale;
    m_internalSpeed = speed / pitch;

    // a rising trigger jumps, holding it high pins the playhead at the frontier
    const float trig = in0(4);
    const bool rising = trig > 0.5f && m_prevTrig <= 0.5f;
    m_prevTrig = trig;
    if (!m_started || trig > 0.5f) {
        double target = sc_frac((double)in0(3)) * frames;
        // the jump lands on the loop pass nearest the playhead, so the analysis carries over
        if (m_started)
            target += std::round((m_nextCenter - target) / frames) * frames;
        if (m_started && rising && mode == kRebuildOnJump)
            rebuildAt(target);
        else
            m_nextCenter = target;
        m_started = true;
    }

    const float absSpeed = std::fabs(m_internalSpeed);
    if (absSpeed > 0.f && absSpeed < 1.f) {
        const int budget = (int)std::ceil(std::fabs(speed) * rateScale * nSamples / m_anHop) + 1
            + kCatchUpFrames * ((nSamples + 63) / 64);
        updateAnalysis(m_buf, budget);
    }

    // frames run ahead by the reach of one more read step, so no window starts before the cleared region
    const int64_t lead = 2 + m_winLen / 2 + (int64_t)std::ceil(m_pitch);
    float* output = out(0);
    const int64_t ringMask = m_olaRing - 1;
    for (int i = 0; i < nSamples; ++i) {
        const int64_t base = (int64_t)std::floor(m_readPos);
        while (!m_prevValid || m_prevTime < base + lead)
            makeFrame(m_buf);

        float value = streamSample(m_readPos);
        if (m_fadePos < m_fadeLen) {
            value = mixFade(value, m_fadeBuf[m_fadePos], m_fadePos);
            ++m_fadePos;
        }
        output[i] = value;

        m_readPos += m_pitch;
        const int64_t keep = (int64_t)std::floor(m_readPos) - 1;
        for (; m_clearTime < keep; ++m_clearTime) {
            m_olaNum[m_clearTime & ringMask] = 0.f;
            m_olaDen[m_clearTime & ringMask] = 0.f;
        }
    }
}

} // namespace BufPlaySelebi

PluginLoad(BufPlaySelebiUGens) {
    ft = inTable;
    registerUnit<BufPlaySelebi::BufPlaySelebi>(ft, "BufPlaySelebi");
}
