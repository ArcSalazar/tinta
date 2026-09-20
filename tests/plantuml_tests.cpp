// PlantUML core-module unit suite. Drives the fake CLI double built from
// tests/fake_plantuml.cpp - no real PlantUML and no Java are required.
//
// Paths are injected by CMake: TINTA_FAKE_PLANTUML is the built fake tool
// executable, TINTA_PLANTUML_SOURCE is src/plantuml.cpp (read by the
// no-network assertion). The suite is a plain check(bool, msg) harness in
// the style of tests/mermaid_tests.cpp and prints
// "All PlantUML tests passed" on success.
//
// Passing --expect-failure makes the harness fail on purpose; ctest runs
// that mode as a separate WILL_FAIL test so the checker itself is proven.

#include "plantuml.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <windows.h>

#ifndef TINTA_FAKE_PLANTUML
#error "TINTA_FAKE_PLANTUML must point at the fake_plantuml target file"
#endif
#ifndef TINTA_PLANTUML_SOURCE
#error "TINTA_PLANTUML_SOURCE must point at src/plantuml.cpp"
#endif

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    failures++;
}

std::wstring toWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0);
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), length);
    return out;
}

std::string toNarrow(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), out.data(), length,
                        nullptr, nullptr);
    return out;
}

// Saves an environment variable, applies a new value for the scope, then
// restores the previous state (including removing a variable that was unset).
class ScopedEnv {
public:
    ScopedEnv(const wchar_t* name, const wchar_t* value) : name_(name) {
        wchar_t previous[32767] = {};
        const DWORD length = GetEnvironmentVariableW(name, previous, 32767);
        had_ = length > 0 && length < 32767;
        if (had_) previous_.assign(previous, length);
        SetEnvironmentVariableW(name, value);
    }

    ~ScopedEnv() {
        SetEnvironmentVariableW(name_.c_str(), had_ ? previous_.c_str() : nullptr);
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::wstring name_;
    std::wstring previous_;
    bool had_ = false;
};

std::filesystem::path scratchRoot() {
    wchar_t base[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, base);
    return std::filesystem::path(base) /
           (L"tinta-plantuml-tests-" + std::to_wstring(GetCurrentProcessId()));
}

int scratchCounter = 0;

std::filesystem::path freshScratch(const wchar_t* label) {
    const std::filesystem::path dir =
        scratchRoot() / (std::wstring(label) + L"-" +
                         std::to_wstring(++scratchCounter));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return dir;
}

int countImages(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return 0;
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        std::error_code typeEc;
        if (!entry.is_regular_file(typeEc)) continue;
        std::wstring ext = entry.path().extension().wstring();
        for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
        if (ext == L".png" || ext == L".svg") ++count;
    }
    return count;
}

// Minimal bounded spawn helper used only to prove a failure scenario really
// produced an artifact before renderSync cleans it up.
bool runProcess(const std::wstring& commandLine, DWORD timeoutMs, DWORD& exitCode) {
    std::vector<wchar_t> cmd(commandLine.begin(), commandLine.end());
    cmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    exitCode = 1;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    } else {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return wait == WAIT_OBJECT_0;
}

const std::wstring kFakeToolPath = toWide(TINTA_FAKE_PLANTUML);

// ---------------------------------------------------------------- language gate

void testLanguageGate() {
    check(plantuml::isFenceLanguage("plantuml"), "plantuml is a fence language");
    check(plantuml::isFenceLanguage("puml"), "puml is a fence language");
    check(plantuml::isFenceLanguage("pu"), "pu is a fence language");
    check(!plantuml::isFenceLanguage("mermaid"), "mermaid is not a plantuml fence");
    check(!plantuml::isFenceLanguage(""), "empty language is not a plantuml fence");
    check(!plantuml::isFenceLanguage("PlantUML"),
          "the gate expects an already-lowercased language");
    check(!plantuml::isFenceLanguage("plantuml2"), "lookalike language is rejected");
}

// ---------------------------------------------------------- preamble injection

void testPreambleInjection() {
    const std::string pre = "skinparam shadowing false\n";

    std::string src = "@startuml\nAlice -> Bob: hi\n@enduml\n";
    check(plantuml::injectPreamble(src, pre), "anchor line is found");
    check(src.compare(0, 9, "@startuml") == 0, "source still starts with the anchor");
    const size_t anchorEnd = src.find('\n') + 1;
    check(src.compare(anchorEnd, pre.size(), pre) == 0,
          "preamble lands immediately after the anchor line");
    check(src.find("skinparam") > src.find("@startuml"),
          "preamble never precedes @startuml");
    check(src.find("Alice -> Bob") > src.find("skinparam shadowing false"),
          "diagram body follows the preamble");

    std::string crlf = "  @StartUml  \r\nBob -> Alice: yo\r\n@enduml\r\n";
    check(plantuml::injectPreamble(crlf, pre), "indented mixed-case anchor is found");
    check(crlf.find("@StartUml  \r\nskinparam shadowing false\n") != std::string::npos,
          "preamble follows a CRLF anchor line");

    std::string named = "@startuml Flow\nA -> B\n";
    check(plantuml::injectPreamble(named, pre), "named block anchor is found");
    check(named.compare(0, 14, "@startuml Flow") == 0 &&
          named.find("skinparam") == 15,
          "preamble follows a named block anchor");

    std::string atEof = "@startuml";
    check(plantuml::injectPreamble(atEof, pre), "anchor alone still injects");
    check(atEof == "@startuml\nskinparam shadowing false\n",
          "an anchor at EOF is terminated before the preamble");

    std::string none = "Alice -> Bob\n@enduml\n";
    const std::string before = none;
    check(!plantuml::injectPreamble(none, pre), "no anchor returns false");
    check(none == before, "a failed injection leaves the source untouched");

    std::string lookalike = "@startumlx\nA -> B\n";
    const std::string lookalikeBefore = lookalike;
    check(!plantuml::injectPreamble(lookalike, pre), "@startumlx is not an anchor");
    check(lookalike == lookalikeBefore, "a lookalike token leaves the source untouched");

    std::string multi =
        "@startuml\nA -> B\n@enduml\n@startuml\nC -> D\n@enduml\n";
    check(plantuml::injectPreamble(multi, pre), "multi-diagram anchor is found");
    const size_t first = multi.find("skinparam");
    check(first != std::string::npos &&
          multi.find("skinparam", first + 1) == std::string::npos,
          "only the first block receives the preamble");

    std::string emptyPre = "@startuml\nA -> B\n";
    const std::string emptyPreBefore = emptyPre;
    check(plantuml::injectPreamble(emptyPre, ""),
          "an empty preamble is a no-op that still finds the anchor");
    check(emptyPre == emptyPreBefore, "an empty preamble changes nothing");
}

// ------------------------------------------------------------ preamble content

void testPreambleContent() {
    const std::string p = plantuml::preamble("Segoe UI", 14.0f, "334455");
    check(p.find("skinparam backgroundColor transparent\n") != std::string::npos,
          "preamble declares a transparent background");
    check(p.find("skinparam shadowing false\n") != std::string::npos,
          "preamble disables shadowing");
    check(p.find("skinparam defaultFontName Segoe UI\n") != std::string::npos,
          "preamble passes the font family");
    check(p.find("skinparam defaultFontSize 14\n") != std::string::npos,
          "preamble prints an integral font size without decimals");
    check(p.find("skinparam defaultFontColor 334455\n") != std::string::npos,
          "preamble passes the text color");

    const size_t background = p.find("backgroundColor");
    const size_t shadowing = p.find("shadowing");
    const size_t fontName = p.find("defaultFontName");
    const size_t fontSize = p.find("defaultFontSize");
    const size_t fontColor = p.find("defaultFontColor");
    check(background != std::string::npos && shadowing != std::string::npos &&
          fontName != std::string::npos && fontSize != std::string::npos &&
          fontColor != std::string::npos && background < shadowing &&
          shadowing < fontName && fontName < fontSize && fontSize < fontColor,
          "preamble lines keep their documented order");

    const std::string fractional = plantuml::preamble("Cascadia Mono", 13.5f, "aabbcc");
    check(fractional.find("skinparam defaultFontSize 13.5\n") != std::string::npos,
          "a fractional font size keeps its fraction");
}

// -------------------------------------------------------------------- cache key

void testCacheKey() {
    const std::string source = "@startuml\nA -> B\n@enduml\n";
    const std::string pre = plantuml::preamble("Segoe UI", 14.0f, "112233");
    const std::wstring tool = L"C:/tools/plantuml.exe";
    const uint64_t baseline = plantuml::cacheKey(source, pre, tool, 4242, 0);

    check(baseline != 0, "cache key is non-zero");
    check(baseline == plantuml::cacheKey(source, pre, tool, 4242, 0),
          "cache key is stable for equal inputs");
    check(baseline != plantuml::cacheKey(source + "\n", pre, tool, 4242, 0),
          "changing the source changes the key");
    check(baseline != plantuml::cacheKey(source, pre + "extra", tool, 4242, 0),
          "changing the preamble changes the key");
    check(baseline != plantuml::cacheKey(source, pre, L"C:/tools/other.exe", 4242, 0),
          "changing the tool path changes the key");
    check(baseline != plantuml::cacheKey(source, pre, tool, 4243, 0),
          "changing the tool stamp changes the key");
    check(baseline != plantuml::cacheKey(source, pre, tool, 4242, 1),
          "changing the format changes the key");

    const uint64_t stamp = plantuml::toolStampFor(kFakeToolPath);
    check(stamp != 0, "tool stamp reads the fake tool file");
    check(stamp == plantuml::toolStampFor(kFakeToolPath), "tool stamp is stable");
    check(plantuml::toolStampFor(L"C:/definitely/missing/tool.exe") == 0,
          "missing tool files stamp as zero");
}

// -------------------------------------------------------------- command line

void testCommandLine() {
    plantuml::Tool exe;
    exe.available = true;
    exe.program = L"C:\\tools\\plantuml.exe";

    const std::wstring png =
        plantuml::buildCommandLine(exe, 0, L"C:\\out", L"C:\\in\\input.puml");
    check(png == L"\"C:\\tools\\plantuml.exe\" -tpng -charset UTF-8 "
                 L"-failfast2 -o \"C:\\out\" \"C:\\in\\input.puml\"",
          "exe PNG command line is exact");

    const std::wstring svg =
        plantuml::buildCommandLine(exe, 1, L"C:\\out", L"C:\\in\\input.puml");
    check(svg == L"\"C:\\tools\\plantuml.exe\" -tsvg -charset UTF-8 "
                 L"-failfast2 -o \"C:\\out\" \"C:\\in\\input.puml\"",
          "exe SVG command line is exact");

    plantuml::Tool jar;
    jar.available = true;
    jar.isJar = true;
    jar.program = L"C:\\Java\\bin\\java.exe";
    jar.jar = L"C:\\tools\\plantuml.jar";
    const std::wstring jarSvg =
        plantuml::buildCommandLine(jar, 1, L"C:\\out", L"C:\\in\\input.puml");
    check(jarSvg == L"\"C:\\Java\\bin\\java.exe\" -jar \"C:\\tools\\plantuml.jar\" "
                    L"-tsvg -charset UTF-8 -failfast2 -o \"C:\\out\" "
                    L"\"C:\\in\\input.puml\"",
          "jar SVG command line is exact");
    check(jarSvg.find(L"-jar") != std::wstring::npos &&
          jarSvg.find(L"java.exe") != std::wstring::npos,
          "jar command line spawns java with -jar");

    check(exe.describe() == L"C:\\tools\\plantuml.exe", "exe describe is the path");
    check(jar.describe() == L"C:\\Java\\bin\\java.exe -jar C:\\tools\\plantuml.jar",
          "jar describe names java and the jar");
    plantuml::Tool unavailable;
    check(unavailable.describe().empty(), "an unavailable tool describes empty");
}

// --------------------------------------------------------------- render success

void testRenderSuccess() {
    const plantuml::Tool tool = plantuml::resolveTool(kFakeToolPath);
    check(tool.available && !tool.isJar, "fake tool resolves as an available exe");

    const std::string source = "@startuml\nAlice -> Bob: hello\n@enduml\n";

    for (int format = 0; format <= 1; ++format) {
        const std::filesystem::path dir = freshScratch(format == 0 ? L"png" : L"svg");
        std::wstring out;
        std::wstring error;
        const bool ok = plantuml::renderSync(tool, source, format, dir.wstring(), out,
                                             15000, error);
        check(ok, format == 0 ? "PNG render succeeds" : "SVG render succeeds");
        if (!ok) {
            std::cerr << "  renderSync error: " << toNarrow(error) << '\n';
            continue;
        }
        check(error.empty(), "a successful render clears the error");
        check(!out.empty(), "a successful render sets outFile");
        check(std::filesystem::exists(out), "outFile exists on disk");
        check(std::filesystem::file_size(out) > 0, "outFile is non-empty");
        const std::wstring expectedName = format == 0 ? L"input.png" : L"input.svg";
        check(std::filesystem::path(out).filename().wstring() == expectedName,
              "outFile uses the format's canonical name");

        const std::filesystem::path staged = dir / L"input.puml";
        check(std::filesystem::exists(staged), "the staged source is written");
        std::ifstream stagedIn(staged, std::ios::binary);
        const std::string stagedText((std::istreambuf_iterator<char>(stagedIn)),
                                     std::istreambuf_iterator<char>());
        check(stagedText == source, "the staged source is byte-identical");
        check(stagedText.size() < 3 ||
              !(static_cast<unsigned char>(stagedText[0]) == 0xEF &&
                static_cast<unsigned char>(stagedText[1]) == 0xBB &&
                static_cast<unsigned char>(stagedText[2]) == 0xBF),
              "the staged source has no UTF-8 BOM");

        if (format == 1) {
            std::ifstream svg(out, std::ios::binary);
            const std::string svgText((std::istreambuf_iterator<char>(svg)),
                                      std::istreambuf_iterator<char>());
            check(svgText.find("TINTA-FAKE-PLANTUML") != std::string::npos,
                  "the SVG carries the fake sentinel");
        }
    }
}

// --------------------------------------------------------------- render failure

void testRenderFailureExitCode() {
    const plantuml::Tool tool = plantuml::resolveTool(kFakeToolPath);
    const std::string source = "@startuml\nA -> B\n@enduml\n";
    const ScopedEnv exitEnv(L"TINTA_FAKE_PLANTUML_EXIT", L"100");

    const std::filesystem::path dir = freshScratch(L"fail-exit");
    std::wstring out;
    std::wstring error;
    const bool ok = plantuml::renderSync(tool, source, 0, dir.wstring(), out, 15000,
                                         error);
    check(!ok, "exit 100 is reported as failure");
    check(!error.empty(), "exit 100 carries an error");
    check(out.empty(), "exit 100 leaves outFile empty");
    check(countImages(dir) == 0, "no image artifact survives a failed render");
    check(!std::filesystem::exists(dir / L"input.puml"),
          "the staged source is removed on failure");
}

void testRenderFailureWithImage() {
    const plantuml::Tool tool = plantuml::resolveTool(kFakeToolPath);
    const std::string source = "@startuml\nA -> B\n@enduml\n";
    const ScopedEnv imageEnv(L"TINTA_FAKE_PLANTUML_FAIL_WITH_IMAGE", L"1");

    // Control: prove the fake really writes its error image in this
    // environment, so the cleanup assertion below is not vacuous.
    const std::filesystem::path control = freshScratch(L"fail-image-control");
    std::filesystem::create_directories(control);
    {
        std::ofstream controlInput(control / L"input.puml", std::ios::binary);
        controlInput << "@startuml\n@enduml\n";
    }
    const std::wstring controlCmd =
        L"\"" + kFakeToolPath + L"\" -tpng -charset UTF-8 -failfast2 -o \"" +
        control.wstring() + L"\" \"" + (control / L"input.puml").wstring() + L"\"";
    DWORD controlExit = 0;
    const bool controlRan = runProcess(controlCmd, 15000, controlExit);
    check(controlRan, "control run of the fake tool completes");
    check(controlExit == 200, "control run exits 200 as the fail switch documents");
    check(std::filesystem::exists(control / L"fake-error.png"),
          "control proves the tool wrote an error image");

    const std::filesystem::path dir = freshScratch(L"fail-image");
    std::wstring out;
    std::wstring error;
    const bool ok = plantuml::renderSync(tool, source, 0, dir.wstring(), out, 15000,
                                         error);
    check(!ok, "an image plus a non-zero exit is still a failure");
    check(!error.empty(), "fail-with-image carries an error");
    check(out.empty(), "fail-with-image leaves outFile empty");
    check(!std::filesystem::exists(dir / L"input.png"),
          "no input.png survives a failed render");
    check(countImages(dir) == 0,
          "every image artifact is scrubbed from the failed workDir");
}

void testRenderTimeout() {
    const plantuml::Tool tool = plantuml::resolveTool(kFakeToolPath);
    const std::string source = "@startuml\nA -> B\n@enduml\n";
    const ScopedEnv sleepEnv(L"TINTA_FAKE_PLANTUML_SLEEP_MS", L"5000");

    const std::filesystem::path dir = freshScratch(L"timeout");
    std::wstring out;
    std::wstring error;
    const auto started = std::chrono::steady_clock::now();
    const bool ok =
        plantuml::renderSync(tool, source, 0, dir.wstring(), out, 500, error);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    check(!ok, "a timeout is reported as failure");
    check(!error.empty(), "a timeout carries an error");
    check(elapsed < 2000, "the 500 ms budget is enforced below the 5 s sleep");
    check(countImages(dir) == 0, "no image artifact survives a timeout");
}

// ----------------------------------------------------------------- tool resolve

void testResolveTool() {
    check(!plantuml::resolveTool(L"").available, "an empty path is unavailable");
    check(!plantuml::resolveTool(L"C:/definitely/missing/plantuml.exe").available,
          "a missing exe is unavailable");
    check(!plantuml::resolveTool(L"C:/definitely/missing/plantuml.jar").available,
          "a missing jar is unavailable");

    const plantuml::Tool exe = plantuml::resolveTool(kFakeToolPath);
    check(exe.available, "an existing exe path resolves");
    check(!exe.isJar, "an exe path is not classified as a jar");
    check(exe.program == kFakeToolPath, "the exe program is the configured path");

    // Jar classification: java.exe comes from SearchPathW, so prepend a temp
    // directory holding a copy of the fake renamed java.exe and restore PATH.
    const std::filesystem::path shimDir = freshScratch(L"java-shim");
    std::filesystem::create_directories(shimDir);
    std::filesystem::copy_file(kFakeToolPath, shimDir / L"java.exe",
                               std::filesystem::copy_options::overwrite_existing);
    const std::filesystem::path jarPath = shimDir / L"plantuml.jar";
    {
        std::ofstream jarFile(jarPath, std::ios::binary);
        jarFile << "PK fake jar";
    }

    wchar_t previousPath[32767] = {};
    const DWORD pathLength = GetEnvironmentVariableW(L"PATH", previousPath, 32767);
    const bool hadPath = pathLength > 0 && pathLength < 32767;
    const std::wstring savedPath =
        hadPath ? std::wstring(previousPath, pathLength) : std::wstring();
    const std::wstring shimmedPath = shimDir.wstring() + L";" + savedPath;
    SetEnvironmentVariableW(L"PATH", shimmedPath.c_str());
    const plantuml::Tool jar = plantuml::resolveTool(jarPath.wstring());
    SetEnvironmentVariableW(L"PATH", hadPath ? savedPath.c_str() : nullptr);

    check(jar.available, "a jar with java on PATH resolves");
    check(jar.isJar, "a .jar path is classified as a jar");
    check(!jar.program.empty(), "jar resolution provides a java program");
    check(jar.jar == jarPath.wstring(), "the jar path is preserved");
    check(jar.program == (shimDir / L"java.exe").wstring(),
          "SearchPathW picked the java.exe from PATH");
}

// --------------------------------------------------------------- no-network gate

void testNoNetworkApis() {
    std::ifstream source(TINTA_PLANTUML_SOURCE, std::ios::binary);
    check(static_cast<bool>(source), "the module source is readable");
    if (!source) return;
    const std::string text((std::istreambuf_iterator<char>(source)),
                           std::istreambuf_iterator<char>());
    const char* banned[] = {"winhttp", "wininet", "WinHttp", "URLDownload", "curl"};
    for (const char* needle : banned) {
        const std::string message =
            std::string("the module source must not reference ") + needle;
        check(text.find(needle) == std::string::npos, message.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--expect-failure") {
            check(false, "self-check: harness fails as designed");
            std::cout << "Self-check ran; designed failures: " << failures << '\n';
            return failures == 0 ? 0 : 1;
        }
    }

    testLanguageGate();
    testPreambleInjection();
    testPreambleContent();
    testCacheKey();
    testCommandLine();
    testRenderSuccess();
    testRenderFailureExitCode();
    testRenderFailureWithImage();
    testRenderTimeout();
    testResolveTool();
    testNoNetworkApis();

    if (failures == 0) {
        // QA hook: TINTA_PLANTUML_TEST_KEEP_SCRATCH=1 keeps the scratch tree
        // so the per-scenario workDirs can be inspected after the run.
        wchar_t keep[8] = {};
        const bool keepScratch =
            GetEnvironmentVariableW(L"TINTA_PLANTUML_TEST_KEEP_SCRATCH", keep, 8) > 0;
        std::cout << "All PlantUML tests passed\n";
        if (keepScratch) {
            std::cout << "scratch: " << toNarrow(scratchRoot().wstring()) << '\n';
        } else {
            std::error_code ec;
            std::filesystem::remove_all(scratchRoot(), ec);
        }
        return 0;
    }

    std::cerr << failures << " PlantUML check(s) failed; scratch kept at "
              << toNarrow(scratchRoot().wstring()) << '\n';
    return 1;
}