/**
 * OpenAL cross platform audio library
 * Copyright (C) 2018 by Raul Herraiz.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iterator>

#include "alc/effects/base.h"
#include "alcomplex.h"
#include "almalloc.h"
#include "alnumbers.h"
#include "alnumeric.h"
#include "alspan.h"
#include "core/bufferline.h"
#include "core/devformat.h"
#include "core/device.h"
#include "core/effectslot.h"
#include "core/mixer.h"
#include "core/mixer/defs.h"
#include "intrusive_ptr.h"

struct ContextBase;


namespace {

using uint = unsigned int;
using complex_d = std::complex<double>;

constexpr size_t StftSize{1024};
constexpr size_t StftHalfSize{StftSize >> 1};
constexpr size_t OversampleFactor{4};

static_assert(StftSize%OversampleFactor == 0, "Factor must be a clean divisor of the size");
constexpr size_t StftStep{StftSize / OversampleFactor};

/* Define a Hann window, used to filter the STFT input and output. */
std::array<double,StftSize> InitHannWindow()
{
    std::array<double,StftSize> ret;
    /* Create lookup table of the Hann window for the desired size. */
    for(size_t i{0};i < StftHalfSize;i++)
    {
        constexpr double scale{al::numbers::pi / double{StftSize}};
        const double val{std::sin((static_cast<double>(i)+0.5) * scale)};
        ret[i] = ret[StftSize-1-i] = val * val;
    }
    return ret;
}
alignas(16) const std::array<double,StftSize> HannWindow = InitHannWindow();


struct FrequencyBin {
    double Magnitude;
    double FreqBin;
};


struct PshifterState final : public EffectState {
    /* Effect parameters */
    size_t mCount;
    size_t mPos;
    uint mPitchShiftI;
    double mPitchShift;

    /* Effects buffers */
    std::array<double,StftSize> mFIFO;
    std::array<double,StftHalfSize+1> mLastPhase;
    std::array<double,StftHalfSize+1> mSumPhase;
    std::array<double,StftSize> mOutputAccum;

    std::array<complex_d,StftSize> mFftBuffer;

    std::array<FrequencyBin,StftHalfSize+1> mAnalysisBuffer;
    std::array<FrequencyBin,StftHalfSize+1> mSynthesisBuffer;

    alignas(16) FloatBufferLine mBufferOut;

    /* Effect gains for each output channel */
    float mCurrentGains[MaxAmbiChannels];
    float mTargetGains[MaxAmbiChannels];


    void deviceUpdate(const DeviceBase *device, const Buffer &buffer) override;
    void update(const ContextBase *context, const EffectSlot *slot, const EffectProps *props,
        const EffectTarget target) override;
    void process(const size_t samplesToDo, const al::span<const FloatBufferLine> samplesIn,
        const al::span<FloatBufferLine> samplesOut) override;

    DEF_NEWDEL(PshifterState)
};

void PshifterState::deviceUpdate(const DeviceBase*, const Buffer&)
{
    /* (Re-)initializing parameters and clear the buffers. */
    mCount       = 0;
    mPos         = StftSize - StftStep;
    mPitchShiftI = MixerFracOne;
    mPitchShift  = 1.0;

    std::fill(mFIFO.begin(),            mFIFO.end(),            0.0);
    std::fill(mLastPhase.begin(),       mLastPhase.end(),       0.0);
    std::fill(mSumPhase.begin(),        mSumPhase.end(),        0.0);
    std::fill(mOutputAccum.begin(),     mOutputAccum.end(),     0.0);
    std::fill(mFftBuffer.begin(),       mFftBuffer.end(),       complex_d{});
    std::fill(mAnalysisBuffer.begin(),  mAnalysisBuffer.end(),  FrequencyBin{});
    std::fill(mSynthesisBuffer.begin(), mSynthesisBuffer.end(), FrequencyBin{});

    std::fill(std::begin(mCurrentGains), std::end(mCurrentGains), 0.0f);
    std::fill(std::begin(mTargetGains),  std::end(mTargetGains),  0.0f);
}

void PshifterState::update(const ContextBase*, const EffectSlot *slot,
    const EffectProps *props, const EffectTarget target)
{
    const int tune{props->Pshifter.CoarseTune*100 + props->Pshifter.FineTune};
    const float pitch{std::pow(2.0f, static_cast<float>(tune) / 1200.0f)};
    mPitchShiftI = fastf2u(pitch*MixerFracOne);
    mPitchShift  = mPitchShiftI * double{1.0/MixerFracOne};

    static constexpr auto coeffs = CalcDirectionCoeffs({0.0f, 0.0f, -1.0f});

    mOutTarget = target.Main->Buffer;
    ComputePanGains(target.Main, coeffs.data(), slot->Gain, mTargetGains);
}

void PshifterState::process(const size_t samplesToDo,
    const al::span<const FloatBufferLine> samplesIn, const al::span<FloatBufferLine> samplesOut)
{
    /* Pitch shifter engine based on the work of Stephan Bernsee.
     * http://blogs.zynaptiq.com/bernsee/pitch-shifting-using-the-ft/
     */

    /* Cycle offset per update expected of each frequency bin (bin 0 is none,
     * bin 1 is x1, bin 2 is x2, etc).
     */
    constexpr double expected_cycles{al::numbers::pi*2.0 / OversampleFactor};

    for(size_t base{0u};base < samplesToDo;)
    {
        const size_t todo{minz(StftStep-mCount, samplesToDo-base)};

        /* Retrieve the output samples from the FIFO and fill in the new input
         * samples.
         */
        auto fifo_iter = mFIFO.begin()+mPos + mCount;
        std::transform(fifo_iter, fifo_iter+todo, mBufferOut.begin()+base,
            [](double d) noexcept -> float { return static_cast<float>(d); });

        std::copy_n(samplesIn[0].begin()+base, todo, fifo_iter);
        mCount += todo;
        base += todo;

        /* Check whether FIFO buffer is filled with new samples. */
        if(mCount < StftStep) break;
        mCount = 0;
        mPos = (mPos+StftStep) & (mFIFO.size()-1);

        /* Time-domain signal windowing, store in FftBuffer, and apply a
         * forward FFT to get the frequency-domain signal.
         */
        for(size_t src{mPos}, k{0u};src < StftSize;++src,++k)
            mFftBuffer[k] = mFIFO[src] * HannWindow[k];
        for(size_t src{0u}, k{StftSize-mPos};src < mPos;++src,++k)
            mFftBuffer[k] = mFIFO[src] * HannWindow[k];
        forward_fft(al::as_span(mFftBuffer));

        /* Analyze the obtained data. Since the real FFT is symmetric, only
         * StftHalfSize+1 samples are needed.
         */
        for(size_t k{0u};k < StftHalfSize+1;k++)
        {
            const double magnitude{std::abs(mFftBuffer[k])};
            const double phase{std::arg(mFftBuffer[k])};

            /* Compute the phase difference from the last update and subtract
             * the expected phase difference for this bin.
             *
             * When oversampling, the expected per-update offset increments by
             * 1/OversampleFactor for every frequency bin. So, the offset wraps
             * every 'OversampleFactor' bin.
             */
            const auto bin_offset = static_cast<double>(k % OversampleFactor);
            double tmp{(phase - mLastPhase[k]) - bin_offset*expected_cycles};
            /* Store the actual phase for the next update. */
            mLastPhase[k] = phase;

            /* Normalize from pi, and wrap the delta between -1 and +1. */
            tmp *= al::numbers::inv_pi;
            int qpd{double2int(tmp)};
            tmp -= qpd + (qpd%2);

            /* Get deviation from bin frequency (-0.5 to +0.5), and account for
             * oversampling.
             */
            tmp *= 0.5 * OversampleFactor;

            /* Compute the k-th partials' frequency bin target and store the
             * magnitude and frequency bin in the analysis buffer. We don't
             * need the "true frequency" since it's a linear relationship with
             * the bin.
             */
            mAnalysisBuffer[k].Magnitude = magnitude;
            mAnalysisBuffer[k].FreqBin = static_cast<double>(k) + tmp;
        }

        /* Shift the frequency bins according to the pitch adjustment,
         * accumulating the magnitudes of overlapping frequency bins.
         */
        std::fill(mSynthesisBuffer.begin(), mSynthesisBuffer.end(), FrequencyBin{});

        constexpr size_t bin_limit{((StftHalfSize+1)<<MixerFracBits) - MixerFracHalf - 1};
        const size_t bin_count{minz(StftHalfSize+1, bin_limit/mPitchShiftI + 1)};
        for(size_t k{0u};k < bin_count;k++)
        {
            const size_t j{(k*mPitchShiftI + MixerFracHalf) >> MixerFracBits};

            /* If more than two bins end up together, use the target frequency
             * bin for the one with the dominant magnitude. There might be a
             * better way to handle this, but it's better than last-index-wins.
             */
            if(mAnalysisBuffer[k].Magnitude > mSynthesisBuffer[j].Magnitude)
                mSynthesisBuffer[j].FreqBin = mAnalysisBuffer[k].FreqBin * mPitchShift;
            mSynthesisBuffer[j].Magnitude += mAnalysisBuffer[k].Magnitude;
        }

        /* Reconstruct the frequency-domain signal from the adjusted frequency
         * bins.
         */
        for(size_t k{0u};k < StftHalfSize+1;k++)
        {
            /* Calculate the actual delta phase for this bin's target frequency
             * bin, and accumulate it to get the actual bin phase.
             */
            double tmp{mSumPhase[k] + mSynthesisBuffer[k].FreqBin*expected_cycles};

            /* Wrap between -pi and +pi for the sum. If mSumPhase is left to
             * grow indefinitely, it will lose precision and produce less exact
             * phase over time.
             */
            int qpd{double2int(tmp * al::numbers::inv_pi)};
            tmp -= al::numbers::pi * (qpd + (qpd%2));
            mSumPhase[k] = tmp;

            mFftBuffer[k] = std::polar(mSynthesisBuffer[k].Magnitude, mSumPhase[k]);
        }
        for(size_t k{StftHalfSize+1};k < StftSize;++k)
            mFftBuffer[k] = std::conj(mFftBuffer[StftSize-k]);

        /* Apply an inverse FFT to get the time-domain signal, and accumulate
         * for the output with windowing.
         */
        inverse_fft(al::as_span(mFftBuffer));

        static constexpr double scale{4.0 / OversampleFactor / StftSize};
        for(size_t dst{mPos}, k{0u};dst < StftSize;++dst,++k)
            mOutputAccum[dst] += HannWindow[k]*mFftBuffer[k].real() * scale;
        for(size_t dst{0u}, k{StftSize-mPos};dst < mPos;++dst,++k)
            mOutputAccum[dst] += HannWindow[k]*mFftBuffer[k].real() * scale;

        /* Copy out the accumulated result, then clear for the next iteration. */
        std::copy_n(mOutputAccum.begin() + mPos, StftStep, mFIFO.begin() + mPos);
        std::fill_n(mOutputAccum.begin() + mPos, StftStep, 0.0);
    }

    /* Now, mix the processed sound data to the output. */
    MixSamples({mBufferOut.data(), samplesToDo}, samplesOut, mCurrentGains, mTargetGains,
        maxz(samplesToDo, 512), 0);
}


struct PshifterStateFactory final : public EffectStateFactory {
    al::intrusive_ptr<EffectState> create() override
    { return al::intrusive_ptr<EffectState>{new PshifterState{}}; }
};

} // namespace

EffectStateFactory *PshifterStateFactory_getFactory()
{
    static PshifterStateFactory PshifterFactory{};
    return &PshifterFactory;
}
