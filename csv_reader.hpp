#pragma once
/**
 * csv_reader.hpp — 高性能数值 CSV 读取器（单头文件，零依赖）
 * High-performance numeric CSV reader: header-only, zero dependency,
 * Linux / macOS / Windows.
 *
 * 统一接口（Unified interface）—— 一个类，模板参数选类型：
 *
 *   // 默认：多线程开启（进程级共享线程池，惰性创建）
 *   CsvReader<double> r("data.csv");
 *   auto res = r.read();            // res.rows / res.cols / res.at(r,c) / res.data
 *
 *   // 关闭多线程（小文件 / 单线程场景）
 *   CsvReader<float> r2("data.csv", CsvOptions{false});   // parallel=false
 *
 *   // 表头 + 自定义分隔符 + 指定分片数
 *   CsvOptions opt;
 *   opt.has_header = true;
 *   opt.delimiter  = '\t';
 *   opt.threads    = 4;
 *   CsvReader<double> r3("data.tsv", opt);
 *
 *   // 流式读取（低内存）；回调内请勿再发起并行读取（共享池内会死锁）
 *   r.stream(2, [](const double* row, size_t ncols, size_t tid) { ... });
 *
 *   // 64 字节对齐（fftw 等需求）
 *   CsvReader<double, 64> r4("data.csv");
 *
 * 字段级解析（可选公共 API）：
 *   double v = parse_number<double>("1.5e3", "1.5e3" + 5);
 *
 * 设计（Design）：
 *   - MappedFile          mmap 零拷贝，跨平台
 *   - NumericTraits<T>    策略（Strategy）：幂表 / 指数上限 / NaN / Inf
 *   - ThreadPool          进程级共享线程池：所有实例共用同一个池，
 *                         首次并行读取时惰性创建（N 个实例 = 1 个池）
 *   - 每实例不持有线程池，实例轻量、析构无等待
 */

#include <vector>
#include <array>
#include <utility>
#include <functional>
#include <stdexcept>
#include <cstring>
#include <string>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <type_traits>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <exception>
#include <new>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX  // 防止 windows.h 的 min/max 宏污染 std::min/std::max
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

 // ─────────────────────────────────────────────
 // 1. 跨平台内存映射封装 / Cross-platform memory mapping
 // ─────────────────────────────────────────────
struct MappedFile {
    const char* data = nullptr;
    size_t      size = 0;

#ifdef _WIN32
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMap = nullptr;
#else
    int fd = -1;
#endif

    bool open(const char* path) {
#ifdef _WIN32
        // 优先按 UTF-8 解释路径；转换失败则回退 ANSI（GBK）路径
        // Try UTF-8 path first; fall back to ANSI (GBK) on failure.
        hFile = INVALID_HANDLE_VALUE;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
        if (wlen > 0) {
            std::wstring wpath(static_cast<size_t>(wlen) - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, path, -1, &wpath[0], wlen);
            hFile = CreateFileW(
                wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                nullptr
            );
        }
        if (hFile == INVALID_HANDLE_VALUE) {
            hFile = CreateFileA(
                path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                nullptr
            );
        }
        if (hFile == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER fs;
        if (!GetFileSizeEx(hFile, &fs)) { close(); return false; }
        size = static_cast<size_t>(fs.QuadPart);
        if (size == 0) { close(); return true; }  // 空文件合法 / empty file is legal

        hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!hMap) { close(); return false; }

        data = static_cast<const char*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
        return data != nullptr;
#else
        fd = ::open(path, O_RDONLY);
        if (fd < 0) return false;

        struct stat sb;
        if (fstat(fd, &sb) < 0) { close(); return false; }
        size = static_cast<size_t>(sb.st_size);
        if (size == 0) { close(); return true; }

        data = static_cast<const char*>(
            mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0)
            );
        if (data == MAP_FAILED) { data = nullptr; close(); return false; }

        madvise(const_cast<char*>(data), size, MADV_SEQUENTIAL);
        return true;
#endif
    }

    void close() {
#ifdef _WIN32
        if (data) { UnmapViewOfFile(data);  data = nullptr; }
        if (hMap) { CloseHandle(hMap);      hMap = nullptr; }
        if (hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile);
            hFile = INVALID_HANDLE_VALUE;
        }
#else
        if (data && size) { munmap(const_cast<char*>(data), size); data = nullptr; }
        if (fd >= 0) { ::close(fd); fd = -1; }
#endif
        size = 0;
    }

    ~MappedFile() { close(); }

    // 禁止拷贝 / non-copyable
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
};


// ─────────────────────────────────────────────
// 2. 对齐分配器 / Aligned allocator
//    满足 fftw 等对数据对齐的要求
//    用法：std::vector<double, AlignedAllocator<double, 64>> v;
// ─────────────────────────────────────────────
template <typename T, size_t Alignment = 16>
struct AlignedAllocator {
    static_assert(Alignment >= alignof(T), "Alignment must be >= alignof(T)");
    static_assert((Alignment & (Alignment - 1)) == 0, "Alignment must be a power of two");

    using value_type = T;

    AlignedAllocator() noexcept = default;
    template <typename U>
    AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept {}

    T* allocate(size_t n) {
        if (n == 0) return nullptr;
        // 头部多存一个指针槽（原始 malloc 地址），释放时还原
        // Keep one pointer slot before the aligned block for free().
        const size_t header = sizeof(void*);
        const size_t raw_bytes = n * sizeof(T) + header + Alignment - 1;
        void* raw = std::malloc(raw_bytes);
        if (!raw) throw std::bad_alloc();
        uintptr_t addr = reinterpret_cast<uintptr_t>(raw) + header;
        uintptr_t aligned = (addr + Alignment - 1) & ~(uintptr_t)(Alignment - 1);
        reinterpret_cast<void**>(aligned)[-1] = raw;
        return reinterpret_cast<T*>(aligned);
    }

    void deallocate(T* p, size_t) noexcept {
        if (!p) return;
        std::free(reinterpret_cast<void**>(p)[-1]);
    }

    template <typename U>
    struct rebind { using other = AlignedAllocator<U, Alignment>; };

    template <typename U>
    bool operator==(const AlignedAllocator<U, Alignment>&) const noexcept { return true; }
    template <typename U>
    bool operator!=(const AlignedAllocator<U, Alignment>&) const noexcept { return false; }
};


// ─────────────────────────────────────────────
// 3. 进程级共享线程池 / Process-wide shared thread pool
//    所有 CsvReader 实例共用同一个池，避免每个实例重复创建线程：
//    - 首次并行读取时惰性创建（Meyers 单例）
//    - 线程数全局设置（默认 = 硬件并发数），须在首次并行读取前调用
//    并发安全：互斥任务队列 + 每次 run() 独立计数，多个调用方可同时提交。
// ─────────────────────────────────────────────
class ThreadPool {
public:
    explicit ThreadPool(size_t nthreads) : stop_(false) {
        if (nthreads < 1) nthreads = 1;
        workers_.reserve(nthreads);
        for (size_t i = 0; i < nthreads; ++i)
            workers_.emplace_back([this] { worker_loop(); });
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    size_t worker_count() const { return workers_.size(); }

    // 并行执行 count 个任务（每个带索引 0..count-1），阻塞直到全部完成。
    // 任务抛出的异常被捕获到 errors[i]，由调用方在返回后检查并 rethrow。
    // 可被多个线程并发调用（每个调用有独立的完成计数）。
    template <typename TaskFn>
    void run(size_t count, TaskFn&& task, std::vector<std::exception_ptr>& errors) {
        errors.assign(count, nullptr);
        std::atomic<size_t> remaining{ count };
        std::mutex done_mu;
        std::condition_variable done_cv;

        {
            std::lock_guard<std::mutex> lk(mu_);
            for (size_t i = 0; i < count; ++i) {
                tasks_.emplace_back([this, i, &task, &errors, &remaining, &done_mu, &done_cv]() {
                    try {
                        task(i);
                    } catch (...) {
                        errors[i] = std::current_exception();
                    }
                    if (remaining.fetch_sub(1) == 1) {
                        std::lock_guard<std::mutex> lk2(done_mu);
                        done_cv.notify_all();
                    }
                });
            }
        }
        cv_.notify_all();

        std::unique_lock<std::mutex> lk(done_mu);
        done_cv.wait(lk, [&] { return remaining.load() == 0; });
    }

    // —— 进程级共享池 / shared process-wide pool ——
    // 首次调用时创建；之后所有调用方得到同一个实例。
    static ThreadPool& shared() {
        static ThreadPool pool(worker_setting());
        return pool;
    }

    // 全局线程数（默认 = 硬件并发数）。注意：须在第一次并行读取之前调用，
    // 池创建后该设置不再影响已创建的池。
    // Set the global worker count (default: hardware concurrency).
    // Must be called before the first parallel read.
    static void set_shared_workers(size_t n) {
        worker_setting() = (n < 1) ? 1 : n;
    }

    static size_t shared_workers() { return worker_setting(); }

private:
    static size_t& worker_setting() {
        static size_t n = [] {
            unsigned h = std::thread::hardware_concurrency();
            return h ? h : 1;
        }();
        return n;
    }

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_;

    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            try { task(); } catch (...) {}  // 防御：任务内部已捕获
        }
    }
};


// ─────────────────────────────────────────────
// 4. 数值策略（Strategy）与统一解析模板
//    Numeric strategy and unified parse templates.
//
//    NumericTraits<T> 提供 CsvReader<T> 需要的全部类型差异：
//    幂表、指数上限、NaN/Inf。新增数值类型（如 long double）只需
//    提供一份特化，无需修改读取器。
// ─────────────────────────────────────────────

// 主模板故意不定义：必须显式特化，避免误用未支持的类型。
// Primary template intentionally undefined: require explicit specialization.
template <typename T>
struct NumericTraits;

// float 特化 / float specialization
// 10^0 ~ 10^38 覆盖整个 float 范围（float 最大 3.4e38），编译期查表
template <>
struct NumericTraits<float> {
    using type = float;
    static constexpr size_t pow10_max = 38;   // float 指数上限 / float exponent limit
    static constexpr std::array<float, pow10_max + 1> pow10_table = {
        1e0f,1e1f,1e2f,1e3f,1e4f,1e5f,1e6f,1e7f,1e8f,1e9f,
        1e10f,1e11f,1e12f,1e13f,1e14f,1e15f,1e16f,1e17f,1e18f,1e19f,
        1e20f,1e21f,1e22f,1e23f,1e24f,1e25f,1e26f,1e27f,1e28f,1e29f,
        1e30f,1e31f,1e32f,1e33f,1e34f,1e35f,1e36f,1e37f,1e38f
    };
    static float quiet_nan() noexcept { return std::numeric_limits<float>::quiet_NaN(); }
    static float infinity() noexcept { return std::numeric_limits<float>::infinity(); }
};

// double 特化 / double specialization
// 10^0 ~ 10^308 完全覆盖 double 范围
template <>
struct NumericTraits<double> {
    using type = double;
    static constexpr size_t pow10_max = 308;  // double 指数上限 / double exponent limit
    static constexpr std::array<double, pow10_max + 1> pow10_table = {
        1e0,1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,
        1e10,1e11,1e12,1e13,1e14,1e15,1e16,1e17,1e18,1e19,
        1e20,1e21,1e22,1e23,1e24,1e25,1e26,1e27,1e28,1e29,
        1e30,1e31,1e32,1e33,1e34,1e35,1e36,1e37,1e38,1e39,
        1e40,1e41,1e42,1e43,1e44,1e45,1e46,1e47,1e48,1e49,
        1e50,1e51,1e52,1e53,1e54,1e55,1e56,1e57,1e58,1e59,
        1e60,1e61,1e62,1e63,1e64,1e65,1e66,1e67,1e68,1e69,
        1e70,1e71,1e72,1e73,1e74,1e75,1e76,1e77,1e78,1e79,
        1e80,1e81,1e82,1e83,1e84,1e85,1e86,1e87,1e88,1e89,
        1e90,1e91,1e92,1e93,1e94,1e95,1e96,1e97,1e98,1e99,
        1e100,1e101,1e102,1e103,1e104,1e105,1e106,1e107,1e108,1e109,
        1e110,1e111,1e112,1e113,1e114,1e115,1e116,1e117,1e118,1e119,
        1e120,1e121,1e122,1e123,1e124,1e125,1e126,1e127,1e128,1e129,
        1e130,1e131,1e132,1e133,1e134,1e135,1e136,1e137,1e138,1e139,
        1e140,1e141,1e142,1e143,1e144,1e145,1e146,1e147,1e148,1e149,
        1e150,1e151,1e152,1e153,1e154,1e155,1e156,1e157,1e158,1e159,
        1e160,1e161,1e162,1e163,1e164,1e165,1e166,1e167,1e168,1e169,
        1e170,1e171,1e172,1e173,1e174,1e175,1e176,1e177,1e178,1e179,
        1e180,1e181,1e182,1e183,1e184,1e185,1e186,1e187,1e188,1e189,
        1e190,1e191,1e192,1e193,1e194,1e195,1e196,1e197,1e198,1e199,
        1e200,1e201,1e202,1e203,1e204,1e205,1e206,1e207,1e208,1e209,
        1e210,1e211,1e212,1e213,1e214,1e215,1e216,1e217,1e218,1e219,
        1e220,1e221,1e222,1e223,1e224,1e225,1e226,1e227,1e228,1e229,
        1e230,1e231,1e232,1e233,1e234,1e235,1e236,1e237,1e238,1e239,
        1e240,1e241,1e242,1e243,1e244,1e245,1e246,1e247,1e248,1e249,
        1e250,1e251,1e252,1e253,1e254,1e255,1e256,1e257,1e258,1e259,
        1e260,1e261,1e262,1e263,1e264,1e265,1e266,1e267,1e268,1e269,
        1e270,1e271,1e272,1e273,1e274,1e275,1e276,1e277,1e278,1e279,
        1e280,1e281,1e282,1e283,1e284,1e285,1e286,1e287,1e288,1e289,
        1e290,1e291,1e292,1e293,1e294,1e295,1e296,1e297,1e298,1e299,
        1e300,1e301,1e302,1e303,1e304,1e305,1e306,1e307,1e308
    };
    static double quiet_nan() noexcept { return std::numeric_limits<double>::quiet_NaN(); }
    static double infinity() noexcept { return std::numeric_limits<double>::infinity(); }
};

// 完整版：支持科学计数法、NaN、Inf、Infinity、前导空格
// Full parser: scientific notation, NaN, Inf/Infinity (case-insensitive), leading whitespace.
template <typename T>
static inline T parse_number_full(const char*& p, const char* end) {
    constexpr size_t POW10_MAX = NumericTraits<T>::pow10_max;

    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p >= end) return NumericTraits<T>::quiet_nan();

    // 解析 NaN
    if (end - p >= 3) {
        if ((p[0] == 'N' || p[0] == 'n') && (p[1] == 'A' || p[1] == 'a') && (p[2] == 'N' || p[2] == 'n')) {
            p += 3;
            return NumericTraits<T>::quiet_nan();
        }
        // 解析 Inf / Infinity
        if ((p[0] == 'I' || p[0] == 'i') && (p[1] == 'N' || p[1] == 'n') && (p[2] == 'F' || p[2] == 'f')) {
            p += 3;
            if (end - p >= 5 && (p[0] == 'i' || p[0] == 'I') && (p[1] == 'n' || p[1] == 'N') &&
                (p[2] == 'i' || p[2] == 'I') && (p[3] == 't' || p[3] == 'T') && (p[4] == 'y' || p[4] == 'Y'))
                p += 5;
            return NumericTraits<T>::infinity();
        }
    }
    // 解析 -Inf / -Infinity
    if (end - p >= 4 && p[0] == '-' && (p[1] == 'I' || p[1] == 'i')) {
        if ((p[2] == 'N' || p[2] == 'n') && (p[3] == 'F' || p[3] == 'f')) {
            p += 4;
            if (end - p >= 5 && (p[0] == 'i' || p[0] == 'I') && (p[1] == 'n' || p[1] == 'N') &&
                (p[2] == 'i' || p[2] == 'I') && (p[3] == 't' || p[3] == 'T') && (p[4] == 'y' || p[4] == 'Y'))
                p += 5;
            return -NumericTraits<T>::infinity();
        }
    }

    T sign = static_cast<T>(1);
    if (*p == '-') { sign = static_cast<T>(-1); ++p; }
    else if (*p == '+') ++p;

    uint64_t int_part = 0;
    int int_digits = 0;
    while (p < end && (unsigned)(*p - '0') < 10 && int_digits < 18) {
        int_part = int_part * 10 + (*p++ - '0');
        int_digits++;
    }
    T val = static_cast<T>(int_part);
    // 超过 18 位：改用浮点累加，保留数量级
    // Beyond 18 digits, accumulate in floating point to keep the magnitude.
    while (p < end && (unsigned)(*p - '0') < 10) {
        val = val * static_cast<T>(10) + static_cast<T>(*p++ - '0');
        int_digits++;
    }
    int frac_digits = 0;

    // 解析小数部分 / fractional part
    if (p < end && *p == '.') {
        ++p;
        uint64_t frac = 0;
        while (p < end && (unsigned)(*p - '0') < 10 && frac_digits < 18) {
            frac = frac * 10 + (*p++ - '0');
            frac_digits++;
        }
        while (p < end && (unsigned)(*p - '0') < 10) ++p;
        val += static_cast<T>(frac) / NumericTraits<T>::pow10_table[frac_digits];
    }

    // 解析科学计数法（指数上限由 NumericTraits<T>::pow10_max 决定）
    int exp = 0, exp_sign = 1;
    if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        if (p < end && *p == '-') { exp_sign = -1; ++p; }
        else if (p < end && *p == '+') ++p;
        // 累加原始指数（上限保护，防超长指数导致 int 溢出 UB；超限后只消费数字）
        while (p < end && (unsigned)(*p - '0') < 10) {
            int d = (*p++ - '0');
            if (exp <= 100000) exp = exp * 10 + d;
        }
        int total_exp = exp_sign * exp;
        // 真实量级 = 数值本身的十进制指数 + 指数部分
        // （必须结合整数位数判断：大整数 × 负指数可能仍是合法值）
        if (val != static_cast<T>(0)) {
            int magnitude = (int)std::floor(std::log10(std::fabs(val))) + total_exp;
            if (total_exp > 0) {
                if (total_exp > (int)POW10_MAX || magnitude > (int)POW10_MAX)
                    val = NumericTraits<T>::infinity();
                else
                    val *= NumericTraits<T>::pow10_table[total_exp];
            }
            else if (total_exp < 0) {
                int a = -total_exp;
                if (magnitude < -(int)POW10_MAX) {
                    val = static_cast<T>(0);  // 真实下溢（subnormal 区精度不可保证）
                }
                else if (a <= (int)POW10_MAX) {
                    val /= NumericTraits<T>::pow10_table[a];
                }
                else {
                    // 指数超表但量级仍合法：分步除法
                    while (a > (int)POW10_MAX) { val /= NumericTraits<T>::pow10_table[POW10_MAX]; a -= (int)POW10_MAX; }
                    if (a > 0) val /= NumericTraits<T>::pow10_table[a];
                }
            }
        }
        // val == 0：任何指数乘除结果保持 0（如 "0e999"）
    }

    // 无有效数字，返回 NaN
    if (int_digits == 0 && frac_digits == 0)
        return NumericTraits<T>::quiet_nan();

    return sign * val;
}

// 快速版：无科学计数法 / Fast path: no scientific notation.
template <typename T>
static inline T parse_number_fast(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p >= end) return NumericTraits<T>::quiet_nan();

    T sign = static_cast<T>(1);
    if (*p == '-') { sign = static_cast<T>(-1); ++p; }
    else if (*p == '+') ++p;

    uint64_t int_part = 0;
    int int_digits = 0;
    while (p < end && (unsigned)(*p - '0') < 10 && int_digits < 18) {
        int_part = int_part * 10 + (*p++ - '0');
        int_digits++;
    }
    T val = static_cast<T>(int_part);
    // 超过 18 位：改用浮点累加，避免 uint64 溢出回绕
    while (p < end && (unsigned)(*p - '0') < 10) {
        val = val * static_cast<T>(10) + static_cast<T>(*p++ - '0');
        int_digits++;
    }

    int frac_digits = 0;
    if (p < end && *p == '.') {
        ++p;
        uint64_t frac = 0;
        while (p < end && (unsigned)(*p - '0') < 10 && frac_digits < 18) {
            frac = frac * 10 + (*p++ - '0');
            frac_digits++;
        }
        while (p < end && (unsigned)(*p - '0') < 10) ++p;
        val += static_cast<T>(frac) / NumericTraits<T>::pow10_table[frac_digits];
    }

    // 无有效数字（"abc"、"-"、空字段等）→ NaN，与 full 版语义一致
    if (int_digits == 0 && frac_digits == 0)
        return NumericTraits<T>::quiet_nan();

    return sign * val;
}

// 统一接口：自动判断是否使用科学计数法 / auto-dispatch between full and fast
template <typename T>
static inline T parse_number(const char* p, const char* end) {
    const char* s = p;
    while (s < end && (*s == ' ' || *s == '\t')) ++s;
    if (s >= end) return NumericTraits<T>::quiet_nan();

    bool has_exp = false;
    bool special = false;
    const char* scan = s;
    while (scan < end) {
        if (*scan == 'e' || *scan == 'E') { has_exp = true; break; }
        // 手写数字判断，避免 std::isdigit 对负 char 的 UB
        if ((unsigned)(*scan - '0') >= 10 && *scan != '.' && *scan != '-' && *scan != '+') break;
        ++scan;
    }
    // NaN / Inf 前缀必须走 full 版（full 才支持 nan/inf/infinity 大小写与正负号）
    if (!has_exp) {
        const char* t = s;
        if (t < end && (*t == '+' || *t == '-')) ++t;
        if (t < end && (*t == 'N' || *t == 'n' || *t == 'I' || *t == 'i')) special = true;
    }

    if (has_exp || special)
        return parse_number_full<T>(p, end);
    else
        return parse_number_fast<T>(s, end);
}

// 解析一行为 T 数组 / Parse one row into an array of T.
template <typename T>
static inline size_t parse_number_row(
    const char* row, const char* row_end,
    T* out, size_t max_cols, char delimiter = ',')
{
    size_t col = 0;
    const char* p = row;
    while (p <= row_end && col < max_cols) {
        const char* fs = p;
        while (p < row_end && *p != delimiter) ++p;
        out[col++] = parse_number<T>(fs, p);
        if (p < row_end) ++p;
        else break;
    }
    return col;
}


// ─────────────────────────────────────────────
// 5. 统一读取选项 / Unified read options
// ─────────────────────────────────────────────
struct CsvOptions {
    bool   parallel   = true;   // 多线程并行读取（默认开启）
    bool   has_header = false;  // 首行为表头（数据从第二行开始）
    char   delimiter  = ',';    // 字段分隔符
    size_t threads    = 0;      // 并行分片数；0 = 全局共享池线程数（仅 parallel=true 时生效）
};


// ─────────────────────────────────────────────
// 6. 统一数值读取器 / Unified numeric CSV reader
//    T 选择读取类型（float / double 等，配合 NumericTraits 特化）；
//    Align > 0 时结果数据按 Align 字节对齐（fftw 等），Align = 0 为标准分配。
//
//    多线程：默认开启。并行读取把文件按行切成若干片，提交给进程级共享
//    线程池（ThreadPool::shared()）执行——所有实例共用一个池。
//    parallel = false 时在调用线程内联解析，不创建任何线程。
//
//    线程安全：不同实例可并发 read()/stream()（共享池本身并发安全）；
//    同一实例不要并发调用。stream 回调内请勿再发起并行读取（避免死锁）。
// ─────────────────────────────────────────────
template <typename T, size_t Align = 0>
class CsvReader {
public:
    using value_type = T;
    using DataVector = typename std::conditional<
        Align == 0,
        std::vector<T>,
        std::vector<T, AlignedAllocator<T, Align>>
    >::type;

    // 读取结果 / read result (row-major flat storage)
    struct ReadResult {
        DataVector data;
        size_t rows = 0;
        size_t cols = 0;
        T& at(size_t r, size_t c) { return data[r * cols + c]; }
        const T& at(size_t r, size_t c) const { return data[r * cols + c]; }
    };

    explicit CsvReader(const char* path, const CsvOptions& opts = CsvOptions())
        : opts_(opts)
    {
        if (!file_.open(path))
            throw std::runtime_error(std::string("Cannot open: ") + path);
        strip_bom();  // 剥离 UTF-8 BOM（EF BB BF），否则首字段解析失败

        // 分片数：parallel 关闭时恒为 1（单线程整文件扫描）
        nchunks_ = opts_.parallel
            ? (opts_.threads ? opts_.threads : ThreadPool::shared_workers())
            : 1;
    }

    // 自动检测列数（取首个数据行的分隔符数量 + 1）
    size_t detect_cols() {
        if (!file_.data || file_.size == 0) return 0;
        const char* p = file_.data;
        const char* end = file_.data + file_.size;

        if (opts_.has_header) {
            const char* nl = (const char*)memchr(p, '\n', end - p);
            p = nl ? nl + 1 : end;
        }
        const char* le = (const char*)memchr(p, '\n', end - p);
        if (!le) le = end;
        const char* re = (le > p && le[-1] == '\r') ? le - 1 : le;

        while (re > p && *(re - 1) == opts_.delimiter) --re;

        size_t cnt = 1;
        for (const char* q = p; q < re; ++q) {
            if (*q == opts_.delimiter) ++cnt;
        }
        return cnt;
    }

    // 批量读取全部数据到内存 / read everything into memory
    ReadResult read(size_t ncols = 0) {
        ReadResult res;
        read_impl(res, ncols);
        return res;
    }

    // 读取到调用方持有的结果对象：out.data 的容量跨调用复用
    // Read into a caller-owned result; capacity is reused across calls.
    void read_into(ReadResult& out, size_t ncols = 0) {
        read_impl(out, ncols);
    }

    // 流式读取（回调处理每一行，低内存）。回调内请勿再发起并行读取。
    // Streaming read: callback per row. Do not start parallel reads inside cb.
    using RowCallback = std::function<void(const T*, size_t, size_t)>;
    size_t stream(size_t ncols, const RowCallback& cb) {
        if (!file_.data || file_.size == 0) return 0;
        if (ncols == 0) ncols = detect_cols_cached();

        const auto& ranges = split_ranges_cached();
        if (ranges.empty()) return 0;
        size_t nr = ranges.size();

        if (chunks_.size() < nr) chunks_.resize(nr);
        for (auto& c : chunks_) {
            if (c.buf.size() < ncols) c.buf.resize(ncols);
        }

        std::atomic<size_t> total{ 0 };
        auto task = [&](size_t tid) {
            const char* p = ranges[tid].first;
            const char* end = ranges[tid].second;
            auto& buf = chunks_[tid].buf;
            size_t loc = 0;

            while (p < end) {
                const char* le = (const char*)std::memchr(p, '\n', end - p);
                if (!le) le = end;
                const char* re = (le > p && le[-1] == '\r') ? le - 1 : le;

                if (re > p) {
                    size_t n = parse_number_row<T>(p, re, buf.data(), ncols, opts_.delimiter);
                    for (size_t i = n; i < ncols; ++i) buf[i] = NumericTraits<T>::quiet_nan();
                    cb(buf.data(), ncols, tid);
                    loc++;
                }
                p = le + 1;
            }
            total += loc;
        };

        run_chunks(nr, task);
        return total.load();
    }

    // 当前实例的并行分片数 / chunk count of this instance
    size_t chunk_count() const { return nchunks_; }

private:
    MappedFile file_;
    CsvOptions opts_;
    size_t nchunks_;

    struct Chunk {
        std::vector<T> data;    // 解析结果缓冲（容量跨调用保留）
        std::vector<T> buf;     // 行缓冲（容量跨调用保留）
        size_t rows = 0;
    };
    std::vector<Chunk> chunks_;
    std::vector<std::pair<const char*, const char*>> cached_ranges_;
    size_t cached_cols_ = 0;
    size_t cached_est_ = 0;
    bool cols_cached_ = false;
    bool ranges_cached_ = false;
    bool est_cached_ = false;

    void strip_bom() noexcept {
        if (file_.size >= 3 &&
            (unsigned char)file_.data[0] == 0xEF &&
            (unsigned char)file_.data[1] == 0xBB &&
            (unsigned char)file_.data[2] == 0xBF) {
            file_.data += 3;
            file_.size -= 3;
        }
    }

    // 执行分片任务：parallel 且多于一片 → 提交共享池；否则调用线程内联执行
    // Execute chunk tasks: submit to the shared pool when parallel with >1
    // chunk; otherwise run inline on the calling thread (no pool, no threads).
    template <typename TaskFn>
    void run_chunks(size_t nr, TaskFn& task) {
        if (opts_.parallel && nr > 1) {
            std::vector<std::exception_ptr> errors(nr, nullptr);
            ThreadPool::shared().run(nr, task, errors);
            for (auto& e : errors) if (e) std::rethrow_exception(e);
        } else {
            for (size_t i = 0; i < nr; ++i) task(i);
        }
    }

    // detect_cols / estimate_rows / split_ranges 结果缓存（文件已 mmap 且不变，缓存安全）
    size_t detect_cols_cached() {
        if (!cols_cached_) { cached_cols_ = detect_cols(); cols_cached_ = true; }
        return cached_cols_;
    }
    size_t estimate_rows_cached() {
        if (!est_cached_) { cached_est_ = estimate_rows(); est_cached_ = true; }
        return cached_est_;
    }
    const std::vector<std::pair<const char*, const char*>>& split_ranges_cached() {
        if (!ranges_cached_) { cached_ranges_ = split_ranges(); ranges_cached_ = true; }
        return cached_ranges_;
    }

    // 共享的读取实现：read() 构造临时结果，read_into() 复用调用方缓冲
    void read_impl(ReadResult& res, size_t ncols) {
        if (!file_.data || file_.size == 0) { res = ReadResult{}; return; }
        if (ncols == 0) ncols = detect_cols_cached();

        const auto& ranges = split_ranges_cached();
        if (ranges.empty()) { res = ReadResult{}; return; }  // 无数据行（如只有表头）
        size_t nr = ranges.size();

        if (chunks_.size() < nr) chunks_.resize(nr);
        size_t est = estimate_rows_cached() / nr + 1024;
        for (auto& c : chunks_) {
            c.rows = 0;
            c.data.clear();
            if (c.data.capacity() < est * ncols) c.data.reserve(est * ncols);
            if (c.buf.size() < ncols) c.buf.resize(ncols);
        }

        auto task = [&](size_t tid) {
            auto& ch = chunks_[tid];
            const char* p = ranges[tid].first;
            const char* end = ranges[tid].second;

            while (p < end) {
                const char* le = (const char*)std::memchr(p, '\n', end - p);
                if (!le) le = end;
                const char* re = (le > p && le[-1] == '\r') ? le - 1 : le;

                if (re > p) {
                    size_t n = parse_number_row<T>(p, re, ch.buf.data(), ncols, opts_.delimiter);
                    for (size_t i = n; i < ncols; ++i) ch.buf[i] = NumericTraits<T>::quiet_nan();
                    ch.data.insert(ch.data.end(), ch.buf.begin(), ch.buf.end());
                    ch.rows++;
                }
                p = le + 1;
            }
        };

        run_chunks(nr, task);

        res.cols = ncols;
        res.rows = 0;
        size_t total = 0;
        for (auto& c : chunks_) { res.rows += c.rows; total += c.data.size(); }
        res.data.resize(total);

        size_t off = 0;
        for (auto& c : chunks_) {
            std::memcpy(res.data.data() + off, c.data.data(), c.data.size() * sizeof(T));
            off += c.data.size();
        }
    }

    // 预估行数（优化内存分配）/ estimate row count for allocation hints
    size_t estimate_rows() {
        size_t sample = std::min(file_.size, (size_t)65536);
        size_t nl = 0;
        for (size_t i = 0; i < sample; ++i) if (file_.data[i] == '\n') nl++;
        if (nl == 0) return 1024;
        return (size_t)(file_.size / ((double)sample / nl)) + 1;
    }

    // 按换行符对齐分片（保证每个线程处理完整行）
    static const char* align_to_newline(const char* p, const char* end) {
        while (p < end && *p != '\n') ++p;
        return (p < end) ? p + 1 : end;
    }

    // 分割文件为若干处理区间 / split file into per-chunk ranges
    std::vector<std::pair<const char*, const char*>> split_ranges() {
        const char* base = file_.data;
        const char* end = base + file_.size;
        const char* cur = base;

        // 跳过表头 / skip header
        if (opts_.has_header) {
            const char* nl = (const char*)std::memchr(cur, '\n', end - cur);
            cur = nl ? nl + 1 : end;
        }

        size_t sz = end - cur;
        size_t chunk = sz / nchunks_;
        std::vector<std::pair<const char*, const char*>> ranges;

        for (size_t i = 0; i < nchunks_; ++i) {
            if (cur >= end) break;
            const char* next = (i == nchunks_ - 1) ? end : align_to_newline(cur + chunk, end);
            ranges.emplace_back(cur, next);
            cur = next;
        }
        return ranges;
    }
};
