#include "runtime/ps2_fmv_override.h"
#include "runtime/ps2_gs_pgs.h"            // [fmvoverride] temporarily drop pack mode during the movie
#include "runtime/ps2_gs_gpu_renderer.h"   // [fmvoverride] GsGpuRenderer::texPackEnabled()

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

#include "runtime/ps2_toml.h"
#include <cstring>   // [linux] std::strstr comes from <cstring> there, not from a transitively included header

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>   // [linuxfix] std::strstr
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern std::atomic<uint32_t> g_ps2ForceSkipFrames;   // [skipforce] defined in ps2_gs_gpu.cpp
extern uint64_t g_fmvCapGen;                          // [fmvcapture] ps2_gs_gpu.cpp: native captured frames
extern "C" const char *ps2xExeDirC();                 // main.cpp: resolved executable dir (honors PS2X_EXEDIR)

namespace ps2x_fmv
{
namespace
{
    struct Frame
    {
        std::vector<uint8_t> rgba;
        int w = 0;
        int h = 0;
        double pts = 0.0;
    };

    std::mutex g_mx;
    std::condition_variable g_cv;
    std::deque<Frame> g_queue;
    std::thread g_thread;
    std::atomic<bool> g_run{false};
    bool g_eof = false;
    bool g_started = false;
    bool g_skipPoked = false;
    bool g_halted = false;   // [fmvoverride] stopped this session (safety/end): don't re-inject
    std::string g_path;
    double g_duration = 0.0;
    double g_fps = 30000.0 / 1001.0;   // [videoclk] source frame rate (native-progress clock unit)
    double g_aspect = 4.0 / 3.0;
    std::chrono::steady_clock::time_point g_start;
    // [videoclk] Native-anchored clock: while the game's own movie advances, the injected video is
    // driven by the native frame counter (one source frame per native frame -> exact sync with the
    // game's playback and its skip). If the native capture stalls, it free-runs on wall time.
    uint64_t g_clockNative = 0;        // last g_fmvCapGen seen
    // [fmvfix] g_fmvCapGen counts captured movie frames since BOOT and is never reset, so the timeline
    // must be relative to this movie: frames captured up to the end of the previous session belong
    // to earlier movies (without this a second movie started its video that many seconds in).
    uint64_t g_clockBase = 0;
    double g_clockT = 0.0;             // presentation time (seconds) in the video timeline
    std::chrono::steady_clock::time_point g_clockLast;

    Frame g_current;            // owned by the present (tick) thread only
    uint64_t g_gen = 0;

    constexpr size_t kQueueMax = 6;

    // ---- enablement / asset paths ------------------------------------------------
    std::string exeDir()
    {
        const char *d = ps2xExeDirC();
        return (d && d[0]) ? std::string(d) : std::string(".");
    }

    std::string packVideoDir() { return exeDir() + "/data/Textures/Video"; }

    std::string envOverridePath()
    {
        const char *v = std::getenv("PS2X_FMV_OVERRIDE");
        return (v && v[0] && v[0] != '0') ? std::string(v) : std::string();
    }

    bool fileExists(const std::string &p)
    {
        std::error_code ec;
        return std::filesystem::is_regular_file(p, ec);
    }

    // [exeDir]/savedata/settings.toml -> the two [video] pack toggles. Cached (the launcher writes
    // them before launch). texture_pack = the pack is installed; intro_video = show the 4K opening
    // override. The intro toggle applies on restart (the native PSS/ADX swap happens at loadELF).
    struct PackToggles { bool texPack = false; bool introVideo = true; };
    const PackToggles &tomlPackToggles()
    {
        static const PackToggles s = []() {
            PackToggles t;
            std::ifstream f(exeDir() + "/savedata/settings.toml");
            if (!f.is_open())
                return t;
            ps2x_toml::Document doc;
            doc.parse(f);
            t.texPack = doc.getB("video.texture_pack", false);
            t.introVideo = doc.getB("video.intro_video", true);
            return t;
        }();
        return s;
    }

    void decodeWorker(std::string path)
    {
        AVFormatContext *fmt = nullptr;
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
        {
            std::fprintf(stderr, "[fmvoverride] open failed: %s\n", path.c_str());
            std::lock_guard<std::mutex> lk(g_mx); g_eof = true; g_cv.notify_all(); return;
        }
        if (avformat_find_stream_info(fmt, nullptr) < 0)
        {
            std::fprintf(stderr, "[fmvoverride] no stream info\n");
            avformat_close_input(&fmt);
            std::lock_guard<std::mutex> lk(g_mx); g_eof = true; g_cv.notify_all(); return;
        }
        int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vs < 0)
        {
            std::fprintf(stderr, "[fmvoverride] no video stream\n");
            avformat_close_input(&fmt);
            std::lock_guard<std::mutex> lk(g_mx); g_eof = true; g_cv.notify_all(); return;
        }
        AVStream *st = fmt->streams[vs];
        const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
        AVCodecContext *cc = avcodec_alloc_context3(dec);
        if (!dec || !cc || avcodec_parameters_to_context(cc, st->codecpar) < 0)
        {
            std::fprintf(stderr, "[fmvoverride] decoder init failed\n");
            if (cc) avcodec_free_context(&cc);
            avformat_close_input(&fmt);
            std::lock_guard<std::mutex> lk(g_mx); g_eof = true; g_cv.notify_all(); return;
        }
        cc->thread_count = 0;
        if (avcodec_open2(cc, dec, nullptr) < 0)
        {
            std::fprintf(stderr, "[fmvoverride] avcodec_open2 failed\n");
            avcodec_free_context(&cc);
            avformat_close_input(&fmt);
            std::lock_guard<std::mutex> lk(g_mx); g_eof = true; g_cv.notify_all(); return;
        }
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (cc->width > 0 && cc->height > 0) g_aspect = (double)cc->width / (double)cc->height;
            if (fmt->duration > 0) g_duration = (double)fmt->duration / (double)AV_TIME_BASE;
            if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
                g_fps = (double)st->avg_frame_rate.num / (double)st->avg_frame_rate.den;
            else if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0)
                g_fps = (double)st->r_frame_rate.num / (double)st->r_frame_rate.den;
            if (g_fps < 1.0 || g_fps > 240.0) g_fps = 30000.0 / 1001.0;
            std::fprintf(stderr, "[fmvoverride] decoding %dx%d, dur=%.2fs, codec=%s\n",
                         cc->width, cc->height, g_duration, avcodec_get_name(st->codecpar->codec_id));
            // [fmvguard] Warn loudly when the clip is very unlikely to decode in software in this
            // process: AV1 has no hwaccel here, and >1440p software decode rarely keeps up. A wrong
            // clip otherwise just shows as a silent black movie.
            {
                const char *cn = avcodec_get_name(st->codecpar->codec_id);
                const bool av1 = (st->codecpar->codec_id == AV_CODEC_ID_AV1) ||
                                 (cn && std::strstr(cn, "av1") != nullptr);
                const bool huge = (cc->width > 2560 || cc->height > 1440);
                if (av1 || huge)
                    std::fprintf(stderr, "[fmvguard] WARNING: %s%s%s -> software decode may produce NO frames "
                                         "(black video). Use H.264 High 8-bit yuv420p at <=2560x1440, 30fps, CRF 16.\n",
                                 av1 ? "AV1 has no hardware decode on this platform" : "",
                                 (av1 && huge) ? " and " : "", huge ? "resolution is above 2560x1440" : "");
            }
        }

        SwsContext *sws = nullptr;
        int sw = 0, sh = 0;
        AVPacket *pkt = av_packet_alloc();
        AVFrame *frm = av_frame_alloc();
        const double timebase = av_q2d(st->time_base);
        bool readDone = false;

        while (g_run.load(std::memory_order_relaxed))
        {
            {   // don't decode too far ahead of the present
                std::unique_lock<std::mutex> lk(g_mx);
                g_cv.wait(lk, [&] { return !g_run.load(std::memory_order_relaxed) || g_queue.size() < kQueueMax; });
                if (!g_run.load(std::memory_order_relaxed)) break;
            }

            int r = readDone ? AVERROR_EOF : av_read_frame(fmt, pkt);
            if (r < 0)
            {
                avcodec_send_packet(cc, nullptr);   // flush
                readDone = true;
            }
            else
            {
                if (pkt->stream_index != vs) { av_packet_unref(pkt); continue; }
                avcodec_send_packet(cc, pkt);
                av_packet_unref(pkt);
            }

            bool gotFrame = false;
            while (true)
            {
                int rr = avcodec_receive_frame(cc, frm);
                {   // [fmvdec] why no frames: the ffmpeg codes (EAGAIN = need more input).
                    static int s_n = 0, s_fr = 0;
                    if (rr == 0) ++s_fr;
                    if (s_n < 30)
                    { ++s_n; std::fprintf(stderr, "[fmvdec] recv=%d frames=%d readDone=%d\n", rr, s_fr, (int)readDone); }
                }
                if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
                if (rr < 0) break;
                gotFrame = true;
                int w = frm->width, h = frm->height;
                if (w <= 0 || h <= 0) { av_frame_unref(frm); continue; }
                if (!sws || w != sw || h != sh)
                {
                    if (sws) sws_freeContext(sws);
                    sws = sws_getContext(w, h, (AVPixelFormat)frm->format, w, h, AV_PIX_FMT_RGBA,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr);
                    sw = w; sh = h;
                }
                Frame fr;
                fr.w = w; fr.h = h;
                fr.rgba.resize((size_t)w * (size_t)h * 4u);
                uint8_t *dst[4] = {fr.rgba.data(), nullptr, nullptr, nullptr};
                int dls[4] = {w * 4, 0, 0, 0};
                sws_scale(sws, frm->data, frm->linesize, 0, h, dst, dls);
                int64_t pts = frm->best_effort_timestamp;
                if (pts == AV_NOPTS_VALUE) pts = frm->pts;
                fr.pts = (pts == AV_NOPTS_VALUE) ? 0.0 : (double)pts * timebase;
                av_frame_unref(frm);
                {
                    std::lock_guard<std::mutex> lk(g_mx);
                    g_queue.push_back(std::move(fr));
                }
                g_cv.notify_all();
            }

            if (readDone && !gotFrame)   // fully drained
            {
                std::lock_guard<std::mutex> lk(g_mx);
                g_eof = true;
                g_cv.notify_all();
                break;
            }
        }

        if (sws) sws_freeContext(sws);
        if (frm) av_frame_free(&frm);
        if (pkt) av_packet_free(&pkt);
        avcodec_free_context(&cc);
        avformat_close_input(&fmt);
    }

    void startSession()
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_queue.clear();
        g_current = Frame{};
        g_eof = false;
        g_skipPoked = false;
        g_duration = 0.0;
        g_aspect = 4.0 / 3.0;
        g_path = videoPath();
        g_start = std::chrono::steady_clock::now();
        g_clockNative = (g_fmvCapGen > 0) ? (g_fmvCapGen - 1) : 0;   // force a fresh sync on first tick
        g_clockT = 0.0;
        g_clockLast = g_start;
        g_started = true;
        g_run.store(true, std::memory_order_relaxed);
        if (g_thread.joinable()) g_thread.join();
        g_thread = std::thread(decodeWorker, g_path);
        std::fprintf(stderr, "[fmvoverride] VIDEO INIT -> injecting '%s'\n", g_path.c_str());
        // [fmvoverride] The movie's VRAM uploads are huge (multi-MB path3); the replacement
        // hashing on every one of them starves the guest and wedges the movie. We don't present
        // the native movie anyway, so drop pack mode while the override is live.
        if (ps2x_pgs::packMode()) ps2x_pgs::setPackEnabled(false);
    }

    void stopSession()
    {
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
        std::fprintf(stderr, "[fmvoverride] VIDEO END at %.2fs (poked=%d) -> stopping injection\n",
                     el, g_skipPoked ? 1 : 0);
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_started = false;
            g_run.store(false, std::memory_order_relaxed);
        }
        g_cv.notify_all();
        if (g_thread.joinable()) g_thread.join();
        std::lock_guard<std::mutex> lk(g_mx);
        g_queue.clear();
        g_current = Frame{};
        g_eof = false;
        g_clockBase = g_fmvCapGen;   // [fmvfix] the next movie's timeline starts after this one's frames
        // [fmvoverride] restore the user's Texture Replacement setting for menus/fights.
        if (ps2x_pgs::packMode()) ps2x_pgs::setPackEnabled(GsGpuRenderer::texPackEnabled());
    }
} // namespace

bool enabled()
{
    if (!envOverridePath().empty()) return true;                 // PS2X_FMV_OVERRIDE wins
    const PackToggles &t = tomlPackToggles();
    if (!t.texPack) return false;                                // [video] texture_pack must be on
    if (!t.introVideo) return false;                             // [video] intro_video off = no override
    return fileExists(packVideoDir() + "/ZS3USOP_4k.mp4");       // ...and the pack ships the video
}

std::string videoPath()
{
    const std::string env = envOverridePath();
    if (!env.empty()) return env;
    return packVideoDir() + "/ZS3USOP_4k.mp4";
}

std::string packAsset(const char *name)
{
    const std::string p = packVideoDir() + "/" + name;
    return fileExists(p) ? p : std::string();
}

bool tick(bool movieActive, FmvOverrideFrame &out)
{
    if (!enabled()) return false;

    if (!movieActive)
    {
        if (g_started) stopSession();
        g_halted = false;   // re-arm for the next movie session
        return false;
    }
    if (!g_started && !g_halted) startSession();
    if (!g_started) return false;

    // [videoclk] Native-anchored clock: native frames drive the timeline while the game's own movie
    // advances (exact sync with playback and its skip); wall time covers a stalled native capture.
    const double el0 = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    {
        const auto now = std::chrono::steady_clock::now();
        const uint64_t nf = g_fmvCapGen;
        const double dt = std::chrono::duration<double>(now - g_clockLast).count();
        g_clockLast = now;
        if (nf != g_clockNative) { g_clockNative = nf; g_clockT = (double)(nf - g_clockBase) / g_fps; }
        else g_clockT += (dt > 0.0 && dt < 0.5) ? dt : 0.0;
        static auto s_lastLog = now;
        if (std::chrono::duration<double>(now - s_lastLog).count() >= 5.0)
        {
            s_lastLog = now;
            std::fprintf(stderr, "[videoclk] native=%llu t=%.2fs wall=%.2fs drift=%.3fs fps=%.3f\n",
                         (unsigned long long)nf, g_clockT, el0, g_clockT - el0, g_fps);
        }
    }
    const double el = g_clockT;
    // [avoffset] shift the video timeline to line it up with the game's native ADX. Positive
    // ADVANCES the video (presents later frames sooner); negative delays it.
    static const double s_avoff = [](){ const char *v = std::getenv("PS2X_FMV_AVOFFSET"); return v ? std::atof(v) : 0.6; }();
    const double vel = el + s_avoff;

    double dur = 0.0;
    {   // advance the queue by presentation time (present-thread only consumer)
        std::lock_guard<std::mutex> lk(g_mx);
        dur = g_duration;
        while (!g_queue.empty())
        {
            if (g_current.rgba.empty() || g_queue.front().pts <= vel)
            {
                g_current = std::move(g_queue.front());
                g_queue.pop_front();
                ++g_gen;
            }
            else break;
        }
    }
    g_cv.notify_all();

    {   // [fmvguard] session active but still no frame after a few seconds -> say so once.
        static bool s_warned = false;
        if (!s_warned && g_current.rgba.empty() && el > 3.0)
        {
            s_warned = true;
            std::fprintf(stderr, "[fmvguard] no video frames after %.1fs -- the clip is not decoding "
                                 "(black movie). Check the codec/resolution.\n", el);
        }
    }
    // [fade] melt to black over the last PS2X_FMV_FADE seconds of the video.
    static const double s_fade = [](){
        const char *v = std::getenv("PS2X_FMV_FADE");
        const double d = v ? std::atof(v) : 0.7;
        return (d >= 0.0 && d < 5.0) ? d : 0.7; }();
    float alpha = 1.0f;
    if (dur > 0.0 && s_fade > 0.0)
    {
        const double rem = dur - el;
        if (rem <= s_fade)
        {
            alpha = (float)(rem / s_fade);
            if (alpha < 0.0f) alpha = 0.0f;
            if (alpha > 1.0f) alpha = 1.0f;
        }
    }

    static const double s_lead = [](){
        const char *v = std::getenv("PS2X_FMV_SKIPLEAD");
        const double d = v ? std::atof(v) : 0.5;
        return (d >= 0.0 && d < 5.0) ? d : 0.5; }();
    static uint32_t s_pulse = 0u;
    if (dur > 0.0 && el >= dur - s_lead)
    {
        // Pulse the skip button (short press + release) instead of holding it down forever:
        // an edge-triggered skip needs a release, and a permanent hold can wedge the game.
        if ((s_pulse % 12u) == 0u) g_ps2ForceSkipFrames.store(4u, std::memory_order_relaxed);
        ++s_pulse;
        if (!g_skipPoked)
        {
            g_skipPoked = true;
            std::fprintf(stderr, "[fmvoverride] %.2fs left (lead=%.2fs) -> poking skip\n", dur - el, s_lead);
        }
    }

    // [fmvsafety] If the native movie never reports VIDEO END (missed reset/delete) the override
    // would keep drawing the last frame over whatever the game shows. Stop drawing shortly after
    // the duration so the game's own frame is visible again.
    if (dur > 0.0 && el > dur + 2.0)
    {
        std::fprintf(stderr, "[fmvoverride] safety stop at %.2fs (no VIDEO END)\n", el);
        stopSession();
        g_halted = true;   // do not re-inject while this same movie session is still active
        return false;
    }

    if (g_current.rgba.empty()) return false;
    out.rgba = g_current.rgba.data();
    out.w = g_current.w;
    out.h = g_current.h;
    out.gen = g_gen;
    out.aspect = g_aspect;
    out.alpha = alpha;
    return true;
}
} // namespace ps2x_fmv
