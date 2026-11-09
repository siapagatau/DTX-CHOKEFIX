#include "PadEngine.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>

namespace {
inline int64_t steadyNowNs() {
    return (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

PadEngine::PadEngine() {
    // std::atomic gak bisa di-brace-init ke nilai tertentu langsung di dalam
    // std::array (cuma zero-init) -> set manual ke 1.0f (volume penuh, gak ngubah
    // apa-apa) di sini, sekali pas engine dibikin.
    for (auto &v : padVolume_) v.store(1.0f);
    // padTune_ (0 semitone) sudah bener lewat zero-init
    // bawaan std::array<std::atomic<float>>, jadi gak perlu loop init di sini.

    // padChoke_ defaultnya AKTIF (true) buat semua pad - beda dari padTune_
    // di atas, di sini zero-init (false) itu JUSTRU KEBALIKAN dari default yang kita
    // mau, jadi wajib di-set manual satu-satu di sini.
    for (auto &c : padChoke_) c.store(true);

    // padLoopTempo_ defaultnya 1.0 (kecepatan/pitch asli, gak ngubah apa-apa) buat
    // semua pad - sama alasan kenapa padChoke_ di atas harus di-set manual: zero-init
    // bawaan std::atomic array (0.0) JUSTRU nilai yang salah/berbahaya di sini (lihat
    // catatan di deklarasi padLoopTempo_ pada PadEngine.h).
    for (auto &t : padLoopTempo_) t.store(1.0f);

}

bool PadEngine::loadMusic(const int16_t* pcm, int numFrames, int channels) {
    if (pcm == nullptr || numFrames <= 0) return false;

    // Lagu panjang/berat butuh alokasi vector yang gede (lihat catatan di MusicTrack
    // pada PadEngine.h). Kalau device gak punya memori cukup, std::vector::resize()
    // ngelempar std::bad_alloc/std::length_error -> kalau gak ditangkep di sini,
    // exception itu bakal nembus JNI boundary dan bikin seluruh aplikasi force close.
    // Makanya di-try/catch: gagal alokasi cuma bikin fungsi ini return false (lagu
    // lama yang lagi keputer, kalau ada, dibiarin gak keganggu), bukan crash total.
    try {
        MusicTrack track;
        track.totalFrames = numFrames;
        track.data.resize((size_t)numFrames * 2); // selalu disimpen stereo, mono di-duplikat ke L/R
        for (int i = 0; i < numFrames; i++) {
            int16_t l, r;
            if (channels <= 1) {
                l = r = pcm[i];
            } else {
                l = pcm[(size_t)i * channels + 0];
                r = pcm[(size_t)i * channels + 1];
            }
            track.data[(size_t)i * 2 + 0] = l;
            track.data[(size_t)i * 2 + 1] = r;
        }

        std::lock_guard<std::mutex> lock(musicMutex_);
        music_ = std::move(track);
        musicTotalFrames_.store(music_.totalFrames);
        musicPosition_.store(0);
        musicPlaying_.store(false);
        return true;
    } catch (const std::exception&) {
        return false;
    } catch (...) {
        return false;
    }
}

void PadEngine::playMusic() {
    // Kalau lagu udah abis keputer sampe habis (render loop di bawah auto-stop
    // begitu posisi kepentok totalFrames, tapi posisinya sendiri gak direset),
    // posisi bacanya masih nyangkut persis di ujung akhir. Tanpa reset ini, klik
    // "play" lagi cuma nyalain flag musicPlaying_ doang -> frame render PERTAMA
    // langsung ketemu pos >= totalFrames lagi dan auto-stop instan, jadi keliatan
    // kayak macet/gak mau muter sama sekali. Fix-nya: begitu play dipanggil pas
    // posisi udah di ujung (atau lewat, buat jaga-jaga), balikin ke 0 dulu -> lagu
    // otomatis keputer ulang dari awal, konsisten sama ekspektasi tombol play.
    int64_t total = musicTotalFrames_.load();
    if (total > 0 && musicPosition_.load() >= total) {
        musicPosition_.store(0);
    }
    musicPlaying_.store(true);
}

void PadEngine::stopMusic() {
    musicPlaying_.store(false);
}

void PadEngine::seekMusic(double seconds) {
    int64_t frame = (int64_t)(seconds * kMusicSampleRate);
    int64_t total = musicTotalFrames_.load();
    if (frame < 0) frame = 0;
    if (frame > total) frame = total;
    musicPosition_.store(frame);
}

double PadEngine::getMusicPositionSeconds() const {
    return (double)musicPosition_.load() / kMusicSampleRate;
}

double PadEngine::getMusicDurationSeconds() const {
    return (double)musicTotalFrames_.load() / kMusicSampleRate;
}

bool PadEngine::isMusicPlaying() const {
    return musicPlaying_.load();
}

void PadEngine::setKendangVolume(float v) {
    kendangVolume_.store(v);
}

void PadEngine::setMusicVolume(float v) {
    musicVolume_.store(v);
}

void PadEngine::setPadVolume(int preset, int pad, float v) {
    int idx = indexOf(preset, pad);
    if (idx < 0) return;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.5f) v = 1.5f; // konsisten sama batas atas kendangVolume_/musicVolume_
    padVolume_[idx].store(v);
}

float PadEngine::getPadVolume(int preset, int pad) const {
    int idx = indexOf(preset, pad);
    if (idx < 0) return 1.0f;
    return padVolume_[idx].load();
}

void PadEngine::setPadTune(int preset, int pad, float semitones) {
    int idx = indexOf(preset, pad);
    if (idx < 0) return;
    if (semitones < -12.0f) semitones = -12.0f;
    if (semitones > 12.0f) semitones = 12.0f;
    padTune_[idx].store(semitones);
}

float PadEngine::getPadTune(int preset, int pad) const {
    int idx = indexOf(preset, pad);
    if (idx < 0) return 0.0f;
    return padTune_[idx].load();
}

void PadEngine::setPadChoke(int preset, int pad, bool enabled) {
    int idx = indexOf(preset, pad);
    if (idx < 0) return;
    padChoke_[idx].store(enabled);
}

bool PadEngine::getPadChoke(int preset, int pad) const {
    int idx = indexOf(preset, pad);
    if (idx < 0) return true; // default aktif kalau idx invalid
    return padChoke_[idx].load();
}

void PadEngine::setPadCrossChokeEnabled(int preset, int pad, bool enabled) {
    int idx = indexOf(preset, pad);
    if (idx < 0) return;
    crossChokeEnabled_[idx].store(enabled);
}

bool PadEngine::getPadCrossChokeEnabled(int preset, int pad) const {
    int idx = indexOf(preset, pad);
    if (idx < 0) return false;
    return crossChokeEnabled_[idx].load();
}

void PadEngine::setPadCrossChokeMask(int preset, int pad, int mask) {
    int idx = indexOf(preset, pad);
    if (idx < 0) return;
    crossChokeMask_[idx].store((uint16_t)(mask & 0xFFFF));
}

int PadEngine::getPadCrossChokeMask(int preset, int pad) const {
    int idx = indexOf(preset, pad);
    if (idx < 0) return 0;
    return (int)crossChokeMask_[idx].load();
}

void PadEngine::setPadLoop(int preset, int pad, bool enabled) {
    int idx = indexOf(preset, pad);
    if (idx < 0) return;
    padLoop_[idx].store(enabled);
    // SENGAJA gak nyentuh voice yang lagi aktif looping di sini walau enabled
    // di-set false: kalau lagi muter, ketukan berikutnya di pad itu tetap yang
    // bakal menghentikannya (lihat pengecekan voice.loop di onAudioReady, yang
    // gak bergantung ke padLoop_ terkini) - biar user yang matiin toggle ini
    // pas lagi muter gak kaget suaranya keputus mendadak tanpa diketuk.
}

bool PadEngine::getPadLoop(int preset, int pad) const {
    int idx = indexOf(preset, pad);
    if (idx < 0) return false;
    return padLoop_[idx].load();
}

void PadEngine::setPadLoopTempo(int preset, int pad, float rate) {
    int idx = indexOf(preset, pad);
    if (idx < 0) return;
    if (rate < 0.5f) rate = 0.5f;
    if (rate > 2.0f) rate = 2.0f;
    padLoopTempo_[idx].store(rate);
}

float PadEngine::getPadLoopTempo(int preset, int pad) const {
    int idx = indexOf(preset, pad);
    if (idx < 0) return 1.0f;
    return padLoopTempo_[idx].load();
}

bool PadEngine::isPadLooping(int preset, int pad) const {
    int idx = indexOf(preset, pad);
    if (idx < 0) return false;
    // LOCK-FREE. Dulu fungsi ini ngambil samplesMutex_ (mutex yang sama dengan yang
    // dipegang audio thread selama onAudioReady). Karena audio thread cuma pakai
    // try_lock dan LANGSUNG BUANG 1 buffer kalau gagal, thread UI yang kebetulan lagi
    // megang lock ini (atau ke-preempt pas megang) bikin audio putus sesaat - dan
    // fungsi ini dipanggil terus-terusan dari polling UI justru pas user lagi
    // ngetuk cepat. Sekarang cukup baca atomic loopPad per voice (ditulis audio
    // thread pas voice dibikin loop / di-fade-out), jadi gak pernah ngeblok audio.
    for (const auto &v : voices_) {
        if (v.active.load() && v.loopPad.load() == idx) return true;
    }
    return false;
}

namespace {
// Kapasitas awal buffer rekam (dalam sample int16, ~340ms stereo 48kHz) - cukup buat
// interval drain normal (~10-30ms), jadi callback audio hampir gak pernah realloc.
constexpr size_t kRecordReserveSamples = 32768;
// Batas pengaman: kalau konsumen (encoder) macet/mati, buffer gak boleh membengkak
// tanpa batas sampai OOM. 120 detik stereo 48kHz ~ 23MB.
constexpr size_t kRecordMaxSamples = 48000u * 2u * 120u;

// Tukar isi buffer rekam dengan vector kosong yang kapasitasnya sudah disiapin DI LUAR
// lock (alokasi di thread pemanggil, bukan di audio thread). Critical section cuma swap.
std::vector<int16_t> swapOutRecordBuffer(std::mutex &m, std::vector<int16_t> &buf) {
    std::vector<int16_t> fresh;
    fresh.reserve(kRecordReserveSamples);
    std::lock_guard<std::mutex> lock(m);
    std::swap(fresh, buf);
    return fresh; // sekarang berisi data yang terkumpul
}
} // namespace

void PadEngine::startRecording() {
    std::lock_guard<std::mutex> lock(recordMutex_);
    recordBuffer_.clear();
    recordBuffer_.reserve(kRecordReserveSamples);
    recording_.store(true);
}

std::vector<int16_t> PadEngine::stopRecording() {
    recording_.store(false);
    // Ambil sisa PCM terakhir. recording_ sudah false, jadi setelah swap ini
    // onAudioReady tidak akan menambah data lagi -> hasilnya adalah "ekor" lengkap.
    return swapOutRecordBuffer(recordMutex_, recordBuffer_);
}

std::vector<int16_t> PadEngine::drainRecordBuffer() {
    // Sengaja TIDAK menyentuh recording_ (beda dari stopRecording()) - onAudioReady
    // harus tetap menulis ke recordBuffer_ karena rekaman masih berlangsung.
    return swapOutRecordBuffer(recordMutex_, recordBuffer_);
}

int PadEngine::indexOf(int preset, int pad) const {
    if (preset < 1 || preset > kMaxPresets || pad < 1 || pad > kMaxPads) return -1;
    return (preset - 1) * kMaxPads + (pad - 1);
}

bool PadEngine::openStream() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(oboe::AudioFormat::Float)
           ->setChannelCount(oboe::ChannelCount::Stereo)
           ->setSampleRate(48000)
           // FIX lagu/pad kedengeran LAMBAT di sebagian HP: tanpa ini Oboe boleh
           // membuka stream di rate native hardware (mis. 44100/ 32000 / 16000)
           // padahal semua PCM kita sudah 48kHz -> diputar lebih lambat & lebih
           // rendah nadanya. Dengan SRC aktif, Oboe yang meresample ke rate hardware.
           ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium)
           ->setCallback(this);

    // Coba sekali lagi (dengan jeda singkat) sebelum nyerah ke mode Shared.
    // Kenapa: kalau app baru aja ditutup terus dibuka lagi cepet, jalur audio
    // Exclusive dari sesi sebelumnya kadang belum sempurna dilepas sama sistem
    // Android (butuh puluhan ms). Percobaan PERTAMA bisa gagal gara-gara itu,
    // padahal beberapa saat kemudian jalurnya udah kosong lagi. Tanpa retry ini,
    // kegagalan sesaat itu langsung bikin turun ke mode Shared (lewat mixer
    // software biasa) -> itu sebabnya sebelumnya kerasa beda tiap buka ulang:
    // volume dikit lebih keras + delay dikit lebih kerasa, padahal cuma
    // beda jalur audio doang, bukan beda volume yang di-set aplikasi.
    oboe::Result result = builder.openStream(stream_);
    if (result != oboe::Result::OK) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        result = builder.openStream(stream_);
    }
    if (result != oboe::Result::OK) {
        // fallback kalau device gak support exclusive low-latency stream
        builder.setSharingMode(oboe::SharingMode::Shared);
        result = builder.openStream(stream_);
        if (result != oboe::Result::OK) return false;
    }
    stream_->requestStart();
    return true;
}

bool PadEngine::start() {
    if (stream_) return true; // stream udah kebuka -> gak perlu buka lagi (idempoten,
                               // aman dipanggil berkali-kali dari onResume tanpa efek samping)
    return openStream();
}

void PadEngine::stop() {
    // Kalau ada proses reconnect yang lagi jalan, tunggu beres dulu biar
    // gak balapan sama stream_ yang lagi kita reset di sini.
    if (restartThread_.joinable()) {
        restartThread_.join();
    }
    if (stream_) {
        stream_->requestStop();
        stream_->close();
        stream_.reset();
    }
}

void PadEngine::onErrorBeforeClose(oboe::AudioStream *stream, oboe::Result error) {
    // Ini yang biasa kepanggil pas mulai/berhenti screen recording: sistem
    // narik paksa stream low-latency eksklusif buat nge-route audio ke
    // capture pipeline si recorder. Gak perlu ngapa-ngapain di sini selain
    // logging kalau perlu, actual recovery-nya ada di onErrorAfterClose.
    (void)stream;
    (void)error;
}

void PadEngine::onErrorAfterClose(oboe::AudioStream *stream, oboe::Result error) {
    (void)stream;
    (void)error;

    // Hindari numpuk banyak thread restart kalau errornya beruntun.
    bool expected = false;
    if (!restarting_.compare_exchange_strong(expected, true)) return;

    // Buka stream baru di thread terpisah (bukan di audio callback thread)
    // biar gak nge-block dan recovery-nya cepet -- ini yang bikin delay
    // ilang begitu sistem selesai reroute audio buat screen recording.
    if (restartThread_.joinable()) restartThread_.join();
    restartThread_ = std::thread([this]() {
        // stream_ lama udah invalid & closed oleh Oboe sebelum callback ini,
        // jadi aman langsung buka yang baru menggantikannya.
        openStream();
        restarting_.store(false);
    });
}

void PadEngine::loadSample(int preset, int pad, const int16_t* pcm, int numFrames, int channels) {
    int idx = indexOf(preset, pad);
    if (idx < 0) return;

    SampleBuffer buf;
    buf.channels = channels;
    buf.data.resize((size_t)numFrames * channels);
    for (size_t i = 0; i < buf.data.size(); i++) {
        buf.data[i] = pcm[i] / 32768.0f;
    }

    // Cari titik loncat-balik loop: scan MUNDUR dari ekor sample nyari frame
    // TERAKHIR yang beneran ada bunyinya (di atas ambang hening), lalu kasih
    // sedikit padding (8ms) biar ekor natural decay/reverb transient terakhir
    // gak kepotong mepet pas loop lompat balik. Sample DATA-nya (buf.data di
    // atas) TIDAK diubah/dipotong sama sekali - ini murni nentuin di frame
    // keberapa WRAP loop terjadi (lihat pemakaiannya di onAudioReady), jadi
    // mode sekali-ketuk (non-loop) tetap muter penuh sample aslinya sampai
    // benar-benar habis, ori 100%, gak kepengaruh field ini sama sekali.
    const float kSilenceThreshold = 0.012f; // ~= 400/32768 di skala int16 lama
    const int kEdgePaddingFrames = (int)(48000 * 0.008); // sample udah 48kHz dari Kotlin
    int lastLoud = -1;
    for (int f = numFrames - 1; f >= 0; f--) {
        bool loud = false;
        size_t base = (size_t)f * channels;
        for (int ch = 0; ch < channels; ch++) {
            if (std::abs(buf.data[base + ch]) > kSilenceThreshold) { loud = true; break; }
        }
        if (loud) { lastLoud = f; break; }
    }
    // Semua frame di bawah ambang (sample hening total / rusak) -> jangan
    // diapa-apain, biarin wrap di panjang penuh seperti biasa.
    buf.loopEndFrame = (lastLoud < 0) ? numFrames : std::min(numFrames, lastLoud + 1 + kEdgePaddingFrames);

    std::lock_guard<std::mutex> lock(samplesMutex_);
    samples_[idx] = std::move(buf);
    // release: begitu ini kebaca true (acquire) di trigger()/audio thread, semua
    // tulisan ke samples_[idx] di atas dijamin udah kelihatan.
    sampleLoaded_[idx].store(true, std::memory_order_release);
}

void PadEngine::clearPad(int preset, int pad) {
    int idx = indexOf(preset, pad);
    if (idx < 0) return;

    // Matiin flag DULUAN (release) sebelum bongkar buffer-nya - trigger() baca
    // flag ini lock-free (acquire) tanpa samplesMutex_, jadi urutan ini yang
    // nyegah audio thread sempat kepilih trigger pad ini pas buffer-nya lagi
    // ditengah-tengah dikosongin di bawah.
    sampleLoaded_[idx].store(false, std::memory_order_release);

    std::lock_guard<std::mutex> lock(samplesMutex_);
    samples_[idx] = SampleBuffer(); // lepas memori PCM lama, balik ke kosong
}

void PadEngine::trigger(int preset, int pad, float velocity, int64_t eventTimeNs) {
    int idx = indexOf(preset, pad);
    if (idx < 0) return;
    // Tolak cepet kalau emang belum ada sample di pad ini (lock-free).
    if (!sampleLoaded_[idx].load(std::memory_order_acquire)) return;

    if (!(velocity > 0.0f)) velocity = 0.0f;   // juga nangkep NaN
    if (velocity > 1.0f) velocity = 1.0f;

    // Waktu ketukan: pakai waktu kejadian asli kalau masuk akal (tidak di masa depan
    // dan tidak lebih tua dari 250ms), kalau tidak pakai "sekarang".
    const int64_t now = steadyNowNs();
    int64_t t = now;
    if (eventTimeNs > 0 && eventTimeNs <= now && (now - eventTimeNs) < 250'000'000LL) {
        t = eventTimeNs;
    }

    // Wait-free (tanpa lock) & multi-producer: ambil tiket, tulis data slot, baru
    // publish lewat triggerSeq_ (release). Lihat catatan di PadEngine.h.
    const int64_t ticket = triggerWriteTicket_.fetch_add(1, std::memory_order_acq_rel);
    const int slot = (int)(ticket % kTriggerQueueSize);
    triggerQueue_[slot].store(idx, std::memory_order_relaxed);
    triggerVelQueue_[slot].store(velocity, std::memory_order_relaxed);
    triggerTimeQueue_[slot].store(t, std::memory_order_relaxed);
    triggerSeq_[slot].store(ticket + 1, std::memory_order_release);
}

oboe::DataCallbackResult PadEngine::onAudioReady(oboe::AudioStream *stream, void *audioData, int32_t numFrames) {
    float *out = static_cast<float*>(audioData);
    int outChannels = stream->getChannelCount();
    std::fill(out, out + (size_t)numFrames * outChannels, 0.0f);

    // try_lock, bukan lock_guard biasa: audio thread ini gak boleh sampe
    // keblokir nunggu loadSample() di thread lain. Kalau lagi kebentur (jarang -
    // cuma pas user lagi ganti sample), mending skip 1 buffer (senyap sekejap)
    // daripada nge-stall dan bikin delay ke seluruh playback (termasuk yang
    // kerekam layar). trigger() dari UI thread SUDAH GAK ikut lock ini lagi
    // (lihat trigger() - lock-free), jadi kontensi di sini sekarang cuma
    // kejadian pas loadSample(), bukan pas maen/mukul pad cepet-cepet.
    std::unique_lock<std::mutex> lock(samplesMutex_, std::try_to_lock);
    if (!lock.owns_lock()) return oboe::DataCallbackResult::Continue;

    // Panjang fade-out (dalam frame) buat voice yang dipotong - dihitung dari sample
    // rate stream yang beneran kepakai (normalnya 48000, lihat openStream()), bukan
    // di-hardcode, biar durasinya tetap kChokeFadeMs walau device maksa rate lain.
    const int sampleRate = stream->getSampleRate() > 0 ? stream->getSampleRate() : 48000;
    const int fadeFrames = std::max(1, (int)(sampleRate * (kChokeFadeMs / 1000.0f)));
    // Semua PCM (pad & lagu) disimpan di 48kHz. Kalau stream ternyata jalan di rate
    // lain (SRC Oboe gak aktif/ gak didukung), baca sample dgn langkah 48000/rate
    // supaya kecepatan & nada tetap benar (bukan lambat/cepat).
    const double rateScale = (double)kMusicSampleRate / (double)sampleRate;
    const bool rateMatches = std::abs(rateScale - 1.0) < 1e-4;

    // Tarik semua ketukan yang antre sejak callback sebelumnya, lalu klaim voice buat
    // masing-masing di sini (audio thread, sambil udah pegang samplesMutex_ buat baca
    // samples_ dengan aman).
    {
        // Jarak maksimal antar ketukan dalam 1 batch yang dihormati, dan lama minimal
        // sebuah ketukan kedengeran penuh sebelum boleh dipotong (dalam frame).
        const int maxStaggerFrames = std::max(1, (int)(sampleRate * (kMaxStaggerMs / 1000.0f)));
        const int minAudibleFrames = std::max(1, (int)(sampleRate * (kMinAudibleMs / 1000.0f)));

        // --- 1) Kumpulin semua ketukan yang sudah ter-publish ---
        struct Hit { int idx; float vel; int64_t timeNs; };
        Hit hits[kTriggerQueueSize];
        int numHits = 0;
        int64_t minTime = std::numeric_limits<int64_t>::max();

        int64_t readIdx = triggerReadIdx_.load(std::memory_order_relaxed);
        const int64_t reserved = triggerWriteTicket_.load(std::memory_order_acquire);
        // Antrean kepenuhan (praktis mustahil) -> proses yang paling baru aja.
        if (reserved - readIdx > kTriggerQueueSize) readIdx = reserved - kTriggerQueueSize;
        while (readIdx < reserved) {
            const int slot = (int)(readIdx % kTriggerQueueSize);
            const int64_t seq = triggerSeq_[slot].load(std::memory_order_acquire);
            // Tiket sudah diambil producer tapi datanya belum ter-publish -> berhenti di
            // sini, sisanya diambil callback berikutnya (beberapa ms lagi), urutan
            // ketukan tetap terjaga & TIDAK ada yang dibuang.
            if (seq < readIdx + 1) break;
            // Slot sudah ditimpa putaran antrean berikutnya -> ketukan ini terlalu tua.
            if (seq > readIdx + 1) { readIdx++; continue; }
            const int idx = triggerQueue_[slot].load(std::memory_order_relaxed);
            const float vel = triggerVelQueue_[slot].load(std::memory_order_relaxed);
            const int64_t t = triggerTimeQueue_[slot].load(std::memory_order_relaxed);
            // Pola seqlock: pastikan slot gak ditimpa producer selagi dibaca.
            std::atomic_thread_fence(std::memory_order_acquire);
            if (triggerSeq_[slot].load(std::memory_order_relaxed) != seq) { readIdx++; continue; }
            readIdx++;
            if (idx < 0 || (size_t)idx >= samples_.size() || samples_[idx].data.empty()) continue;
            hits[numHits++] = Hit{idx, vel, t};
            if (t < minTime) minTime = t;
        }
        triggerReadIdx_.store(readIdx, std::memory_order_relaxed);

        // --- 2) Proses satu-satu ---
        //
        // Dua masalah lama yang bikin ketukan cepat "ketelan":
        //  (a) Dua ketukan yang jatuh di batch yang sama dulu dimulai di frame yang
        //      SAMA persis -> numpuk jadi satu bunyi (dobel gak kedengeran dobel),
        //      padahal di jari mereka berjarak beberapa ms. Sekarang tiap ketukan
        //      dapat startDelay sesuai selisih waktu aslinya terhadap ketukan paling
        //      awal di batch (dibatasi kMaxStaggerMs), jadi jaraknya terjaga.
        //  (b) Ketukan lama yang dipotong choke (sesama pad, antar pad, stop loop) bisa
        //      langsung di-fade sebelum sempat kedengeran. Dulu ini cuma dicegah
        //      sebagian (voice satu batch dikecualikan dari choke sesama pad, tapi
        //      TIDAK dari choke antar pad / stop loop). Sekarang SEMUA jalur choke
        //      lewat chokeVoice(): fade baru mulai tepat saat ketukan pemotong mulai
        //      kedengeran, dan tidak lebih awal dari kMinAudibleMs setelah ketukan
        //      yang dipotong itu sendiri mulai bunyi.
        std::array<bool, kMaxVoices> assignedThisBatch{};
        assignedThisBatch.fill(false);

        auto chokeVoice = [&](int vi, int newHitDelay) {
            Voice &ov = voices_[vi];
            // Waktu (dari awal buffer ini) saat ketukan pemotong mulai bunyi, dikurangi
            // sisa start-delay voice yang dipotong -> fadeDelay dihitung dari saat voice
            // yang dipotong mulai dirender.
            int d = newHitDelay - ov.startDelay;
            const int minLeft = minAudibleFrames - ov.renderedFrames;
            if (minLeft > d) d = minLeft;
            if (d < 0) d = 0;
            beginFadeOut(ov, fadeFrames, d);
        };

        for (int h = 0; h < numHits; h++) {
            const int idx = hits[h].idx;
            const float hitVelocity = hits[h].vel;
            int hitDelay = 0;
            if (numHits > 1) {
                const double dNs = (double)(hits[h].timeNs - minTime);
                double df = dNs * 1e-9 * (double)sampleRate;
                if (df > (double)maxStaggerFrames) df = (double)maxStaggerFrames;
                if (df > 0.0) hitDelay = (int)df;
            }

            // --- CHOKE ANTAR PAD ---
            // Pad idx ini baru dipukul. Pad lain (di preset yang sama) yang punya
            // crossChokeEnabled_ aktif DAN mencentang pad ini di mask-nya dihentikan
            // (satu arah, per pad - lihat setPadCrossChokeMask).
            {
                int hitPreset = idx / kMaxPads + 1;
                int hitPad = idx % kMaxPads + 1;
                int presetBase = (hitPreset - 1) * kMaxPads;
                for (int p = 0; p < kMaxPads; p++) {
                    int ownerIdx = presetBase + p;
                    if (ownerIdx == idx) continue; // pad gak nyetop dirinya sendiri lewat jalur ini
                    if (!crossChokeEnabled_[ownerIdx].load()) continue;
                    uint16_t mask = crossChokeMask_[ownerIdx].load();
                    if ((mask & (uint16_t)(1u << (hitPad - 1))) == 0) continue;
                    for (int i = 0; i < kMaxVoices; i++) {
                        if (voices_[i].active.load() && voices_[i].buffer == &samples_[ownerIdx]) {
                            chokeVoice(i, hitDelay);
                        }
                    }
                }
            }

            // --- LOOP (gaya DTX M12) ---
            // Kalau pad ini SAAT INI punya voice yang lagi looping (voice.loop), ketukan
            // ini fungsinya STOP loop itu, bukan mulai bunyi baru. Dicek dari status
            // voice yang beneran jalan (bukan padLoop_[idx]) supaya mematikan toggle
            // pas loop lagi muter tetap bisa di-stop dengan ketukan berikutnya.
            int existingLoop = -1;
            for (int i = 0; i < kMaxVoices; i++) {
                if (voices_[i].active.load() && voices_[i].loop &&
                    voices_[i].buffer == &samples_[idx]) {
                    existingLoop = i;
                    break;
                }
            }
            if (existingLoop >= 0) {
                chokeVoice(existingLoop, hitDelay);
                continue; // stop-toggle - ketukan ini gak mulai voice baru
            }

            // --- CHOKE SESAMA PAD (default aktif, bisa dimatiin per pad) ---
            // Semua voice pad ini yang masih bunyi di-FADE (bukan dipotong mendadak,
            // biar gak "tet"), sementara ketukan baru dapat voice BARU dan langsung
            // bunyi -> crossfade singkat. Voice yang baru dibuat di batch ini ikut
            // di-choke juga (lewat chokeVoice, tetap dijamin kedengeran minimal
            // kMinAudibleMs).
            if (padChoke_[idx].load()) {
                for (int i = 0; i < kMaxVoices; i++) {
                    if (voices_[i].active.load() && voices_[i].buffer == &samples_[idx]) {
                        chokeVoice(i, hitDelay);
                    }
                }
            }

            // --- Pilih voice buat ketukan ini ---
            int v = -1;
            for (int i = 0; i < kMaxVoices; i++) {
                if (!assignedThisBatch[i] && !voices_[i].active.load()) {
                    v = i;
                    break;
                }
            }
            if (v < 0) {
                // Semua slot kepake. Curi voice yang lagi fade-out dan paling hampir
                // senyap (progress terbesar) - jauh lebih halus daripada nyuri yang
                // lagi keras bunyinya. Voice yang fade-nya belum mulai (fadeDelay > 0)
                // atau belum mulai bunyi (startDelay > 0) bukan kandidat.
                float bestProgress = -1.0f;
                for (int i = 0; i < kMaxVoices; i++) {
                    if (!assignedThisBatch[i] && voices_[i].active.load() && voices_[i].fading &&
                        voices_[i].fadeDelay == 0 && voices_[i].startDelay == 0 &&
                        voices_[i].fadeProgress > bestProgress) {
                        bestProgress = voices_[i].fadeProgress;
                        v = i;
                    }
                }
            }
            if (v < 0) {
                // Fallback round-robin, hindari slot yang sudah dipakai batch ini.
                for (int tries = 0; tries < kMaxVoices; tries++) {
                    int cand = nextVoice_.fetch_add(1) % kMaxVoices;
                    if (!assignedThisBatch[cand]) { v = cand; break; }
                }
                if (v < 0) v = nextVoice_.fetch_add(1) % kMaxVoices; // semua kepake -> curi salah satu
            }

            Voice &nv = voices_[v];
            // Matiin dulu kalau slot ini bekas voice yang masih aktif (hasil curian)
            // supaya isPadLooping()/mixing gak ngeliat state setengah-diganti.
            nv.active.store(false);
            nv.buffer = &samples_[idx];
            nv.positionF = 0.0;
            // Velocity MIDI (1.0 buat sentuhan/keyboard) ikut di-snapshot bareng volume pad.
            nv.volume = padVolume_[idx].load() * hitVelocity;
            // Loop baru kalau setelan pad ini aktif (padLoop_[idx]); voice wrap ke frame 0
            // tanpa dinonaktifkan sampai ketukan berikutnya nge-stop lewat existingLoop.
            // HARUS di-reset eksplisit karena slot ini bisa bekas pad lain.
            nv.loop = padLoop_[idx].load();
            nv.loopPad.store(nv.loop ? idx : -1);
            // Voice ini "milik" slot (preset,pad) idx - dipakai buat nyari tempo LIVE loop
            // (padLoopTempo_[idx]) tiap callback. padLoopTempo_ sengaja TIDAK di-reset
            // pas loop baru mulai: tempo terakhir yang di-set user nempel terus.
            nv.padIndex = idx;
            // 2^(semitone/12): rasio kecepatan baca sample buat tune.
            nv.pitchRatio = std::pow(2.0f, padTune_[idx].load() / 12.0f);
            // Reset state fade & jadwal: ketukan baru mulai dari gain penuh.
            nv.fading = false;
            nv.fadeProgress = 0.0f;
            nv.fadeInc = 0.0f;
            nv.fadeDelay = 0;
            nv.startDelay = hitDelay;
            nv.renderedFrames = 0;
            nv.active.store(true);
            assignedThisBatch[v] = true;
        }
    }

    for (auto &v : voices_) {
        if (!v.active.load() || v.buffer == nullptr) continue;
        int srcChannels = v.buffer->channels;
        int totalFrames = (int)(v.buffer->data.size() / srcChannels);
        if (totalFrames <= 0) { v.active.store(false); continue; }
        // kendangVol = mixer global (ngalikan SEMUA pad), v.volume = level pad ITU
        // SENDIRI (di-set lewat dialog Pengaturan) - dua-duanya digabung di sini,
        // jadi user bisa nurunin/naikin 1 pad doang tanpa ganggu pad lain, DAN
        // tetep punya kontrol volume kendang keseluruhan seperti biasa.
        float kendangVol = kendangVolume_.load() * v.volume;

        // Tempo LIVE loop (gaya turntable DTX M12): CUMA berlaku buat voice yang lagi
        // loop (v.loop true) - voice biasa (sekali ketuk) gak kepengaruh sama sekali,
        // tetap pakai pitchRatio apa adanya. Dihitung SEKALI per voice per callback
        // (bukan tiap frame di dalam) - cukup buat kedengeran berubah "langsung" tanpa
        // nge-load atomic berkali-kali sia-sia dalam 1 buffer yang sama.
        float loopTempo = (v.loop && v.padIndex >= 0) ? padLoopTempo_[v.padIndex].load() : 1.0f;

        // Titik wrap loop: PAKAI loopEndFrame (bukan totalFrames) buat voice yang
        // lagi loop - itu udah dihitung SEKALI pas loadSample() (nyari frame
        // terakhir yang beneran ada bunyinya + padding kecil), jadi loop lompat
        // balik ke awal PERSIS pas levelnya mulai turun ke hening, bukan nunggu
        // beneran abis nyampe akhir data (yang bisa aja masih nyisa hening ekor
        // dari file WAV aslinya). totalFrames (panjang PENUH) tetap dipakai apa
        // adanya buat voice yang BUKAN loop (sekali ketuk) - mode itu tetap muter
        // sample ori sampai bener-bener habis, sama sekali gak kepengaruh.
        int wrapFrames = totalFrames;
        if (v.loop && v.buffer->loopEndFrame > 0 && v.buffer->loopEndFrame <= totalFrames) {
            wrapFrames = v.buffer->loopEndFrame;
        }

        for (int frame = 0; frame < numFrames; frame++) {
            // Ketukan yang dijadwalkan mulai belakangan di dalam batch (lihat startDelay)
            // belum kedengeran & posisinya belum maju sama sekali selama delay ini.
            if (v.startDelay > 0) { v.startDelay--; continue; }
            // positionF pecahan (bukan int) karena pitchRatio biasanya bukan 1.0 kalau
            // pad di-tune - baca 2 frame source terdekat (i0/i1) & interpolasi linear,
            // ini yang bikin hasil pitch-shift-nya gak berisik/aliasing kasar walau
            // resampling-nya sederhana (linear, bukan sinc).
            if (v.positionF >= (double)wrapFrames) {
                if (v.loop) {
                    // Wrap balik ke awal sample TANPA JEDA - dilakukan di tengah
                    // frame loop yang sama (bukan nunggu callback berikutnya), jadi
                    // gak ada satu buffer pun yang kelewat senyap. fmod (bukan cuma
                    // "- wrapFrames") biar tetap benar walau pitchRatio besar bikin
                    // positionF sempat lompat lebih dari 1 panjang sample sekaligus.
                    v.positionF = std::fmod(v.positionF, (double)wrapFrames);
                } else {
                    v.active.store(false);
                    break;
                }
            }
            // Gain fade-out (1.0 kalau voice gak lagi dipotong). Kurva smoothstep
            // (1 - p^2*(3-2p)) bukan garis lurus: turunnya landai di awal DAN akhir,
            // jadi gak ada "patahan" gelombang di titik mulai/selesai fade (patahan
            // itu sendiri bisa kedengeran sebagai klik halus). Kalau progress udah
            // penuh, voice dimatiin di sini - frame ini gak dirender (gain-nya 0).
            float fadeGain = 1.0f;
            if (v.fading) {
                if (v.fadeDelay > 0) {
                    // Fade sudah diminta tapi belum waktunya (lihat chokeVoice) - tetap
                    // bunyi penuh dulu.
                    v.fadeDelay--;
                } else {
                    float p = v.fadeProgress;
                    if (p >= 1.0f) {
                        v.active.store(false);
                        v.fading = false;
                        break;
                    }
                    fadeGain = 1.0f - p * p * (3.0f - 2.0f * p);
                    v.fadeProgress = p + v.fadeInc;
                }
            }

            int i0 = (int)v.positionF;
            // i1 di-clamp ke frame terakhir (bukan lanjut ke luar array) - pas posisi
            // udah di frame paling akhir, ini efektif "nahan" sample terakhir buat
            // interpolasi alih-alih baca di luar batas buffer.
            int i1 = std::min(i0 + 1, totalFrames - 1);
            float frac = (float)(v.positionF - (double)i0);

            for (int ch = 0; ch < outChannels; ch++) {
                int srcCh = (srcChannels == 1) ? 0 : (ch % srcChannels);
                float s0 = v.buffer->data[(size_t)i0 * srcChannels + srcCh];
                float s1 = v.buffer->data[(size_t)i1 * srcChannels + srcCh];
                float sample = s0 + (s1 - s0) * frac;
                float mixed = sample * kendangVol * fadeGain;
                out[frame * outChannels + ch] += mixed;
            }

            // loopTempo cuma pernah != 1.0 kalau v.loop true (lihat perhitungan di
            // atas), jadi voice non-loop tetap maju persis sebesar pitchRatio-nya
            // seperti sebelumnya - baris ini gak ngubah perilaku pad yang gak loop.
            v.positionF += (double)(v.pitchRatio * loopTempo) * rateScale;
            if (v.renderedFrames < (1 << 30)) v.renderedFrames++;
        }
    }

    // Mix track musik (kalau lagi play) tepat di sini -> sebelum clamp & sebelum
    // capture rekaman, jadi lagu otomatis ikut ke-record bareng pukulan kendang,
    // persis kayak yang kedengeran. try_lock juga di sini biar loadMusic() (bisa
    // makan waktu, filenya bisa gede) gak pernah nge-block audio thread.
    if (musicPlaying_.load()) {
        std::unique_lock<std::mutex> mlock(musicMutex_, std::try_to_lock);
        if (mlock.owns_lock() && !music_.data.empty()) {
            float musicVol = musicVolume_.load();
            int64_t pos = musicPosition_.load();
            int64_t totalFrames = music_.totalFrames;
            if (rateMatches) {
                for (int frame = 0; frame < numFrames; frame++) {
                    if (pos >= totalFrames) {
                        musicPlaying_.store(false); // lagu abis -> auto-stop
                        break;
                    }
                    for (int ch = 0; ch < outChannels; ch++) {
                        int srcCh = ch % 2; // music_ selalu stereo
                        float sample = music_.data[(size_t)pos * 2 + srcCh] / 32768.0f;
                        out[frame * outChannels + ch] += sample * musicVol;
                    }
                    pos++;
                }
            } else {
                // Stream bukan 48kHz -> maju pecahan frame per output frame + interpolasi linear.
                double posF = (double)pos + musicFrac_;
                for (int frame = 0; frame < numFrames; frame++) {
                    int64_t i0 = (int64_t)posF;
                    if (i0 >= totalFrames) {
                        musicPlaying_.store(false); // lagu abis -> auto-stop
                        break;
                    }
                    int64_t i1 = std::min<int64_t>(i0 + 1, totalFrames - 1);
                    float frac = (float)(posF - (double)i0);
                    for (int ch = 0; ch < outChannels; ch++) {
                        int srcCh = ch % 2;
                        float s0 = music_.data[(size_t)i0 * 2 + srcCh] / 32768.0f;
                        float s1 = music_.data[(size_t)i1 * 2 + srcCh] / 32768.0f;
                        out[frame * outChannels + ch] += (s0 + (s1 - s0) * frac) * musicVol;
                    }
                    posF += rateScale;
                }
                pos = (int64_t)posF;
                musicFrac_ = posF - (double)pos;
            }
            musicPosition_.store(pos);
        }
    }

    // soft clamp biar gak crackling kalau beberapa pad numpuk
    for (int i = 0; i < numFrames * outChannels; i++) {
        out[i] = std::max(-1.0f, std::min(1.0f, out[i]));
    }

    // Kalau lagi rekam: tangkep persis buffer yang barusan di-mix di atas (post-clamp),
    // jadi hasil rekaman = persis apa yang kedengeran.
    //
    // FIX "tet tet tet" (suara REKAM LAYAR kedengeran kretek/putus-putus tiap
    // sebentar): sebelumnya di sini pakai try_lock, dan kalau lagi kebentur sama
    // drainRecordBuffer()/stopRecording() (dipanggil berkali-kali per detik dari
    // thread encoder audio rekam layar), SELURUH buffer callback ini (bisa
    // beberapa milidetik audio) langsung DIBUANG diam-diam dari recordBuffer_ -
    // padahal suara yang didengar user real-time TETAP normal (buang-nya cuma
    // di jalur rekaman). Hasilnya: di FILE hasil rekaman ada bolong kecil
    // berulang di semua titik yang kebentur lock itu, dan tiap bolong kedengeran
    // sebagai klik/"tet" karena gelombang audio jadi gak nyambung mulus.
    // Sekarang pakai lock_guard biasa (nunggu, gak pernah skip data) - AMAN buat
    // audio thread real-time karena drainRecordBuffer()/stopRecording() sudah
    // diubah supaya cuma pegang recordMutex_ untuk satu std::deque::swap() O(1)
    // (lihat komentar di sana), jadi paling lama nunggu cuma beberapa instruksi,
    // bukan durasi copy seluruh buffer rekaman.
    if (recording_.load()) {
        std::lock_guard<std::mutex> recLock(recordMutex_);
        // resize + tulis langsung (bukan push_back per sample ke deque): tidak ada
        // malloc per-chunk di audio thread, dan kapasitas sudah di-reserve di sisi drain.
        const size_t samplesThisBuffer = (size_t)numFrames * outChannels;
        const size_t oldSize = recordBuffer_.size();
        if (oldSize + samplesThisBuffer <= kRecordMaxSamples) {
            recordBuffer_.resize(oldSize + samplesThisBuffer);
            int16_t *dst = recordBuffer_.data() + oldSize;
            for (size_t i = 0; i < samplesThisBuffer; i++) {
                dst[i] = (int16_t)std::lround(out[i] * 32767.0f);
            }
        }
    }

    return oboe::DataCallbackResult::Continue;
}
