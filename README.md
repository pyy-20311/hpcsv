# hpcsv

[English](README.md) | [简体中文](README_zh.md)(aa.md)

## Fast reading of large-scale (GB-level) CSV data — e.g. test scenarios for radar signal processing algorithms.

> High-performance numeric CSV reader for float/double data — header-only (`csv_reader.hpp`), zero dependency, C++17.

`hpcsv` is a cross-platform reader for **numeric CSV** files (e.g. `double,double,double...`):
mmap zero-copy plus a process-wide shared thread pool for parallel parsing. One unified
interface covers float / double, single- / multi-threading, any delimiter, header rows,
and memory alignment (FFTW). The whole library is a single header, `csv_reader.hpp` —
drop it into your project and `#include` it.

Strings and quoted fields (RFC 4180) are **not** supported. The library focuses on
reading pure numeric CSV into **row-major contiguous memory** at maximum throughput,
ready to be handed to C interfaces such as FFTW / BLAS / SIMD as raw pointers
(see [Using the data as raw pointers](#using-the-data-as-raw-pointers)).

---

## Features

- 📄 **Header-only, zero dependency** — just `#include "csv_reader.hpp"`; nothing else to link except `-pthread`
- ⚡ **mmap zero-copy** — the file is mapped directly; file content is never copied during parsing (Linux / macOS / Windows)
- 🧵 **Multithreaded by default** — line-aligned chunking → parallel parsing on a thread pool; disable with one flag to fall back to a single-threaded inline path
- 🏊 **Process-wide shared thread pool** — N `CsvReader` instances create only **one** pool in total (created lazily on the first parallel read)
- 🔢 **Unified float / double interface** — pick the type with template parameter `T`; parsing logic is shared, zero duplication
- 🧮 **Full numeric semantics** — scientific notation, `nan` / `inf` / `infinity` (case-insensitive), >18-digit integers, exponent-overflow protection, overflow → ±inf, underflow → 0
- 🧭 **Format robustness** — UTF-8 BOM auto-stripped, CRLF / LF, missing trailing newline, blank lines skipped, empty / invalid fields → NaN, short rows padded with NaN, over-wide rows truncated
- 📐 **Optional memory alignment** — `CsvReader<T, Align>` (FFTW / SIMD needs)

## Requirements

- C++17 compiler (GCC ≥ 7 / Clang ≥ 5 / MSVC 2017 15.7+)
- Link `pthread` on Linux/macOS: `-pthread`
- The Windows code path (CreateFileMapping + UTF-8 paths with ANSI fallback) is implemented; currently continuously verified on Linux (GCC 8.5 / Clang)

---

## Quick Start

```cpp
#include "csv_reader.hpp"

// 1. Batch read (multithreading on by default)
CsvReader<double> reader("data.csv");
auto res = reader.read();        // res.data / res.rows / res.cols / res.at(r, c)
std::printf("rows=%zu cols=%zu first=%g\n", res.rows, res.cols, res.at(0, 0));

// 2. Disable multithreading (small files / deterministic single thread)
CsvReader<float> reader2("data.csv", CsvOptions{false /* parallel */});

// 3. Header row + tab delimiter + explicit chunk count
CsvOptions opt;
opt.has_header = true;
opt.delimiter  = '\t';
opt.threads    = 4;              // 0 = auto (shared pool size)
CsvReader<double> reader3("data.tsv", opt);

// 4. Streaming read (low memory, per-row callback)
CsvReader<double> reader4("data.csv");
reader4.stream(2, [](const double* row, size_t ncols, size_t /*tid*/) {
    // consume the row; copy anything you need to keep
});

// 5. 64-byte alignment (FFTW etc.)
CsvReader<double, 64> reader5("data.csv");
auto aligned = reader5.read();   // aligned.data.data() is 64-byte aligned
```

Build:

```bash
g++ -std=c++17 -O2 -pthread -o demo demo.cpp
```

> With C++20 you can use designated initializers:
> `CsvReader<double> r("f.csv", CsvOptions{.parallel = false});`

---

## API Overview

### `struct CsvOptions`

| Member | Default | Description |
|---|---|---|
| `bool parallel` | `true` | Parallel reading (on by default) |
| `bool has_header` | `false` | First row is a header; data starts on the second row |
| `char delimiter` | `','` | Field delimiter (a single char; `\t`, `;`, space, ...) |
| `size_t threads` | `0` | Parallel chunk count; `0` = shared pool size (only when `parallel=true`) |

### `template <typename T, size_t Align = 0> class CsvReader`

- `T`: the numeric type; requires a `NumericTraits<T>` specialization (built in: `float`, `double`)
- `Align`: alignment in bytes of the result buffer; `0` = standard allocation, `>0` = N-byte alignment (power of two)

| Method | Description |
|---|---|
| `CsvReader(const char* path, const CsvOptions& = {})` | Opens the file (throws `std::runtime_error` on failure); strips the UTF-8 BOM |
| `ReadResult read(size_t ncols = 0)` | Reads everything into memory; `ncols = 0` auto-detects from the first data row |
| `void read_into(ReadResult&, size_t ncols = 0)` | Reads into a caller-owned result (capacity is reused across calls) |
| `size_t stream(size_t ncols, const RowCallback&)` | Streaming read; callback `(const T*, ncols, tid)`; returns the row count |
| `size_t detect_cols()` | Auto-detects the column count (delimiters in the first data row + 1) |
| `size_t chunk_count()` | Parallel chunk count of this instance (always 1 when `parallel=false`) |

`ReadResult`: `data` (a row-major flat array — `std::vector<T>` or the aligned variant), `rows`, `cols`, and `at(r, c)` for 2-D access.

### Tuning the shared thread pool

```cpp
ThreadPool::set_shared_workers(8);          // call before the first parallel read
// afterwards: ThreadPool::shared() / shared_workers() / worker_count()
```

All instances share one process-wide singleton pool, created lazily on the first parallel read; the worker count defaults to hardware concurrency.

### Field-level parsing (optional public API)

```cpp
const char* p = text.data();
double v = parse_number<double>(p, p + text.size());                    // one field
size_t n = parse_number_row<double>(row, row_end, out, max_cols, ',');  // one row
```

### Parsing semantics

| Input | Result |
|---|---|
| `123` `-1.5` `+2` `.5` `1e10` `-2.5e-3` | the numeric value |
| `nan` `NaN` `inf` `Infinity` `-inf` `-infinity` | ±NaN / ±inf (case-insensitive) |
| empty field, `abc`, `-`, `.`, `e5`, ... (no significant digits) | `NaN` |
| `1abc` `1.2.3` `1 5` (partially parseable) | parses up to the first invalid char (strtod style): `1.0` / `1.2` / `1.0` |
| exponent overflow (e.g. `1e999`) | ±inf |
| exponent underflow (e.g. `1e-999`) | `0.0` (float is zeroed below ~1e-38) |
| integers with more than 18 digits | accumulated in floating point, magnitude preserved, relative error ≤ float/double precision |

> Quotes (`"..."`) are not special: they participate in parsing as ordinary characters
> (`"1.5"` → `1.5`, `"abc"` → NaN).

---

## Using the data as raw pointers

`read()` returns **row-major contiguous heap memory** — parsed values are copied out of
the mmap region into an ordinary vector, independent of the reader/file lifetime — so you
can hand raw pointers to any C / C++ API (FFTW, BLAS/LAPACK, SIMD, plotting libraries, ...)
with zero copying:

```cpp
CsvReader<double, 64> reader("data.csv", CsvOptions{false});
auto res = reader.read();                // keep the result alive as long as the pointers

double* p = res.data.data();             // first element of the contiguous buffer (64-byte aligned here)
size_t  n = res.data.size();             // total element count = rows * cols

process(p, n);                           // plain C interface
analyze(static_cast<const double*>(p), n);  // pass const double* to read-only APIs
```

Row-major indexing: element `(r, c)` lives at `p[r * cols + c]` (same as `res.at(r, c)`);
the row stride is `cols` doubles, the column stride is 1.

**Pointer lifetime rules:**

- `read()` data belongs to `res.data` (independent heap memory) — pointers stay valid
  after the `reader` is destroyed; as long as `res` is alive the pointers may be used
  safely from other threads (data is never modified once the read finishes)
- Row pointers passed to a `stream()` callback point into the reader's reused buffers
  and are **valid only inside the callback** — copy anything you need to keep
- If you `std::move(res.data)` to take ownership, keep the new vector alive

### FFTW integration (tested example)

`CsvReader<double, 64>::data()` satisfies FFTW's SIMD alignment requirements
(16/32/64 bytes), so it can be used directly as transform input/output — no
`fftw_malloc` needed:

```cpp
#include "csv_reader.hpp"
#include <fftw3.h>

// single column of N samples → real-to-complex FFT
CsvReader<double, 64> reader("signal.csv", CsvOptions{false});
auto res = reader.read();
size_t n = res.rows;

fftw_complex* out = fftw_alloc_complex(n / 2 + 1);
fftw_plan p = fftw_plan_dft_r2c_1d((int)n, res.data.data(), out, FFTW_ESTIMATE);
fftw_execute(p);
// ... use out[0..n/2]; remember fftw_destroy_plan(p); fftw_free(out);
```

Build & link (when FFTW is installed under the prefix `~/projects/fftw`):

```bash
g++ -std=c++17 -O2 -pthread \
    -I"$HOME/projects/fftw/include" -L"$HOME/projects/fftw/lib" -lfftw3 \
    -o demo demo.cpp
LD_LIBRARY_PATH="$HOME/projects/fftw/lib" ./demo
```

---

## Design

```
csv_reader.hpp (single header, in dependency order)
├── MappedFile          cross-platform memory mapping (mmap / CreateFileMapping)
├── AlignedAllocator    aligned allocator (used when Align > 0)
├── ThreadPool          process-wide shared thread pool (Meyers singleton, lazy)
│     └─ shared by all CsvReader instances: N instances = 1 pool
├── NumericTraits<T>    strategy: pow10 table / exponent limit / NaN / Inf
├── parse_number<T>     unified parse templates (full: scientific notation; fast: no exponent)
└── CsvReader<T, Align> unified reader: chunk → parallel parse → assemble
```

- **Thread model**: with `parallel=true` the file is split into `threads` line-aligned
  chunks submitted to the shared pool; with `parallel=false` parsing runs inline on the
  calling thread and no threads are created at all
- **Thread safety**: distinct instances may call `read()` / `stream()` concurrently
  (the shared pool is concurrency-safe); do not call the same instance concurrently;
  **do not start a parallel read inside a `stream()` callback** (it would deadlock
  the shared pool)
- **Memory reuse**: column count / row-count estimate / chunk ranges / chunk buffers are
  cached across calls

### Adding a new numeric type

Provide one `NumericTraits` specialization and then use `CsvReader<long double>` directly:

```cpp
template <> struct NumericTraits<long double> {
    using type = long double;
    static constexpr size_t pow10_max = 4932;   // platform-dependent exponent limit
    static constexpr std::array<long double, pow10_max + 1> pow10_table = {...};
    static long double quiet_nan() noexcept { return std::numeric_limits<long double>::quiet_NaN(); }
    static long double infinity() noexcept { return std::numeric_limits<long double>::infinity(); }
};
```
