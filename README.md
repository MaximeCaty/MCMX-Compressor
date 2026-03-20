# MCMX

> **MCM — Unchained.** A high-performance fork of the MCM compressor engineered for real-world speed without sacrificing the compression ratio MCM is known for.

---

## Why MCMX?

MCM is a top tier compressor regarding speed/ratio. Its weakness has always been speed — making it impractical for many real-world workloads. 

MCMX fixes that through a few improvements: modern compiler, faster level, and multithreading.

## What's New

### ⚙️ Compiler Optimisations
- **Modern LTO compilation** — Link-Time Optimisation squeezes an additional **~5% speed** from the binary at no cost to compression.
- **PGO (Profile-Guided Optimisation)** — Post-compile training on representative workloads yields a further **~6% speed** improvement by optimising real hot paths.

### 🗜️ New Compression Level: `-z`
A new level faster than `-t`, with limited ratio loss when sufficient memory is available. Ideal when you want to stay in MCM territory but need more throughput.

### 🧠 Automatic Memory level Selection
When no memory level is specified, MCMX determines the optimal level automatically:
- Targets the **3× file size sweet spot** — the point where MCM delivers its best ratio/speed trade-off.
- Scales **down automatically** if free RAM is insufficient, so you never crash or thrash.

### 🧵 Multithreading — The Biggest Win

This is where MCMX makes its largest gains:

| Feature | Detail |
|---|---|
| **Block splitting** | Files are split into blocks and compressed / decompressed in parallel |
| **Shared dictionary** | Mixed Context model tolerates threading with minimal ratio loss using shared dictionary across threads. |

> **Note on threading vs. ratio:** BWT compressors lose significant ratio when threaded because context is lost between blocks. MCM's Mixed Context approach is more resilient — making MCMX's multithreading genuinely effective rather than a trade-off.

### Misc. Fixs

- Fixed the decompression output file path beeing ignored in v0.83 when running on single file
- Small memory managment improvments with no significant impact

---

## Performance at a Glance

| Compressor | Speed | Compression Ratio |
|---|---|---|
| MCM (original) | Slow | ★★★★★ |
| **MCMX (1 thread)** | **Faster** | **★★★★★** |
| **MCMX (2+ threads)** | **Much faster** | **★★★★☆** |
| Libbsc | Fastest | ★★★★☆ |

Test run on TAR containing 150 MB of binaries

| Compressor                | Size KB | RAM Peak | Compr. MS | Decomp. MS | VS Libbsc |
|---------------------------|---------|----------|-----------|------------|---------------------------|
| libbsc 3.3.12 -e2 -b200   | 13 782  | 505 MB   | 1473      | 1000       |                           |
| MCM 0.83 -m7              | 10 837  | 558 MB   | 13500     | 13092      | -21.3%                    |
| MCMX 0.85 -m7             | 10 837  | 559 MB   | 12000     | 13239      | -21.3%                    |
| MCMX 0.85 -m7  -threads 2 | 10 907  | 832 MB   | **6750**      | **6928**       | -20.8%                    |
| MCMX 0.85 -z7 -threads 2  | 11 895  | 690 MB   | **4570**      | **4854**       | -15.8%                    |



At the fastest levels (`-z` / `-t`) with 2 or more threads, MCMX approaches libbsc in throughput while maintaining a higher compression ratio in many scenario, especialy on structured data.

---

## Usage

```
mcmx [options] <input> <output>

Compression levels :
  -z    Fast mode — fastest (new)
  -t    Turbo mode
  -m    Medium (default if auto-selection not applicable)
  -h    High (! use 2x memory)
  -x    Maximum ratio (! use 2x memory)
  
Memory levels (optional) :
  Combine after compression level
  -<c>1-11  RAM allocation for the compressor, higher give better ratio. Recommended sweet spot is about 3-4x file size.
  Automatically determined when not provided to 3x file size, adjusted to available RAM.

Options:
  -threads <n>  Use n threads (e.g. -T4 for 4 threads)
          Omit to use a single thread
```
Exemple:

Compress : ```mcmx -z -threads 2 inputfile.bin outputfile.mcmx```

Compress with specific memory allocation : ```mcmx -z11 -threads 2 inputfile.bin outputfile.mcmx```

Decompress : ```mcmx d -threads 2 outputfile.mcmx outputfile.decompressed```

If no level is passed, MCMX selects one automatically based on file size and available RAM.

---

## Building

The repository is built with Visual Studio Code along with MinGW compiler using CMake. You need Microsoft C++ extension, Cmake extension and installed MinGW compiler.
Open the folder using VS Code, then run :
CMake : Delete Cache and Reconfigure
Then CTRL + F5 to build the executable.

 For PGO compilation :
 - Build the trainer executbale with cmake ```--build build --target mcm_train -j4```
 - Run compression with the trainer on multiple files eg ```build\mcmx_train.exe -z7 -threads 2 "C:\yourfile-sample" "C:\yourfile-sample.mcmx"```
 - Compile the PGO using trained data : ```cmake --build build --target mcm_pgo_train``` the gain is about 5-7% speed with reasonable training.


---

## Background

MCM uses a **Mixed Context Model** — a combination of context mixing and arithmetic coding — which gives it exceptional compression density. The tradeoff is CPU intensity. MCMX addresses this directly:

- **Compiler flags** reduce per-cycle overhead.
- **Multithreading** scales across cores without the dictionary fragmentation that hurts BWT-based parallel compressors (e.g. pbzip2, p7zip BWT mode).
- **Auto-level** makes it safe to use in scripts without knowing the file size or available memory in advance.

---

## Credits

MCMX is a fork of [MCM](https://github.com/mathieuchartier/mcm) by Mathieu Chartier. All credit for the core compression algorithm goes to the original author.

---

## License

Inherits the license of the upstream MCM project. See [`LICENSE`](LICENSE) for details.
