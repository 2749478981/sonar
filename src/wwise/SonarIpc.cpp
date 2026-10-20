// ===========================================================================
//  SonarIpc.cpp —— 共享内存写入端 + 日志实现
// ===========================================================================
#include "SonarIpc.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace sonaraudio {

bool IpcWriter::Create(std::string& log) {
    const uint32_t sz = ShmSize();
    hMap_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sz, kShmName);
    if (!hMap_) {
        char b[160];
        std::snprintf(b, sizeof(b), "CreateFileMappingW 失败 gle=%lu\n", ::GetLastError());
        log += b;
        return false;
    }
    void* p = ::MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, sz);
    if (!p) {
        char b[160];
        std::snprintf(b, sizeof(b), "MapViewOfFile 失败 gle=%lu\n", ::GetLastError());
        log += b;
        ::CloseHandle(hMap_);
        hMap_ = nullptr;
        return false;
    }
    hdr_ = reinterpret_cast<SonarAudioHeader*>(p);
    // 全新初始化（同会话可能有残留，直接覆盖成干净状态）
    std::memset(hdr_, 0, sizeof(SonarAudioHeader));
    hdr_->magic     = kShmMagic;
    hdr_->version   = kShmVersion;
    hdr_->capacity  = kRingCapacity;
    hdr_->shm_size  = sz;
    hdr_->write_seq = 0;
    // 事件区清零（避免读端读到未初始化垃圾）
    std::memset(RingStart(hdr_), 0, sizeof(SonarAudioEvent) * kRingCapacity);

    char b[192];
    std::snprintf(b, sizeof(b), "共享内存已建：Local\\SonarAudioEvents_v1  size=%u bytes (%u slots)\n",
                  (unsigned)sz, (unsigned)kRingCapacity);
    log += b;
    return true;
}

void IpcWriter::Destroy() {
    if (hdr_) { ::UnmapViewOfFile(hdr_); hdr_ = nullptr; }
    if (hMap_) { ::CloseHandle(hMap_); hMap_ = nullptr; }
}

uint32_t IpcWriter::Push(uint32_t event_id, uint32_t playing_id, uint32_t game_obj_id,
                        uint32_t media_id, uint32_t flags,
                        const char* event_name, const char* bank_name) {
    if (!hdr_) return 0;

    const uint32_t s = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    SonarAudioEvent* ring = RingStart(hdr_);
    SonarAudioEvent& e = ring[(s - 1) % kRingCapacity];

    e.seq         = s;
    e.event_id    = event_id;
    e.playing_id  = playing_id;
    e.game_obj_id = game_obj_id;
    e.media_id    = media_id;
    e.flags       = flags;
    e.tick_ms     = ::GetTickCount64();
    CopyName(e.event_name, event_name, kNameLen);
    CopyName(e.bank_name, bank_name, kNameLen);

    // Release：保证上面的字段先于 write_seq 对读端可见
    hdr_->events_total = s;
    std::atomic_thread_fence(std::memory_order_release);
    hdr_->write_seq = s;
    return s;
}

// 定长字段拷贝：截断 + 保证结尾 0
void IpcWriter::CopyName(char* dst, const char* src, uint32_t cap) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    const size_t n = std::strlen(src);
    const size_t k = (n < cap - 1) ? n : (cap - 1);
    std::memcpy(dst, src, k);
    dst[k] = 0;
}

// ===========================================================================
//  日志
// ===========================================================================
namespace {
std::wstring g_logPath;
CRITICAL_SECTION g_logCs;
bool g_logInit = false;
std::atomic<uint64_t> g_logCount{0};
std::atomic<uint64_t> g_logWindowStart{0};
uint64_t g_logWindowCount = 0;

// 限速：每 1 秒最多 2000 行（防止异常情况下刷爆磁盘）
bool RateOk() {
    const uint64_t now = ::GetTickCount64();
    uint64_t start = g_logWindowStart.load();
    if (start == 0) { g_logWindowStart.store(now); return true; }
    if (now - start >= 1000) { g_logWindowStart.store(now); g_logWindowCount = 0; return true; }
    if (g_logWindowCount > 2000) return false;
    return true;
}
}  // namespace

void LogInit(const std::wstring& dir) {
    // 并入 WeaponSoundEnhance.dll 后用自己的日志名（GUI 侧兼容读取旧名）
    g_logPath = dir + L"WeaponSoundEnhance_wem.log";
    ::InitializeCriticalSection(&g_logCs);
    g_logInit = true;
}

void LogLine(const char* fmt, ...) {
    if (!g_logInit) return;
    if (!RateOk()) return;
    if (g_logCount.load() > 400000) return;  // 硬上限，防爆盘

    char body[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    SYSTEMTIME st; ::GetLocalTime(&st);
    char line[1200];
    int n = std::snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\r\n",
                          st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
    if (n <= 0) return;

    // ★ 体积上限：超过 512KB 就清空重写（不是"只停写"——那样文件会停在几十 MB）。
    //   读取端（插件/GUI）都有"文件变小 = 被截断"的检测，清了不会重放旧事件。
    {
        static uint64_t sLastCheck = 0;
        const uint64_t nowMs = ::GetTickCount64();
        if (nowMs - sLastCheck > 5000) {
            sLastCheck = nowMs;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (::GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fad) &&
                (((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow) > (512ull << 10)) {
                ::DeleteFileW(g_logPath.c_str());   // 清空重写；读取端会把 fileSize 变小当成截断
            }
        }
    }
    ::EnterCriticalSection(&g_logCs);
    HANDLE h = ::CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        ::WriteFile(h, line, (DWORD)n, &w, nullptr);
        ::CloseHandle(h);
        ++g_logWindowCount;
        g_logCount.fetch_add(1);
    }
    ::LeaveCriticalSection(&g_logCs);
}

}  // namespace sonaraudio
