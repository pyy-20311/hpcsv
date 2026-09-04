# hpcsv

> 高性能数值 CSV 读取器（单头文件 `csv_reader.hpp`）· 零依赖 · C++17
> High-performance numeric CSV reader for float/double data — header-only, zero dependency.

`hpcsv` 是一个面向**数值型 CSV**（如 `double,double,double...`）的跨平台读取器：
mmap 零拷贝 + 进程级共享线程池并行解析，一个统一接口同时覆盖 float / double、
单线程 / 多线程、任意分隔符、表头、内存对齐（FFTW）等场景。整个库只有一个
头文件 `csv_reader.hpp`，复制即可引入。

本库**不解析字符串 / 带引号字段**（RFC 4180 引号模式不支持），专注把纯数值 CSV
以最高吞吐读成**行优先的连续内存数组**——可直接以裸指针交给 FFTW / BLAS / SIMD
等 C 接口使用（见下文「以指针形式使用数据」）。

---

## 特性 / Features

- 📄 **单头文件，零依赖**：仅需 `#include "csv_reader.hpp"`，无需链接第三方库（仅 `-pthread`）
- ⚡ **mmap 零拷贝**：文件直接映射，解析全程不复制文件内容（Linux / macOS / Windows）
- 🧵 **多线程并行**（默认开启）：按行对齐分片 → 线程池并行解析；可一键关闭走单线程内联路径
- 🏊 **进程级共享线程池**：N 个 `CsvReader` 实例全程只创建**一次**线程池（首次并行读取时惰性创建）
- 🔢 **float / double 统一接口**：模板参数 `T` 选类型，解析逻辑零重复
- 🧮 **完整数值语义**：科学计数法、`nan`/`inf`/`infinity`（大小写不敏感）、超长整数、
  超长指数防溢出、上溢→±inf、下溢→0
- 🧭 **格式健壮**：UTF-8 BOM 自动剥离、CRLF / LF、无结尾换行、空行跳过、
  空字段/非法字段→NaN、短行 NaN 补齐、超宽行截断
- 📐 **可选内存对齐**：`CsvReader<T, Align>`（fftw 等 SIMD 需求）

## 需求 / Requirements

- C++17 编译器（GCC ≥ 7 / Clang ≥ 5 / MSVC 2017 15.7+）
- 多线程路径需要链接 pthread（Linux/macOS）：`-pthread`
- Windows 代码路径（CreateFileMapping + UTF-8 路径回退 ANSI）已实现，
  当前在 Linux（GCC 8.5 / Clang）上持续验证

---

## 快速开始 / Quick Start

```cpp
#include "csv_reader.hpp"

// 1. 批量读取（默认多线程开启）
CsvReader<double> reader("data.csv");
auto res = reader.read();        // res.data / res.rows / res.cols / res.at(r, c)
std::printf("rows=%zu cols=%zu first=%g\n", res.rows, res.cols, res.at(0, 0));

// 2. 关闭多线程（小文件 / 需确定性单线程）
CsvReader<float> reader2("data.csv", CsvOptions{false /* parallel */});

// 3. 表头 + 制表符 + 指定分片数
CsvOptions opt;
opt.has_header = true;
opt.delimiter  = '\t';
opt.threads    = 4;              // 0 = 自动（全局共享池线程数）
CsvReader<double> reader3("data.tsv", opt);

// 4. 流式读取（低内存，逐行回调）
CsvReader<double> reader4("data.csv");
reader4.stream(2, [](const double* row, size_t ncols, size_t /*tid*/) {
    // 处理这一行；返回前拷贝需要保留的数据
});

// 5. 64 字节对齐（fftw 等需求）
CsvReader<double, 64> reader5("data.csv");
auto aligned = reader5.read();   // aligned.data.data() 满足 64 字节对齐
```

编译：

```bash
g++ -std=c++17 -O2 -pthread -o demo demo.cpp
```

> C++20 起可直接用指定初始化：`CsvReader<double> r("f.csv", CsvOptions{.parallel = false});`

---

## API 一览 / API Overview

### `struct CsvOptions`

| 成员 | 默认 | 说明 |
|---|---|---|
| `bool parallel` | `true` | 多线程并行读取（默认开启） |
| `bool has_header` | `false` | 首行为表头，数据从第二行开始 |
| `char delimiter` | `','` | 字段分隔符（单字符，支持 `\t` `;` 空格等） |
| `size_t threads` | `0` | 并行分片数；`0` = 全局共享池线程数（仅 `parallel=true` 时生效） |

### `template <typename T, size_t Align = 0> class CsvReader`

- `T`：数值类型，需有 `NumericTraits<T>` 特化（内置 `float` / `double`）
- `Align`：结果数据对齐字节数；`0` = 标准分配，`>0` = N 字节对齐（2 的幂）

| 方法 | 说明 |
|---|---|
| `CsvReader(const char* path, const CsvOptions& = {})` | 打开文件（失败抛 `std::runtime_error`），自动剥离 UTF-8 BOM |
| `ReadResult read(size_t ncols = 0)` | 批量读入内存；`ncols=0` 时按首行数据自动检测列数 |
| `void read_into(ReadResult&, size_t ncols = 0)` | 读入调用方持有的结果对象（容量跨调用复用） |
| `size_t stream(size_t ncols, const RowCallback&)` | 流式读取，回调 `(const T*, ncols, tid)`，返回行数 |
| `size_t detect_cols()` | 自动检测列数（首个数据行分隔符数 + 1） |
| `size_t chunk_count()` | 当前实例的并行分片数（`parallel=false` 时恒为 1） |

`ReadResult`：`data`（行优先扁平数组，`std::vector<T>` 或对齐版本）、`rows`、`cols`、
`at(r, c)` 二维访问。

### 全局线程池调节

```cpp
ThreadPool::set_shared_workers(8);          // 须在第一次并行读取之前调用
// 之后：ThreadPool::shared() / shared_workers() / worker_count()
```

所有实例共用进程级单例池，首次并行读取时惰性创建；线程数默认 = 硬件并发数。

### 字段级解析（可选公共 API）

```cpp
const char* p = text.data();
double v = parse_number<double>(p, p + text.size());   // 单字段
size_t n = parse_number_row<double>(row, row_end, out, max_cols, ',');  // 单行
```

### 解析语义 / Parsing semantics

| 输入 | 结果 |
|---|---|
| `123` `-1.5` `+2` `.5` `1e10` `-2.5e-3` | 对应数值 |
| `nan` `NaN` `inf` `Infinity` `-inf` `-infinity` | ±NaN / ±inf（大小写不敏感） |
| 空字段、`abc`、`-`、`.`、`e5` 等无有效数字 | `NaN` |
| `1abc` `1.2.3` `1 5`（部分可解析） | 解析到首个非法字符（strtod 风格），返回 `1.0`/`1.2`/`1.0` |
| 指数上溢（如 `1e999`） | ±inf |
| 指数下溢（如 `1e-999`） | `0.0`（float 在 <1e-38 量级即归零） |
| 超过 18 位的整数 | 浮点累加保数量级，相对误差 ≤ 浮点精度 |

> 引号 `"..."` 不被特殊处理：会按普通字符参与解析（如 `"1.5"` 得到 `1.5`，`"abc"` 得到 NaN）。

---

## 以指针形式使用数据 / Use the data as raw pointers

`read()` 的结果是**行优先的连续堆内存**（解析时已从 mmap 映射区拷入普通 vector，
与 reader / 文件的生命周期无关），因此可以零拷贝地以裸指针交给任意 C / C++ API
（FFTW、BLAS/LAPACK、SIMD、绘图库等）：

```cpp
CsvReader<double, 64> reader("data.csv", CsvOptions{false});
auto res = reader.read();                // 结果对象要活得比指针久

double* p = res.data.data();             // 连续缓冲首地址（此处 64 字节对齐）
size_t  n = res.data.size();             // 元素总数 = rows * cols

process(p, n);                           // 普通 C 接口
analyze(static_cast<const double*>(p), n);  // 只读接口传 const 指针
```

行优先索引：元素 `(r, c)` 位于 `p[r * cols + c]`（等价于 `res.at(r, c)`），
行间距 = `cols` 个 double，列间距 = 1。

**指针生命周期规则：**

- `read()` 的数据已属于 `res.data`（独立堆内存）——`reader` 销毁后指针仍有效；
  只要 `res` 存活，指针可安全跨线程使用（读取结束后数据不再被修改）
- `stream()` 回调里的行指针指向 reader 内部的复用缓冲，**仅在回调内有效**，
  需要保留时请在回调内自行拷贝
- 若用 `std::move(res.data)` 转移所有权，务必保持新 vector 存活

### 接 FFTW（实测示例）

`CsvReader<double, 64>` 的 `data()` 指针满足 FFTW 的 SIMD 对齐要求（16/32/64 字节），
可直接作为变换输入/输出，无需 `fftw_malloc`：

```cpp
#include "csv_reader.hpp"
#include <fftw3.h>

// 单列 N 点信号 → 实→复 FFT
CsvReader<double, 64> reader("signal.csv", CsvOptions{false});
auto res = reader.read();
size_t n = res.rows;

fftw_complex* out = fftw_alloc_complex(n / 2 + 1);
fftw_plan p = fftw_plan_dft_r2c_1d((int)n, res.data.data(), out, FFTW_ESTIMATE);
fftw_execute(p);
// ... 用 out[0..n/2]；记得 fftw_destroy_plan(p); fftw_free(out);
```

编译链接（FFTW 安装在前缀 `~/projects/fftw` 时）：

```bash
g++ -std=c++17 -O2 -pthread \
    -I"$HOME/projects/fftw/include" -L"$HOME/projects/fftw/lib" -lfftw3 \
    -o demo demo.cpp
LD_LIBRARY_PATH="$HOME/projects/fftw/lib" ./demo
```

---

## 设计 / Design

```
csv_reader.hpp（单头文件，按依赖顺序）
├── MappedFile          跨平台内存映射（mmap / CreateFileMapping）
├── AlignedAllocator    对齐分配器（Align > 0 时使用）
├── ThreadPool          进程级共享线程池（Meyers 单例，惰性创建）
│     └─ 所有 CsvReader 实例共用，N 实例 = 1 池
├── NumericTraits<T>    策略（Strategy）：幂表 / 指数上限 / NaN / Inf
├── parse_number<T>     统一解析模板（full 科学计数法 / fast 无指数 自动分流）
└── CsvReader<T, Align> 统一读取器：分片 → 并行解析 → 汇总
```
- **线程模型**：`parallel=true` 把文件按行切成 `threads` 片提交共享池；
  `parallel=false` 在调用线程内联解析，完全不创建线程
- **线程安全**：不同实例可并发 `read()`/`stream()`（共享池并发安全）；
  同一实例勿并发调用；**stream 回调内勿再发起并行读取**（共享池内会死锁）
- **内存复用**：列数 / 行数估算 / 分片区间 / chunk 缓冲跨多次调用缓存

### 扩展新数值类型

新增 `long double` 等类型只需提供一份 `NumericTraits` 特化 + 直接使用 `CsvReader<long double>`：

```cpp
template <> struct NumericTraits<long double> {
    using type = long double;
    static constexpr size_t pow10_max = 4932;   // long double 指数上限（按平台）
    static constexpr std::array<long double, pow10_max + 1> pow10_table = {...};
    static long double quiet_nan() noexcept { return std::numeric_limits<long double>::quiet_NaN(); }
    static long double infinity() noexcept { return std::numeric_limits<long double>::infinity(); }
};
```

---
