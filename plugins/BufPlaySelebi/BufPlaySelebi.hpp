// BufPlaySelebi.hpp
// Aleksandar Koruga (aleksandar.koruga@gmail.com)
//
// Buffer player with percussion aware time stretching after SELEBI
// (Akaishi, Holighaus, Yatabe, arXiv:2602.16421): a phase vocoder on a nonstationary
// Gabor transform whose windows shrink around detected percussive events.

#pragma once

#include "SC_PlugIn.hpp"
#include <cstdint>

namespace BufPlaySelebi {

class BufPlaySelebi : public SCUnit {
public:
    BufPlaySelebi();
    ~BufPlaySelebi();

private:
    struct HeapEntry {
        float key;
        int bin;
        bool current;
    };

    void next(int nSamples);

    bool prepare(int winLen);
    void release();
    size_t layoutMemory(char* base);
    void readGrain(const SndBuf* buf, int64_t center, int half, int dir);
    void transform(int half, const float* window, float* spectrum);

    // look-ahead onset analysis on a fixed grid of the buffer
    float analyseFrame(const SndBuf* buf, int64_t frame);
    void updateAnalysis(const SndBuf* buf, int budget);
    bool analysed(int64_t first, int64_t last) const;
    float medianRatio(int64_t frame) const;
    bool percussivePeak(int64_t frame, float& height) const;

    // synthesis
    void makeFrame(const SndBuf* buf);
    void integratePhase(int bins, int hop, RGen& rgen);
    float olaSample(int64_t time) const;
    float streamSample(double pos) const;

    // position changes
    float mixFade(float incoming, float outgoing, int index) const;
    void captureFade();
    void rebuildAt(double target);

    // used by SIMPLE_GET_BUF
    float m_fbufnum;
    SndBuf* m_buf;

    bool m_ok;
    bool m_started;
    const float* m_bufData;
    int m_bufFrames;

    int m_winLen; // long window V, also the FFT size of every frame
    int m_longHop; // synthesis hop of long windows
    int m_anHop; // analysis grid hop
    int m_promReach; // peak prominence reach in analysis frames
    int m_anSide; // analysis frames kept on each side of the playhead
    int m_anRing;
    int m_olaRing;
    float m_magFloor;

    char* m_mem;
    scfft* m_fwd;
    scfft* m_inv;
    float* m_work;
    float* m_spec;
    float* m_grain;

    float* m_tabH;
    float* m_tabTH;
    float* m_tabDH;
    float* m_tabTDH;
    float* m_winH;
    float* m_winTH;
    float* m_winDH;
    float* m_specH;
    float* m_specTH;
    float* m_specDH;
    float* m_specTDH;

    float* m_anRatio;
    int64_t m_anLo;
    int64_t m_anHi;

    float* m_curMag;
    float* m_curNorm;
    float* m_curDt;
    float* m_curDw;
    float* m_curPhase;
    float* m_prevNorm;
    float* m_prevDt;
    float* m_prevPhase;
    uint8_t* m_done;
    HeapEntry* m_heap;
    bool m_prevValid;
    int64_t m_prevTime;

    float* m_olaNum;
    float* m_olaDen;
    int64_t m_nextTime;
    double m_nextCenter;
    double m_readPos;
    int64_t m_clearTime;

    float m_internalSpeed;
    float m_pitch;

    float m_prevTrig;
    float* m_fadeBuf;
    int m_fadeLen;
    int m_fadePos;
};

} // namespace BufPlaySelebi
