#ifndef TINTA_PLANTUML_H
#define TINTA_PLANTUML_H

// PlantUML bridge: hands a diagram source to a locally installed PlantUML
// tool (a native plantuml.exe, or java.exe plus plantuml.jar) and brings
// back a rendered image.
//
// The module is app-free and headless: standard C++ plus the Win32 process
// and file APIs, no Direct2D/DirectWrite, no UI, no theme or App types, and
// no remote or network access of any kind. That keeps it unit-testable
// against the fake CLI double in tests/fake_plantuml.cpp and keeps the
// shipped binary a dependency-free, offline product.
//
// Real-CLI facts this interface is built on (probed against PlantUML 1.2026.8):
//   - skinparam lines placed BEFORE `@startuml` are silently ignored, so the
//     theme preamble is injected immediately AFTER the anchor line
//     (injectPreamble);
//   - `-failfast2` suppresses the error-image artifact on syntax errors;
//   - exit 100 means "no diagram in this source", 200 a syntax error.

#include <cstdint>
#include <string>

#include <windows.h>

namespace plantuml {

// Fence languages that route to the PlantUML renderer. The argument must
// already be lowercased (callers normalize the markdown info string before
// asking). True for "plantuml", "puml" and "pu".
bool isFenceLanguage(const std::string& lowercasedLanguage);

// A resolved PlantUML tool. `program` is the executable to spawn: the tool
// itself for a native exe, java.exe for a jar. `jar` is set only for jars.
struct Tool {
    bool available = false;
    bool isJar = false;
    std::wstring program;
    std::wstring jar;

    // Human-readable form for the settings row: the program path, or
    // "<java> -jar <jar>"; empty when unavailable.
    std::wstring describe() const;
};

// Resolves a user-configured path into a runnable tool:
//   - an empty path yields an unavailable tool;
//   - a case-insensitive ".jar" suffix is treated as a jar: the file must
//     exist and java.exe must be found via SearchPathW (the java runtime is
//     resolved from PATH; there is deliberately no second java setting);
//   - anything else is treated as an executable and must exist.
// Missing files and a missing java.exe yield an unavailable tool, never a
// partial one.
Tool resolveTool(const std::wstring& userPath);

// Inserts `preambleLines` immediately after the first `@startuml` line
// (matched after trimming surrounding whitespace, case-insensitively; a block
// name after the token, as in `@startuml Flow`, is accepted too).
//
// Returns false when the source has no `@startuml` anchor: the caller treats
// that source as unusable and falls back to showing it as code. There is
// deliberately NO auto-wrapping of plain sources.
//
// Multi-diagram sources: only the first `@startuml` block receives the
// preamble, and later tasks render that first block only.
//
// The inserted block is newline-terminated so the following source line stays
// intact; an anchor at end-of-file is terminated first. An empty preamble is a
// no-op that still reports whether an anchor exists. On success `source` is
// modified; on failure it is left untouched.
bool injectPreamble(std::string& source, const std::string& preambleLines);

// The theme preamble injected into a diagram source, one skinparam per line
// (the returned block is newline-terminated):
//   skinparam backgroundColor transparent
//   skinparam shadowing false
//   skinparam defaultFontName <fontFamily>
//   skinparam defaultFontSize <fontBaseSize>
//   skinparam defaultFontColor <textColorHex>
// Integral sizes print without a decimal point (14, not 14.0).
std::string preamble(const std::string& fontFamily, float fontBaseSize,
                     const std::string& textColorHex);

// Stable 64-bit FNV-1a over every rendering input: source, preamble, tool
// path, tool stamp and output format (0=png, 1=svg). Equal inputs hash equal
// across runs; changing any field changes the key. Each field is
// length-prefixed, so no two different input tuples collide by concatenation.
uint64_t cacheKey(const std::string& source, const std::string& preambleLines,
                  const std::wstring& toolPath, uint64_t toolStamp,
                  int format);

// File stamp (last-write time plus size) for cache invalidation when the tool
// is upgraded in place. Returns 0 when the file cannot be queried.
uint64_t toolStampFor(const std::wstring& path);

// Exact command line for the tool: the quoted program first, then an optional
// `-jar "<jar>"`, then `-tpng|-tsvg -charset UTF-8 -failfast2 -o "<outDir>"
// "<inputFile>"`. Format 0 selects PNG, 1 SVG (anything else renders PNG).
std::wstring buildCommandLine(const Tool& tool, int format,
                              const std::wstring& outDir,
                              const std::wstring& inputFile);

// Runs the tool synchronously in `workDir` and returns the rendered image.
// Writes `workDir\input.puml` (UTF-8, no BOM), spawns the tool with
// CREATE_NO_WINDOW and waits at most `timeoutMs`, terminating the process on
// timeout (an INFINITE budget is rejected: every spawn stays bounded).
//
// The exit code is validated FIRST: on any failure (non-zero exit, timeout,
// missing or empty output) the private workDir is scrubbed of every image
// artifact the tool may have written plus the staged source, `error` is set
// and `outFile` stays empty. Only a zero exit with a non-empty
// `workDir\input.png|svg` returns true and sets `outFile`.
//
// This is the deliberate synchronous entry point used by print and export;
// the interactive layout path must schedule it off the UI thread instead.
bool renderSync(const Tool& tool, const std::string& sourceWithPreamble,
                int format, const std::wstring& workDir, std::wstring& outFile,
                DWORD timeoutMs, std::wstring& error);

}  // namespace plantuml

#endif  // TINTA_PLANTUML_H