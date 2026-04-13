// Minimal mono 32-bit float WAV loader (no external libraries).

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr uint16_t kWaveFormatPcm = 0x0001;
constexpr uint16_t kWaveFormatIeeeFloat = 0x0003;  // WAVE_FORMAT_IEEE_FLOAT

// Directory containing this executable (project folder when you build/run from there).
std::filesystem::path application_directory() {
    namespace fs = std::filesystem;
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return fs::current_path();
    }
    return fs::path(buf).parent_path();
#else
    return fs::current_path();
#endif
}

bool read_u32_le(std::istream& in, uint32_t& out) {
    unsigned char b[4];
    in.read(reinterpret_cast<char*>(b), 4);
    if (!in || in.gcount() != 4) {
        return false;
    }
    out = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
          (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

uint16_t u16_le(const unsigned char* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t u32_le(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// IEEE 754 little-endian float bits from WAV data.
float f32_le_from_bytes(const unsigned char* p) {
    uint32_t u = u32_le(p);
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

bool write_u16_le(std::ostream& out, uint16_t v) {
    const unsigned char b[2] = {static_cast<unsigned char>(v & 0xffU),
                                static_cast<unsigned char>((v >> 8) & 0xffU)};
    out.write(reinterpret_cast<const char*>(b), 2);
    return static_cast<bool>(out);
}

bool write_u32_le(std::ostream& out, uint32_t v) {
    const unsigned char b[4] = {
        static_cast<unsigned char>(v & 0xffU),
        static_cast<unsigned char>((v >> 8) & 0xffU),
        static_cast<unsigned char>((v >> 16) & 0xffU),
        static_cast<unsigned char>((v >> 24) & 0xffU)};
    out.write(reinterpret_cast<const char*>(b), 4);
    return static_cast<bool>(out);
}

}  // namespace

struct WavLoadResult {
    uint32_t sample_rate = 0;
    uint16_t num_channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t total_samples = 0;
    std::vector<float> samples;
};

struct PhaseVocoderFrame {
    std::vector<float> magnitude;
    std::vector<float> phase;
    std::vector<float> deltaPhase;
    std::vector<float> trueFreq;
};

struct SynthesizedPhaseFrame {
    std::vector<float> magnitude;
    std::vector<float> outputPhase;
};

bool load_mono_float_wav(const std::string& path, WavLoadResult& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "Could not open file: " + path;
        return false;
    }

    char riff[4];
    file.read(riff, 4);
    if (!file || std::memcmp(riff, "RIFF", 4) != 0) {
        error = "Not a RIFF file (expected 'RIFF').";
        return false;
    }

    uint32_t riff_chunk_size = 0;
    if (!read_u32_le(file, riff_chunk_size)) {
        error = "Failed to read RIFF chunk size.";
        return false;
    }

    char wave[4];
    file.read(wave, 4);
    if (!file || std::memcmp(wave, "WAVE", 4) != 0) {
        error = "Not a WAVE file (expected 'WAVE' after RIFF size).";
        return false;
    }

    bool have_fmt = false;
    bool have_data = false;
    uint16_t audio_format = 0;
    uint32_t sample_rate = 0;
    uint16_t num_channels = 0;
    uint16_t bits_per_sample = 0;

    std::vector<unsigned char> pcm_bytes;

    while (true) {
        char id[4];
        file.read(id, 4);
        if (file.gcount() != 4) {
            break;
        }

        uint32_t chunk_size = 0;
        if (!read_u32_le(file, chunk_size)) {
            error = "Failed to read chunk size.";
            return false;
        }

        if (std::memcmp(id, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                error = "fmt chunk is too small (need at least 16 bytes).";
                return false;
            }
            std::vector<unsigned char> fmt(chunk_size);
            file.read(reinterpret_cast<char*>(fmt.data()), chunk_size);
            if (static_cast<std::streamsize>(file.gcount()) !=
                static_cast<std::streamsize>(chunk_size)) {
                error = "Unexpected end of file while reading fmt chunk.";
                return false;
            }

            audio_format = u16_le(fmt.data());
            num_channels = u16_le(fmt.data() + 2);
            sample_rate = u32_le(fmt.data() + 4);
            bits_per_sample = u16_le(fmt.data() + 14);
            have_fmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (have_data) {
                error = "Multiple data chunks are not supported.";
                return false;
            }
            have_data = true;
            pcm_bytes.resize(chunk_size);
            if (chunk_size > 0) {
                file.read(reinterpret_cast<char*>(pcm_bytes.data()), chunk_size);
                if (static_cast<uint32_t>(file.gcount()) != chunk_size) {
                    error = "Unexpected end of file while reading data chunk.";
                    return false;
                }
            }
        } else {
            if (chunk_size > 0) {
                file.seekg(static_cast<std::istream::off_type>(chunk_size), std::ios::cur);
                if (!file) {
                    error = "Failed to skip chunk.";
                    return false;
                }
            }
        }

        if (chunk_size % 2 == 1) {
            file.seekg(1, std::ios::cur);
        }
    }

    if (!have_fmt) {
        error = "Missing fmt chunk.";
        return false;
    }
    if (!have_data) {
        error = "Missing data chunk.";
        return false;
    }

    if (audio_format != kWaveFormatPcm && audio_format != kWaveFormatIeeeFloat) {
        error = "Unsupported WAV format tag " + std::to_string(audio_format) +
                " (need 16-bit PCM or 32-bit float).";
        return false;
    }
    if (num_channels != 1 && num_channels != 2) {
        error = "Unsupported channel count " + std::to_string(num_channels) + " (need mono or stereo).";
        return false;
    }
    if (audio_format == kWaveFormatPcm && bits_per_sample != 16) {
        error = "16-bit PCM WAV expected; got " + std::to_string(bits_per_sample) + " bits per sample.";
        return false;
    }
    if (audio_format == kWaveFormatIeeeFloat && bits_per_sample != 32) {
        error = "32-bit float WAV expected; got " + std::to_string(bits_per_sample) + " bits per sample.";
        return false;
    }

    const uint32_t bytes_per_frame = static_cast<uint32_t>(num_channels) *
                                     (static_cast<uint32_t>(bits_per_sample) / 8U);
    if (bytes_per_frame == 0 || pcm_bytes.size() % bytes_per_frame != 0) {
        error = "Data size does not align to frame size.";
        return false;
    }

    const uint32_t frame_count =
        static_cast<uint32_t>(pcm_bytes.size() / bytes_per_frame);

    out.sample_rate = sample_rate;
    out.num_channels = 1;
    out.bits_per_sample = 32;
    out.total_samples = frame_count;
    out.samples.resize(frame_count);

    const unsigned char* raw = pcm_bytes.data();

    if (audio_format == kWaveFormatPcm) {
        if (num_channels == 1) {
            for (uint32_t i = 0; i < frame_count; ++i) {
                const int16_t s =
                    static_cast<int16_t>(u16_le(raw + i * 2U));
                out.samples[i] = static_cast<float>(s) / 32768.0f;
            }
        } else {
            for (uint32_t i = 0; i < frame_count; ++i) {
                const int16_t L =
                    static_cast<int16_t>(u16_le(raw + i * 4U));
                const int16_t R =
                    static_cast<int16_t>(u16_le(raw + i * 4U + 2U));
                out.samples[i] =
                    0.5f * (static_cast<float>(L) / 32768.0f + static_cast<float>(R) / 32768.0f);
            }
        }
    } else {
        if (num_channels == 1) {
            for (uint32_t i = 0; i < frame_count; ++i) {
                out.samples[i] = f32_le_from_bytes(raw + i * 4U);
            }
        } else {
            for (uint32_t i = 0; i < frame_count; ++i) {
                const float L = f32_le_from_bytes(raw + i * 8U);
                const float R = f32_le_from_bytes(raw + i * 8U + 4U);
                out.samples[i] = 0.5f * (L + R);
            }
        }
    }

    return true;
}

bool writeMonoFloatWav(const std::string& path, const std::vector<float>& samples, uint32_t sampleRate) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    const uint64_t num_samples_u64 = samples.size();
    if (num_samples_u64 > static_cast<uint64_t>(UINT32_MAX) / 4U) {
        return false;
    }
    const uint32_t num_samples = static_cast<uint32_t>(num_samples_u64);
    const uint32_t data_bytes = num_samples * 4U;

    constexpr uint16_t kChannels = 1;
    constexpr uint16_t kBits = 32;
    constexpr uint16_t kAlign = kChannels * (kBits / 8U);
    const uint32_t byte_rate = sampleRate * kChannels * (kBits / 8U);

    const uint32_t fmt_chunk_data_bytes = 16U;
    const uint32_t riff_payload_bytes =
        4U + (8U + fmt_chunk_data_bytes) + (8U + data_bytes);

    if (!file.write("RIFF", 4)) {
        return false;
    }
    if (!write_u32_le(file, riff_payload_bytes)) {
        return false;
    }
    if (!file.write("WAVE", 4)) {
        return false;
    }
    if (!file.write("fmt ", 4)) {
        return false;
    }
    if (!write_u32_le(file, fmt_chunk_data_bytes)) {
        return false;
    }
    constexpr uint16_t kIeeeFloat = 0x0003;
    if (!write_u16_le(file, kIeeeFloat)) {
        return false;
    }
    if (!write_u16_le(file, kChannels)) {
        return false;
    }
    if (!write_u32_le(file, sampleRate)) {
        return false;
    }
    if (!write_u32_le(file, byte_rate)) {
        return false;
    }
    if (!write_u16_le(file, kAlign)) {
        return false;
    }
    if (!write_u16_le(file, kBits)) {
        return false;
    }
    if (!file.write("data", 4)) {
        return false;
    }
    if (!write_u32_le(file, data_bytes)) {
        return false;
    }

    for (uint32_t i = 0; i < num_samples; ++i) {
        uint32_t u = 0;
        std::memcpy(&u, &samples[i], sizeof(float));
        if (!write_u32_le(file, u)) {
            return false;
        }
    }
    return static_cast<bool>(file);
}

std::vector<float> createHannWindow(size_t size) {
    std::vector<float> w(size);
    if (size == 0) {
        return w;
    }
    if (size == 1) {
        w[0] = 1.0f;
        return w;
    }

    constexpr float kTwoPi = 6.283185307179586476925286766559f;
    const float denom = static_cast<float>(size - 1);
    for (size_t n = 0; n < size; ++n) {
        const float x =
            0.5f * (1.0f - std::cos(kTwoPi * static_cast<float>(n) / denom));
        w[n] = std::clamp(x, 0.0f, 1.0f);
    }
    return w;
}

namespace {

constexpr float kTwoPiF = 6.283185307179586476925286766559f;

bool is_power_of_two(size_t n) { return n > 0 && (n & (n - 1U)) == 0; }

// Cooley–Tukey radix-2 DIT FFT; N must be a power of 2 and >= 1.
std::vector<std::complex<float>> fft_radix2_recursive(std::vector<std::complex<float>> a) {
    const size_t n = a.size();
    if (n <= 1) {
        return a;
    }

    std::vector<std::complex<float>> even(n / 2);
    std::vector<std::complex<float>> odd(n / 2);
    for (size_t i = 0; i < n / 2; ++i) {
        even[i] = a[2 * i];
        odd[i] = a[2 * i + 1];
    }

    even = fft_radix2_recursive(std::move(even));
    odd = fft_radix2_recursive(std::move(odd));

    std::vector<std::complex<float>> y(n);
    for (size_t k = 0; k < n / 2; ++k) {
        const float angle = -kTwoPiF * static_cast<float>(k) / static_cast<float>(n);
        const std::complex<float> t =
            std::complex<float>(std::cos(angle), std::sin(angle)) * odd[k];
        y[k] = even[k] + t;
        y[k + n / 2] = even[k] - t;
    }
    return y;
}

// O(N^2) DFT for arbitrary N >= 1.
std::vector<std::complex<float>> dft_naive(const std::vector<std::complex<float>>& input) {
    const size_t n = input.size();
    std::vector<std::complex<float>> out(n);
    const float nf = static_cast<float>(n);
    for (size_t k = 0; k < n; ++k) {
        std::complex<float> sum{0.0f, 0.0f};
        for (size_t j = 0; j < n; ++j) {
            const float angle =
                -kTwoPiF * static_cast<float>(k) * static_cast<float>(j) / nf;
            sum += input[j] * std::complex<float>(std::cos(angle), std::sin(angle));
        }
        out[k] = sum;
    }
    return out;
}

// O(N^2) inverse DFT for arbitrary N >= 1.
std::vector<std::complex<float>> idft_naive(const std::vector<std::complex<float>>& input) {
    const size_t n = input.size();
    std::vector<std::complex<float>> out(n);
    const float nf = static_cast<float>(n);
    const float scale = 1.0f / nf;
    for (size_t j = 0; j < n; ++j) {
        std::complex<float> sum{0.0f, 0.0f};
        for (size_t k = 0; k < n; ++k) {
            const float angle =
                kTwoPiF * static_cast<float>(k) * static_cast<float>(j) / nf;
            sum += input[k] * std::complex<float>(std::cos(angle), std::sin(angle));
        }
        out[j] = sum * scale;
    }
    return out;
}

}  // namespace

std::vector<std::complex<float>> fft(const std::vector<std::complex<float>>& input) {
    const size_t n = input.size();
    if (n == 0) {
        return {};
    }
    if (is_power_of_two(n)) {
        return fft_radix2_recursive(input);
    }
    return dft_naive(input);
}

std::vector<std::complex<float>> ifft(const std::vector<std::complex<float>>& input) {
    const size_t n = input.size();
    if (n == 0) {
        return {};
    }
    if (is_power_of_two(n)) {
        std::vector<std::complex<float>> conj_in(n);
        for (size_t i = 0; i < n; ++i) {
            conj_in[i] = std::conj(input[i]);
        }
        std::vector<std::complex<float>> tmp = fft_radix2_recursive(std::move(conj_in));
        std::vector<std::complex<float>> out(n);
        const float scale = 1.0f / static_cast<float>(n);
        for (size_t i = 0; i < n; ++i) {
            out[i] = std::conj(tmp[i]) * scale;
        }
        return out;
    }
    return idft_naive(input);
}

std::vector<std::vector<std::complex<float>>> stft(const std::vector<float>& signal,
                                                   size_t frameSize,
                                                   size_t hopSize) {
    std::vector<std::vector<std::complex<float>>> frames_out;
    if (frameSize == 0 || hopSize == 0 || signal.size() < frameSize) {
        return frames_out;
    }

    const std::vector<float> window = createHannWindow(frameSize);
    for (size_t start = 0; start + frameSize <= signal.size(); start += hopSize) {
        std::vector<std::complex<float>> frame_complex(frameSize);
        for (size_t i = 0; i < frameSize; ++i) {
            frame_complex[i] = std::complex<float>(signal[start + i] * window[i], 0.0f);
        }
        frames_out.push_back(fft(frame_complex));
    }
    return frames_out;
}

constexpr float kTransientEnergyThreshold = 1.5f;

std::vector<bool> detectTransientFrames(
    const std::vector<std::vector<std::complex<float>>>& spectra) {
    std::vector<bool> flags;
    if (spectra.empty()) {
        return flags;
    }

    const size_t num_frames = spectra.size();
    flags.resize(num_frames, false);

    std::vector<float> energy(num_frames);
    for (size_t i = 0; i < num_frames; ++i) {
        double sum_sq_mag = 0.0;
        for (const std::complex<float>& c : spectra[i]) {
            const float m = std::abs(c);
            sum_sq_mag += static_cast<double>(m) * static_cast<double>(m);
        }
        energy[i] = static_cast<float>(sum_sq_mag);
    }

    for (size_t i = 1; i < num_frames; ++i) {
        if (energy[i] > energy[i - 1] * kTransientEnergyThreshold) {
            flags[i] = true;
        }
    }
    return flags;
}

std::vector<float> istft(const std::vector<std::vector<std::complex<float>>>& spectra,
                         size_t frameSize,
                         size_t hopSize) {
    if (spectra.empty() || frameSize == 0 || hopSize == 0) {
        return {};
    }
    for (const auto& frame : spectra) {
        if (frame.size() != frameSize) {
            return {};
        }
    }

    const size_t num_frames = spectra.size();
    const size_t out_len = (num_frames - 1U) * hopSize + frameSize;
    const std::vector<float> window = createHannWindow(frameSize);

    std::vector<float> output(out_len, 0.0f);
    std::vector<float> den(out_len, 0.0f);

    for (size_t k = 0; k < num_frames; ++k) {
        const std::vector<std::complex<float>> time_domain = ifft(spectra[k]);
        for (size_t i = 0; i < frameSize; ++i) {
            const size_t n = k * hopSize + i;
            const float w = window[i];
            output[n] += time_domain[i].real() * w;
            den[n] += w * w;
        }
    }

    constexpr float kEps = 1e-9f;
    for (size_t n = 0; n < out_len; ++n) {
        if (den[n] > kEps) {
            output[n] /= den[n];
        } else {
            output[n] = 0.0f;
        }
    }
    return output;
}

namespace {

float wrap_pi(float x) { return std::atan2(std::sin(x), std::cos(x)); }

}  // namespace

std::vector<PhaseVocoderFrame> analyzePhaseVocoder(
    const std::vector<std::vector<std::complex<float>>>& spectra,
    size_t frameSize,
    size_t hopSize,
    float sampleRate) {
    (void)sampleRate;

    std::vector<PhaseVocoderFrame> frames;
    if (spectra.empty() || frameSize == 0 || hopSize == 0) {
        return frames;
    }
    for (const auto& spec : spectra) {
        if (spec.size() != frameSize) {
            return {};
        }
    }

    const size_t num_bins = frameSize;
    const float hop_f = static_cast<float>(hopSize);
    const float n_f = static_cast<float>(frameSize);
    constexpr float kTwoPi = 6.283185307179586476925286766559f;

    std::vector<float> prev_phase(num_bins, 0.0f);

    for (size_t fi = 0; fi < spectra.size(); ++fi) {
        PhaseVocoderFrame row;
        row.magnitude.resize(num_bins);
        row.phase.resize(num_bins);
        row.deltaPhase.resize(num_bins);
        row.trueFreq.resize(num_bins);

        for (size_t bin = 0; bin < num_bins; ++bin) {
            const std::complex<float> c = spectra[fi][bin];
            row.magnitude[bin] = std::abs(c);
            row.phase[bin] = std::atan2(c.imag(), c.real());

            const float expected =
                kTwoPi * static_cast<float>(bin) * hop_f / n_f;

            if (fi == 0) {
                row.deltaPhase[bin] = 0.0f;
                row.trueFreq[bin] = kTwoPi * static_cast<float>(bin) / n_f;
            } else {
                const float phase_diff = wrap_pi(row.phase[bin] - prev_phase[bin]);
                const float residual = phase_diff - expected;
                row.deltaPhase[bin] = wrap_pi(residual);
                row.trueFreq[bin] =
                    (kTwoPi * static_cast<float>(bin) / n_f) + row.deltaPhase[bin] / hop_f;
            }
        }

        prev_phase = row.phase;
        frames.push_back(std::move(row));
    }
    return frames;
}

std::vector<SynthesizedPhaseFrame> synthesizePhaseVocoder(
    const std::vector<PhaseVocoderFrame>& analysisFrames,
    size_t frameSize,
    size_t synthesisHop) {
    std::vector<SynthesizedPhaseFrame> out;
    if (analysisFrames.empty() || frameSize == 0 || synthesisHop == 0) {
        return out;
    }

    for (const auto& a : analysisFrames) {
        if (a.magnitude.size() != frameSize || a.phase.size() != frameSize ||
            a.trueFreq.size() != frameSize) {
            return {};
        }
    }

    const size_t num_bins = frameSize;
    const float hop_f = static_cast<float>(synthesisHop);

    out.reserve(analysisFrames.size());

    for (size_t fi = 0; fi < analysisFrames.size(); ++fi) {
        SynthesizedPhaseFrame row;
        row.magnitude = analysisFrames[fi].magnitude;
        row.outputPhase.resize(num_bins);

        if (fi == 0) {
            row.outputPhase = analysisFrames[fi].phase;
        } else {
            const SynthesizedPhaseFrame& prev = out[fi - 1];
            for (size_t k = 0; k < num_bins; ++k) {
                row.outputPhase[k] =
                    prev.outputPhase[k] + analysisFrames[fi].trueFreq[k] * hop_f;
            }
        }
        out.push_back(std::move(row));
    }
    return out;
}

std::vector<std::vector<std::complex<float>>> rebuildSpectra(
    const std::vector<SynthesizedPhaseFrame>& synthFrames) {
    std::vector<std::vector<std::complex<float>>> spectra;
    spectra.reserve(synthFrames.size());

    for (const auto& fr : synthFrames) {
        const size_t n = fr.magnitude.size();
        std::vector<std::complex<float>> spec;
        spec.reserve(n);
        for (size_t k = 0; k < n; ++k) {
            const float m = fr.magnitude[k];
            const float p = fr.outputPhase[k];
            spec.emplace_back(m * std::cos(p), m * std::sin(p));
        }
        spectra.push_back(std::move(spec));
    }
    return spectra;
}

std::vector<float> phaseVocoderTimeStretch(const std::vector<float>& signal,
                                           size_t frameSize,
                                           size_t analysisHop,
                                           float stretchRatio) {
    if (signal.empty() || frameSize == 0 || analysisHop == 0 || stretchRatio <= 0.0f) {
        return {};
    }

    const double synthesis_hop_d =
        std::round(static_cast<double>(analysisHop) * static_cast<double>(stretchRatio));
    const size_t synthesisHop =
        static_cast<size_t>(std::max(1.0, synthesis_hop_d));

    const std::vector<std::vector<std::complex<float>>> stft_frames =
        stft(signal, frameSize, analysisHop);
    if (stft_frames.empty()) {
        return {};
    }

    const std::vector<PhaseVocoderFrame> pv_frames =
        analyzePhaseVocoder(stft_frames, frameSize, analysisHop, 44100.0f);
    if (pv_frames.empty()) {
        return {};
    }

    const std::vector<SynthesizedPhaseFrame> synth_frames =
        synthesizePhaseVocoder(pv_frames, frameSize, synthesisHop);
    if (synth_frames.empty()) {
        return {};
    }

    const std::vector<std::vector<std::complex<float>>> rebuilt = rebuildSpectra(synth_frames);
    return istft(rebuilt, frameSize, synthesisHop);
}

float signalRms(const std::vector<float>& x) {
    if (x.empty()) {
        return 0.0f;
    }
    double sum_sq = 0.0;
    for (float v : x) {
        const double d = static_cast<double>(v);
        sum_sq += d * d;
    }
    return static_cast<float>(std::sqrt(sum_sq / static_cast<double>(x.size())));
}

float signalMeanAbs(const std::vector<float>& x) {
    if (x.empty()) {
        return 0.0f;
    }
    double sum = 0.0;
    for (float v : x) {
        sum += std::abs(static_cast<double>(v));
    }
    return static_cast<float>(sum / static_cast<double>(x.size()));
}

float meanAbsoluteErrorCommon(const std::vector<float>& a, const std::vector<float>& b) {
    const size_t n = std::min(a.size(), b.size());
    if (n == 0) {
        return 0.0f;
    }
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sum += std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
    }
    return static_cast<float>(sum / static_cast<double>(n));
}

double durationRatio(size_t output_samples, size_t input_samples) {
    if (input_samples == 0) {
        return 0.0;
    }
    return static_cast<double>(output_samples) / static_cast<double>(input_samples);
}

void printPhaseVocoderStretchQualityReport(const std::vector<float>& original,
                                           const std::vector<float>& stretched) {
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Phase vocoder time-stretch quality:\n";
    std::cout << "  original_sample_count " << original.size() << '\n';
    std::cout << "  stretched_sample_count " << stretched.size() << '\n';
    std::cout << "  original_rms " << signalRms(original) << '\n';
    std::cout << "  stretched_rms " << signalRms(stretched) << '\n';
    std::cout << "  original_mean_abs " << signalMeanAbs(original) << '\n';
    std::cout << "  stretched_mean_abs " << signalMeanAbs(stretched) << '\n';
    std::cout << "  duration_ratio " << durationRatio(stretched.size(), original.size()) << '\n';
    std::cout << "  mean_abs_error_common_region " << meanAbsoluteErrorCommon(original, stretched)
              << '\n';
}

int main() {
    namespace fs = std::filesystem;
    const fs::path app_dir = application_directory();
    const std::string path = (app_dir / "mi_audio_original.wav").string();
    const std::string out_stretched_path = (app_dir / "output_stretched.wav").string();
    WavLoadResult result;
    std::string err;
    if (!load_mono_float_wav(path, result, err)) {
        std::cerr << "Error: " << err << '\n';
        return 1;
    }

    std::cout << "Sample rate: " << result.sample_rate << " Hz\n";
    std::cout << "Total samples: " << result.total_samples << '\n';

    const uint32_t n = std::min<uint32_t>(10, result.total_samples);
    std::cout << "First " << n << " sample value(s):\n";
    for (uint32_t i = 0; i < n; ++i) {
        std::cout << "  [" << i << "] = " << result.samples[i] << '\n';
    }

    constexpr size_t kTsFrame = 1024;
    constexpr size_t kTsAnalysisHop = 512;
    constexpr float kTsRatio = 1.1f;
    const std::vector<float> stretched =
        phaseVocoderTimeStretch(result.samples, kTsFrame, kTsAnalysisHop, kTsRatio);
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Time stretch parameters: frameSize=" << kTsFrame << " analysisHop=" << kTsAnalysisHop
              << " stretchRatio=" << kTsRatio << '\n';
    printPhaseVocoderStretchQualityReport(result.samples, stretched);
    {
        const size_t show_ts = std::min<size_t>(10, stretched.size());
        std::cout << "  first " << show_ts << " stretched samples:\n   ";
        for (size_t i = 0; i < show_ts; ++i) {
            std::cout << stretched[i] << (i + 1 < show_ts ? " " : "");
        }
        std::cout << '\n';
    }
    if (!writeMonoFloatWav(out_stretched_path, stretched, result.sample_rate)) {
        std::cerr << "Error: could not write " << out_stretched_path << '\n';
    } else {
        std::cout << "  wrote " << out_stretched_path << '\n';
    }

    constexpr size_t kStftFrame = 1024;
    constexpr size_t kStftHop = 512;
    const std::vector<std::vector<std::complex<float>>> stft_frames =
        stft(result.samples, kStftFrame, kStftHop);
    std::cout << "STFT (frameSize=" << kStftFrame << ", hopSize=" << kStftHop << "):\n";
    std::cout << "  number of frames: " << stft_frames.size() << '\n';
    if (!stft_frames.empty()) {
        std::cout << "  complex bins per frame: " << stft_frames[0].size() << '\n';
    }

    const std::vector<bool> transient_flags = detectTransientFrames(stft_frames);
    size_t transient_count = 0;
    for (bool t : transient_flags) {
        if (t) {
            ++transient_count;
        }
    }
    std::cout << "Transient detection (energy ratio threshold " << kTransientEnergyThreshold
              << "):\n";
    std::cout << "  total_frames " << transient_flags.size() << '\n';
    std::cout << "  transient_frames " << transient_count << '\n';
    std::cout << "  first_30_flags ";
    const size_t show_tr = std::min<size_t>(30, transient_flags.size());
    for (size_t i = 0; i < show_tr; ++i) {
        std::cout << (transient_flags[i] ? 1 : 0) << (i + 1 < show_tr ? " " : "");
    }
    std::cout << '\n';

    const std::vector<PhaseVocoderFrame> pv_frames = analyzePhaseVocoder(
        stft_frames, kStftFrame, kStftHop, static_cast<float>(result.sample_rate));
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Phase vocoder (frame 1, bins 0..9):\n";
    if (pv_frames.size() > 1) {
        const PhaseVocoderFrame& f1 = pv_frames[1];
        const size_t bins_show = std::min<size_t>(10, f1.magnitude.size());
        for (size_t b = 0; b < bins_show; ++b) {
            std::cout << "  bin " << b << "  mag=" << f1.magnitude[b] << "  phase=" << f1.phase[b]
                      << "  deltaPhase=" << f1.deltaPhase[b] << "  trueFreq=" << f1.trueFreq[b]
                      << '\n';
        }
    } else {
        std::cout << "  (not enough frames for frame 1)\n";
    }

    constexpr size_t kSynthHop = 512;
    const std::vector<SynthesizedPhaseFrame> synth_frames =
        synthesizePhaseVocoder(pv_frames, kStftFrame, kSynthHop);
    const std::vector<std::vector<std::complex<float>>> rebuilt_spectra =
        rebuildSpectra(synth_frames);
    std::cout << "Phase vocoder synthesis (frame 1, bins 0..9), synthesisHop=" << kSynthHop
              << ":\n";
    if (pv_frames.size() > 1 && synth_frames.size() > 1 && rebuilt_spectra.size() > 1) {
        const size_t bins_show = std::min<size_t>(10, pv_frames[1].phase.size());
        for (size_t b = 0; b < bins_show; ++b) {
            const std::complex<float> rb = rebuilt_spectra[1][b];
            std::cout << "  bin " << b << "  orig_phase=" << pv_frames[1].phase[b]
                      << "  synth_phase=" << synth_frames[1].outputPhase[b]
                      << "  mag=" << pv_frames[1].magnitude[b] << "  rebuilt=(" << rb.real() << ", "
                      << rb.imag() << ")\n";
        }
    } else {
        std::cout << "  (not enough frames for synthesis frame 1)\n";
    }

    const std::vector<float> istft_signal = istft(stft_frames, kStftFrame, kStftHop);
    std::cout << "ISTFT (same frame/hop as STFT):\n";
    std::cout << "  original sample count: " << result.samples.size() << '\n';
    std::cout << "  reconstructed sample count: " << istft_signal.size() << '\n';

    const size_t cmp_len = std::min(result.samples.size(), istft_signal.size());
    double sum_abs_err = 0.0;
    for (size_t i = 0; i < cmp_len; ++i) {
        sum_abs_err += std::abs(static_cast<double>(result.samples[i] - istft_signal[i]));
    }
    const double mean_abs_err = (cmp_len > 0) ? (sum_abs_err / static_cast<double>(cmp_len)) : 0.0;

    std::cout << std::fixed << std::setprecision(6);
    const size_t show = std::min<size_t>(10, cmp_len);
    std::cout << "  first " << show << " original samples:\n   ";
    for (size_t i = 0; i < show; ++i) {
        std::cout << result.samples[i] << (i + 1 < show ? " " : "");
    }
    std::cout << "\n  first " << show << " reconstructed samples:\n   ";
    for (size_t i = 0; i < show; ++i) {
        std::cout << istft_signal[i] << (i + 1 < show ? " " : "");
    }
    std::cout << "\n  mean absolute error (overlapping region, " << cmp_len << " samples): "
              << mean_abs_err << '\n';

    const std::vector<float> hann = createHannWindow(16);
    std::cout << "Hann window (size 16):\n";
    for (size_t i = 0; i < hann.size(); ++i) {
        std::cout << "  [" << i << "] = " << hann[i] << '\n';
    }

    std::cout << "FFT / IFFT test (N = 8)\n";
    std::cout << "  fft(empty) -> " << fft({}).size() << " coefficients\n";

    const std::vector<float> impulse = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<std::complex<float>> x_complex;
    x_complex.reserve(impulse.size());
    for (float v : impulse) {
        x_complex.emplace_back(v, 0.0f);
    }
    const std::vector<std::complex<float>> spectrum = fft(x_complex);
    const std::vector<std::complex<float>> reconstructed = ifft(spectrum);

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Original:\n  ";
    for (size_t i = 0; i < impulse.size(); ++i) {
        std::cout << impulse[i] << (i + 1 < impulse.size() ? " " : "");
    }
    std::cout << "\nReconstructed (real part):\n  ";
    for (size_t i = 0; i < reconstructed.size(); ++i) {
        std::cout << reconstructed[i].real() << (i + 1 < reconstructed.size() ? " " : "");
    }
    std::cout << "\nReconstructed (imag part; expect ~0):\n  ";
    for (size_t i = 0; i < reconstructed.size(); ++i) {
        std::cout << reconstructed[i].imag() << (i + 1 < reconstructed.size() ? " " : "");
    }
    std::cout << '\n';

    return 0;
}
