#include "sbmusic_player.hpp"

#if defined(TL_HAVE_OPUS)

#include "http_win.hpp"
#include "sbmusic_api.hpp" // stream_url, auth_header

#include <SDL.h>
#include <opusfile.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace tl::sbmusic {

namespace {

constexpr int kRate = 48000; // Opus decode toujours a 48 kHz
constexpr int kChannels = 2;

std::mutex g_m; // protege g_of / g_data / g_dev (partage avec le callback audio)
OggOpusFile* g_of = nullptr;
std::vector<unsigned char> g_data; // op_open_memory NE COPIE PAS : garder en vie
SDL_AudioDeviceID g_dev = 0;
bool g_audioSub = false;

std::atomic<bool> g_paused{true};
std::atomic<float> g_volume{0.8f};
std::atomic<long long> g_posSamples{0}; // position (echantillons/canal)
long long g_totalSamples = 0;

// Callback audio SDL (tourne sur le fil audio). Decode a la volee depuis le
// titre en memoire. S16 stereo, 48 kHz.
void audio_cb(void*, Uint8* stream, int len) {
    std::memset(stream, 0, static_cast<size_t>(len));
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_of || g_paused.load(std::memory_order_relaxed)) return;

    auto* out = reinterpret_cast<opus_int16*>(stream);
    const int framesWanted = len / (kChannels * static_cast<int>(sizeof(opus_int16)));
    int filled = 0;
    while (filled < framesWanted) {
        const int got =
            op_read_stereo(g_of, out + filled * kChannels,
                           (framesWanted - filled) * kChannels);
        if (got <= 0) break; // fin du titre ou erreur de decodage
        filled += got;
    }
    g_posSamples.fetch_add(filled, std::memory_order_relaxed);

    const float v = g_volume.load(std::memory_order_relaxed);
    if (v < 0.999f) {
        const int total = filled * kChannels;
        for (int i = 0; i < total; ++i)
            out[i] = static_cast<opus_int16>(out[i] * v);
    }
    if (filled == 0) g_paused.store(true, std::memory_order_relaxed); // fin
}

// Ouvre le peripherique audio une seule fois. Appele sous g_m.
bool ensure_device_locked() {
    if (g_dev) return true;
    if (!g_audioSub) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
        g_audioSub = true;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = kRate;
    want.format = AUDIO_S16SYS;
    want.channels = kChannels;
    want.samples = 2048;
    want.callback = audio_cb;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have,
                               SDL_AUDIO_ALLOW_FORMAT_CHANGE);
    return g_dev != 0;
}

} // namespace

bool can_play() { return true; }

bool play(const std::string& trackId, std::string* err) {
    const std::string url = stream_url(trackId);
    if (url.empty()) {
        if (err) *err = "titre invalide";
        return false;
    }
    const std::string hdr = auth_header();
    if (hdr.empty()) {
        if (err) *err = "non connecté à SB'Music";
        return false;
    }
    // Telechargement du flux entier (authentifie). Bloquant : worker attendu.
    auto r = http::get_response(url, hdr);
    if (!r || r->status != 200 || r->body.empty()) {
        if (err)
            *err = r ? ("échec du flux (HTTP " + std::to_string(r->status) + ")")
                     : "flux injoignable";
        return false;
    }

    std::vector<unsigned char> data(r->body.begin(), r->body.end());
    int oe = 0;
    OggOpusFile* of = op_open_memory(data.data(), data.size(), &oe);
    if (!of) {
        if (err) *err = "flux Opus illisible";
        return false;
    }
    const ogg_int64_t total = op_pcm_total(of, -1);

    std::lock_guard<std::mutex> lk(g_m);
    if (g_dev) SDL_PauseAudioDevice(g_dev, 1); // stoppe le callback le temps du swap
    if (g_of) op_free(g_of);
    g_data = std::move(data); // garde le buffer en vie pour op_read_stereo
    g_of = of;
    g_totalSamples = total > 0 ? total : 0;
    g_posSamples.store(0, std::memory_order_relaxed);
    g_paused.store(false, std::memory_order_relaxed);
    if (!ensure_device_locked()) {
        if (err) *err = "sortie audio indisponible";
        op_free(g_of);
        g_of = nullptr;
        return false;
    }
    SDL_PauseAudioDevice(g_dev, 0); // demarre la lecture
    return true;
}

void toggle_pause() {
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_of || !g_dev) return;
    const bool p = !g_paused.load(std::memory_order_relaxed);
    g_paused.store(p, std::memory_order_relaxed);
    SDL_PauseAudioDevice(g_dev, p ? 1 : 0);
}

void stop() {
    std::lock_guard<std::mutex> lk(g_m);
    if (g_dev) SDL_PauseAudioDevice(g_dev, 1);
    if (g_of) {
        op_free(g_of);
        g_of = nullptr;
    }
    g_data.clear();
    g_posSamples.store(0, std::memory_order_relaxed);
    g_totalSamples = 0;
    g_paused.store(true, std::memory_order_relaxed);
}

bool has_track() {
    std::lock_guard<std::mutex> lk(g_m);
    return g_of != nullptr;
}

bool is_playing() {
    std::lock_guard<std::mutex> lk(g_m);
    return g_of != nullptr && !g_paused.load(std::memory_order_relaxed);
}

double position_sec() {
    return static_cast<double>(g_posSamples.load(std::memory_order_relaxed)) / kRate;
}

double duration_sec() {
    std::lock_guard<std::mutex> lk(g_m);
    return static_cast<double>(g_totalSamples) / kRate;
}

void seek(double sec) {
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_of) return;
    const ogg_int64_t target = static_cast<ogg_int64_t>(sec * kRate);
    if (op_pcm_seek(g_of, target) == 0)
        g_posSamples.store(target, std::memory_order_relaxed);
}

void set_volume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_volume.store(v, std::memory_order_relaxed);
}

float volume() { return g_volume.load(std::memory_order_relaxed); }

void shutdown() {
    std::lock_guard<std::mutex> lk(g_m);
    if (g_dev) {
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    if (g_of) {
        op_free(g_of);
        g_of = nullptr;
    }
    g_data.clear();
}

} // namespace tl::sbmusic

#else // Pas de decodeur Opus : stubs.

namespace tl::sbmusic {
bool can_play() { return false; }
bool play(const std::string&, std::string* err) {
    if (err) *err = "lecteur audio indisponible dans cette version";
    return false;
}
void toggle_pause() {}
void stop() {}
bool has_track() { return false; }
bool is_playing() { return false; }
double position_sec() { return 0.0; }
double duration_sec() { return 0.0; }
void seek(double) {}
void set_volume(float) {}
float volume() { return 0.0f; }
void shutdown() {}
} // namespace tl::sbmusic

#endif
