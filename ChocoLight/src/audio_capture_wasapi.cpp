/**
 * @file audio_capture_wasapi.cpp
 * @brief Phase F.0.11.6.5.A13 — Windows WASAPI loopback audio capture 实现
 *
 * 实现要点:
 *   1. capture thread 内 CoInitializeEx(MULTITHREADED) — 不污染主线程 COM apartment
 *   2. IMMDeviceEnumerator → GetDefaultAudioEndpoint(eRender, eConsole)
 *   3. IAudioClient::Initialize 用 AUDCLNT_STREAMFLAGS_LOOPBACK + SHAREMODE_SHARED
 *   4. IAudioCaptureClient::GetBuffer 循环拉数据, AUDCLNT_BUFFERFLAGS_SILENT 时填零
 *   5. 内部 SPSC ring buffer (mutex 保护, 简化锁实现), 16 MB 容量 (~85 秒 48kHz stereo float32)
 *   6. Ring overflow 时丢弃最老数据 (主线程拉不及时, 录像会丢音, 但不 crash)
 */

#include "audio_capture_wasapi.h"
#include "light.h"
#include <cstring>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>

#ifdef _WIN32
// WASAPI 头文件 (Windows SDK)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <combaseapi.h>

// FFmpeg AVSampleFormat 常量 (与 light_av/video_backend_ffmpeg 同模式)
enum {
    FF_AV_SAMPLE_FMT_S16  = 1,
    FF_AV_SAMPLE_FMT_FLT  = 3,
    FF_AV_SAMPLE_FMT_S16P = 6,
    FF_AV_SAMPLE_FMT_FLTP = 8,
};

// WAVEFORMAT 子类型 GUID (来自 ksmedia.h)
static const GUID KSDATAFORMAT_SUBTYPE_PCM_GUID         = {0x00000001, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_GUID  = {0x00000003, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
#endif

namespace AudioCaptureWASAPI {

// ============================================================
// 内部状态 (跨平台共用)
// ============================================================
struct CaptureState {
    std::thread             thread;
    std::atomic<bool>       stop_flag{false};
    std::atomic<bool>       running{false};

    int                     sample_rate = 0;
    int                     channels    = 0;
    int                     sample_fmt  = 0;   // FFmpeg AVSampleFormat
    int                     bytes_per_frame = 0;  // channels × bytes_per_sample (interleaved)

    // Ring buffer (interleaved frames, capacity = kRingFrames × bytes_per_frame)
    //   容量上限固定 16 MB (~85 秒 @ 48kHz stereo float32 = 384KB/s)
    static constexpr size_t kRingBytes = 16 * 1024 * 1024;
    std::vector<uint8_t>    ring;            // size = kRingBytes; 在 Start 时 resize 一次
    size_t                  write_pos = 0;   // capture thread 写指针 (byte offset)
    size_t                  read_pos  = 0;   // 主线程 / audio thread 读指针
    std::mutex              mu;              // 保护 write_pos / read_pos
    std::atomic<int64_t>    total_captured_frames{0};
};
static CaptureState g;

// ============================================================
// Ring buffer 操作 (内部, 加锁版本)
// ============================================================

/// 计算当前 ring 中可读字节数 (受 mu 保护时调用)
static size_t ring_avail_bytes_unsafe() {
    if (g.write_pos >= g.read_pos) {
        return g.write_pos - g.read_pos;
    }
    return CaptureState::kRingBytes - (g.read_pos - g.write_pos);
}

/// Capture thread 写入 (生产者): src_bytes 个字节追加; ring 满则丢弃最老 (移动 read_pos)
static void ring_write(const uint8_t* src, size_t src_bytes) {
    std::lock_guard<std::mutex> lk(g.mu);
    const size_t cap = CaptureState::kRingBytes;
    for (size_t i = 0; i < src_bytes; ) {
        size_t writable = cap - g.write_pos;
        size_t chunk    = (src_bytes - i < writable) ? (src_bytes - i) : writable;
        std::memcpy(g.ring.data() + g.write_pos, src + i, chunk);
        g.write_pos = (g.write_pos + chunk) % cap;
        i += chunk;
        // overflow: write 追上 read → 丢最老的 chunk 字节 (read_pos 跟着前移)
        if (g.write_pos == g.read_pos) {
            g.read_pos = (g.read_pos + chunk) % cap;
        }
    }
}

/// 消费者读取: 拷贝最多 max_bytes 到 dst, 返实际字节数 (可能 < max_bytes)
static size_t ring_read(uint8_t* dst, size_t max_bytes) {
    std::lock_guard<std::mutex> lk(g.mu);
    const size_t avail = ring_avail_bytes_unsafe();
    const size_t want  = (max_bytes < avail) ? max_bytes : avail;
    const size_t cap   = CaptureState::kRingBytes;
    for (size_t i = 0; i < want; ) {
        size_t readable = cap - g.read_pos;
        size_t chunk    = (want - i < readable) ? (want - i) : readable;
        std::memcpy(dst + i, g.ring.data() + g.read_pos, chunk);
        g.read_pos = (g.read_pos + chunk) % cap;
        i += chunk;
    }
    return want;
}

// ============================================================
// Windows WASAPI 实现 (_WIN32 only)
// ============================================================

#ifdef _WIN32

// COM 句柄释放 helper (避免 ATL CComPtr 依赖)
template <typename T>
static void safe_release_(T*& p) { if (p) { p->Release(); p = nullptr; } }

/// Capture thread 主循环 — 内部 init COM / WASAPI, 退出前 cleanup
static void wasapi_thread_main_() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_owned = SUCCEEDED(hr);   // S_FALSE = 已 init 过 (不算我们 own)
    // (即便 com_owned=false 也继续, 假设主线程已 init COM)

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;
    IAudioClient*        client     = nullptr;
    IAudioCaptureClient* capture    = nullptr;
    HANDLE               event      = nullptr;
    WAVEFORMATEX*        mix_fmt    = nullptr;

    // 失败时统一退出 cleanup
    auto fail_exit = [&]() {
        safe_release_(capture);
        safe_release_(client);
        safe_release_(device);
        safe_release_(enumerator);
        if (mix_fmt) { CoTaskMemFree(mix_fmt); mix_fmt = nullptr; }
        if (event)   { CloseHandle(event); event = nullptr; }
        if (com_owned) CoUninitialize();
        g.running.store(false);
    };

    // 1) IMMDeviceEnumerator
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator) {
        CC::Log(CC::LOG_ERROR, "WASAPI: CoCreateInstance(MMDeviceEnumerator) failed (0x%08lX)", (long)hr);
        fail_exit();
        return;
    }

    // 2) 默认 render 设备 (eRender = 0)
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        CC::Log(CC::LOG_ERROR, "WASAPI: GetDefaultAudioEndpoint(eRender) failed (0x%08lX)", (long)hr);
        fail_exit();
        return;
    }

    // 3) IAudioClient
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr) || !client) {
        CC::Log(CC::LOG_ERROR, "WASAPI: device->Activate(IAudioClient) failed (0x%08lX)", (long)hr);
        fail_exit();
        return;
    }

    // 4) 拿到默认设备的 mix format
    hr = client->GetMixFormat(&mix_fmt);
    if (FAILED(hr) || !mix_fmt) {
        CC::Log(CC::LOG_ERROR, "WASAPI: GetMixFormat failed (0x%08lX)", (long)hr);
        fail_exit();
        return;
    }

    // 5) 解析 mix_fmt → FFmpeg AVSampleFormat
    //    WASAPI shared loopback 通常是 WAVE_FORMAT_EXTENSIBLE + SUBTYPE_IEEE_FLOAT 32-bit stereo 48kHz
    bool is_float = false;
    bool is_pcm   = false;
    if (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float = true;
    } else if (mix_fmt->wFormatTag == WAVE_FORMAT_PCM) {
        is_pcm = true;
    } else if (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        // 检查 SubFormat GUID
        WAVEFORMATEXTENSIBLE* ext = (WAVEFORMATEXTENSIBLE*)mix_fmt;
        if (std::memcmp(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_GUID, sizeof(GUID)) == 0) {
            is_float = true;
        } else if (std::memcmp(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM_GUID, sizeof(GUID)) == 0) {
            is_pcm = true;
        }
    }
    int ff_fmt = 0;
    if (is_float && mix_fmt->wBitsPerSample == 32) {
        ff_fmt = FF_AV_SAMPLE_FMT_FLT;   // interleaved float32
    } else if (is_pcm && mix_fmt->wBitsPerSample == 16) {
        ff_fmt = FF_AV_SAMPLE_FMT_S16;   // interleaved int16
    } else {
        CC::Log(CC::LOG_ERROR, "WASAPI: unsupported mix format (tag=%d bits=%d float=%d pcm=%d)",
                (int)mix_fmt->wFormatTag, (int)mix_fmt->wBitsPerSample, (int)is_float, (int)is_pcm);
        fail_exit();
        return;
    }
    g.sample_rate     = (int)mix_fmt->nSamplesPerSec;
    g.channels        = (int)mix_fmt->nChannels;
    g.sample_fmt      = ff_fmt;
    g.bytes_per_frame = (int)mix_fmt->nBlockAlign;   // channels × bytes_per_sample

    // 6) 初始化 IAudioClient (LOOPBACK 必须用 SHARED 模式)
    //    buffer duration = 200ms (REFERENCE_TIME 单位是 100ns, 1 ms = 10000)
    //    LOOPBACK + EVENTCALLBACK 互斥 → 用轮询 (GetBuffer + Sleep)
    const REFERENCE_TIME hns_buffer = 200 * 10000;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_LOOPBACK,
                            hns_buffer, 0, mix_fmt, nullptr);
    if (FAILED(hr)) {
        CC::Log(CC::LOG_ERROR, "WASAPI: IAudioClient::Initialize failed (0x%08lX)", (long)hr);
        fail_exit();
        return;
    }

    // 7) IAudioCaptureClient
    hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
    if (FAILED(hr) || !capture) {
        CC::Log(CC::LOG_ERROR, "WASAPI: GetService(IAudioCaptureClient) failed (0x%08lX)", (long)hr);
        fail_exit();
        return;
    }

    // 8) Start
    hr = client->Start();
    if (FAILED(hr)) {
        CC::Log(CC::LOG_ERROR, "WASAPI: IAudioClient::Start failed (0x%08lX)", (long)hr);
        fail_exit();
        return;
    }

    g.running.store(true);
    CC::Log(CC::LOG_INFO, "WASAPI: loopback started (%dHz × %dch, fmt=%s)",
            g.sample_rate, g.channels, is_float ? "FLT" : "S16");

    // 9) Capture loop: 轮询 GetBuffer; 无数据时 Sleep 10ms
    //    LOOPBACK 模式无 event 通知, 必须 polling
    while (!g.stop_flag.load()) {
        UINT32 packet_frames = 0;
        hr = capture->GetNextPacketSize(&packet_frames);
        if (FAILED(hr)) {
            CC::Log(CC::LOG_WARN, "WASAPI: GetNextPacketSize failed (0x%08lX), stop capture", (long)hr);
            break;
        }
        if (packet_frames == 0) {
            // 无数据 → sleep 10ms (LOOPBACK 模式约每 10-20ms 来一包)
            Sleep(10);
            continue;
        }

        // 拉一包数据
        BYTE*  data       = nullptr;
        UINT32 frame_cnt  = 0;
        DWORD  flags      = 0;
        hr = capture->GetBuffer(&data, &frame_cnt, &flags, nullptr, nullptr);
        if (FAILED(hr)) {
            CC::Log(CC::LOG_WARN, "WASAPI: GetBuffer failed (0x%08lX), stop capture", (long)hr);
            break;
        }
        const size_t bytes = (size_t)frame_cnt * g.bytes_per_frame;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            // 静音包: 自己填零写入 (保持时间线连续, 不让 ring 空着导致 audio 落后 video)
            std::vector<uint8_t> zeros(bytes, 0);
            ring_write(zeros.data(), bytes);
        } else if (data && bytes > 0) {
            ring_write(data, bytes);
        }
        g.total_captured_frames.fetch_add((int64_t)frame_cnt);
        capture->ReleaseBuffer(frame_cnt);
    }

    // 10) Stop + cleanup
    client->Stop();
    CC::Log(CC::LOG_INFO, "WASAPI: capture loop exited (total_captured=%lld frames)",
            (long long)g.total_captured_frames.load());
    fail_exit();
}

bool Start(int* out_sample_rate, int* out_channels, int* out_sample_fmt) {
    if (g.running.load()) {
        CC::Log(CC::LOG_WARN, "WASAPI::Start: already running");
        if (out_sample_rate) *out_sample_rate = g.sample_rate;
        if (out_channels)    *out_channels    = g.channels;
        if (out_sample_fmt)  *out_sample_fmt  = g.sample_fmt;
        return true;
    }
    g.stop_flag.store(false);
    g.write_pos = 0;
    g.read_pos  = 0;
    g.total_captured_frames.store(0);
    g.ring.assign(CaptureState::kRingBytes, 0);   // 一次性分配 16 MB

    // Spawn capture thread; 等待最多 200ms 让 thread 完成 init (避免 Start 返回时格式还未填)
    g.thread = std::thread(wasapi_thread_main_);
    for (int i = 0; i < 20 && !g.running.load() && g.thread.joinable(); ++i) {
        // 简单 spin-wait: thread init 失败时 g.running 永远 false, fail_exit 会让 thread 退出
        // 这里用 Sleep 而非 cv 避免死锁 (init 路径上 fail_exit 已设 running=false)
        Sleep(10);
    }
    if (!g.running.load()) {
        // Init 失败 → join thread (它应已退出 fail_exit)
        if (g.thread.joinable()) g.thread.join();
        g.ring.clear();
        g.ring.shrink_to_fit();
        return false;
    }

    if (out_sample_rate) *out_sample_rate = g.sample_rate;
    if (out_channels)    *out_channels    = g.channels;
    if (out_sample_fmt)  *out_sample_fmt  = g.sample_fmt;
    return true;
}

void Stop() {
    if (!g.thread.joinable() && !g.running.load()) return;
    g.stop_flag.store(true);
    if (g.thread.joinable()) g.thread.join();
    g.ring.clear();
    g.ring.shrink_to_fit();
    g.write_pos = 0;
    g.read_pos  = 0;
    g.sample_rate = 0;
    g.channels    = 0;
    g.sample_fmt  = 0;
    g.bytes_per_frame = 0;
}

#else  // 非 Windows: 静默 disable

bool Start(int*, int*, int*) {
    CC::Log(CC::LOG_INFO, "WASAPI::Start: audio capture only on Windows (skipped)");
    return false;
}

void Stop() {}

#endif

// ============================================================
// 跨平台共用 API
// ============================================================

bool IsRunning() {
    return g.running.load();
}

int Pull(uint8_t* dst, int max_frames) {
    if (!dst || max_frames <= 0 || !g.running.load() || g.bytes_per_frame == 0) return 0;
    const size_t want_bytes = (size_t)max_frames * g.bytes_per_frame;
    const size_t got_bytes  = ring_read(dst, want_bytes);
    return (int)(got_bytes / g.bytes_per_frame);
}

int Pending() {
    if (!g.running.load() || g.bytes_per_frame == 0) return 0;
    std::lock_guard<std::mutex> lk(g.mu);
    return (int)(ring_avail_bytes_unsafe() / g.bytes_per_frame);
}

int64_t TotalCaptured() {
    return g.total_captured_frames.load();
}

} // namespace AudioCaptureWASAPI
