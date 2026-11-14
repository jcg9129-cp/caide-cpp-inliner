//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#include "../caideInliner.hpp"
#include "../clang_version.h"

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>

#if CAIDE_CLANG_VERSION_AT_LEAST(16, 0)
#include <optional>
#elif CAIDE_CLANG_VERSION_AT_LEAST(6, 0)
#include <llvm/ADT/Optional.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>


using std::ifstream;
using std::string;
using std::vector;

static vector<string> readNonEmptyLines(const string& filePath) {
    vector<string> lines;
    ifstream file{filePath.c_str()};
    string line;

    while (std::getline(file, line)) {
        if (line.find_first_not_of(" \t\r\n") != string::npos)
            lines.push_back(line);
    }

    return lines;
}

static string pathConcat(const string& directory, const string& fileName) {
    return directory + "/" + fileName;
}

// Optionally check semantics independently of the etalon: compile the original
// and inlined programs with CAIDE_TEST_CXX, then compare their outcomes/stdout.
// Only per-test portable flags reach the judge; auto-detected Clang flags must
// never reach it. Fragments/Clang-only originals may fail to build and are skipped.
struct JudgeTempFiles {
    const string& directory;

    ~JudgeTempFiles() {
        for (const char* name : {"judge-original", "judge-inlined", "judge-build.log",
                                 "judge-original.stdout", "judge-original.stderr",
                                 "judge-inlined.stdout", "judge-inlined.stderr"})
            std::remove(pathConcat(directory, name).c_str());
    }
};

struct JudgeOutcome {
    int exitCode;
    string error;
};

static JudgeOutcome executeJudge(const vector<string>& command, const string& input,
                                const string& output, const string& error, unsigned timeout) {
#if CAIDE_CLANG_VERSION_AT_LEAST(7, 0)
    vector<llvm::StringRef> args(command.begin(), command.end());
#else
    vector<const char*> args;
    for (const string& arg : command)
        args.push_back(arg.c_str());
    args.push_back(nullptr);
#endif

    const llvm::StringRef paths[] = {input, output, error};
#if CAIDE_CLANG_VERSION_AT_LEAST(16, 0)
    std::optional<llvm::StringRef> redirects[] = {paths[0], paths[1], paths[2]};
#elif CAIDE_CLANG_VERSION_AT_LEAST(6, 0)
    llvm::Optional<llvm::StringRef> redirects[] = {paths[0], paths[1], paths[2]};
#else
    const llvm::StringRef* redirects[] = {&paths[0], &paths[1], &paths[2]};
#endif

    JudgeOutcome result;
    result.exitCode = llvm::sys::ExecuteAndWait(command[0],
#if CAIDE_CLANG_VERSION_AT_LEAST(7, 0)
        args,
#else
        args.data(),
#endif
        {}, redirects, timeout, 0, &result.error);
    return result;
}

static vector<string> judgeOptions(const vector<string>& options, bool includePaths) {
    vector<string> result;
    for (std::size_t i = 0; i < options.size(); ++i) {
        const string& opt = options[i];
        // User include paths are needed only by the original: their headers get
        // inlined. System include paths (-isystem) are needed by both, because
        // the output keeps system #includes as they are.
        const bool include = opt.compare(0, 2, "-I") == 0 || opt.compare(0, 7, "-iquote") == 0;
        const bool portable = opt.compare(0, 2, "-D") == 0 ||
            opt.compare(0, 2, "-U") == 0 || opt.compare(0, 5, "-std=") == 0 ||
            opt.compare(0, 8, "-isystem") == 0;
        if (!include && !portable)
            continue;

        if (portable || includePaths)
            result.push_back(opt);
        if ((opt == "-I" || opt == "-isystem" || opt == "-iquote" ||
             opt == "-D" || opt == "-U") && i + 1 < options.size()) {
            ++i;
            if (portable || includePaths)
                result.push_back(options[i]);
        }
    }
    return result;
}

static void printJudgeLog(const string& path, unsigned maxLines, const JudgeOutcome& outcome) {
    if (!outcome.error.empty())
        std::cout << "[judge] " << outcome.error << '\n';
    ifstream log{path.c_str()};
    string line;
    for (unsigned i = 0; i < maxLines && std::getline(log, line); ++i)
        std::cout << line << '\n';
}

static bool compareJudgeStdout(const string& originalPath, const string& inlinedPath) {
    ifstream original{originalPath.c_str(), std::ios::binary};
    ifstream inlined{inlinedPath.c_str(), std::ios::binary};
    if (!original || !inlined) {
        std::cout << "[judge] could not read captured stdout\n";
        return false;
    }
    string originalLine, inlinedLine;
    for (std::size_t line = 1; ; ++line) {
        const bool hasOriginal = static_cast<bool>(std::getline(original, originalLine));
        const bool hasInlined = static_cast<bool>(std::getline(inlined, inlinedLine));
        if (!hasOriginal && !hasInlined)
            return true;
        if (hasOriginal != hasInlined || originalLine != inlinedLine ||
            original.eof() != inlined.eof()) {
            std::cout << "[judge] stdout differs at line " << line << '\n'
                      << "[judge] original: " << (hasOriginal ? originalLine : "<end of stdout>")
                      << (hasOriginal && original.eof() ? " [no final newline]" : "") << '\n'
                      << "[judge] inlined:  " << (hasInlined ? inlinedLine : "<end of stdout>")
                      << (hasInlined && inlined.eof() ? " [no final newline]" : "") << '\n';
            return false;
        }
    }
}

static bool checkJudge(const string& testDirectory, const string& tempDirectory,
                       const vector<string>& cppFiles, const vector<string>& options) {
    const char* cxx = std::getenv("CAIDE_TEST_CXX");
    if (!cxx || !*cxx)
        return true;
    std::istringstream words{cxx};
    vector<string> command;
    for (string word; words >> word; )
        command.push_back(word);
    if (command.empty())
        return true;

    const auto compiler = llvm::sys::findProgramByName(command[0]);
    if (!compiler) {
        // A misconfigured judge must not pass silently.
        std::cout << "[judge] failed: judge compiler not found: " << command[0] << ": "
                  << compiler.getError().message() << '\n';
        return false;
    }
    command[0] = *compiler;
    const JudgeTempFiles cleanup{tempDirectory};
    const string originalExe = pathConcat(tempDirectory, "judge-original");
    const string inlinedExe = pathConcat(tempDirectory, "judge-inlined");
    const string buildLog = pathConcat(tempDirectory, "judge-build.log");

    for (bool original : {true, false}) {
        vector<string> build = command;
        const vector<string> flags = judgeOptions(options, original);
        build.insert(build.end(), flags.begin(), flags.end());
        if (original)
            build.insert(build.end(), cppFiles.begin(), cppFiles.end());
        else
            build.push_back(pathConcat(tempDirectory, "result.cpp"));
        build.push_back("-o");
        build.push_back(original ? originalExe : inlinedExe);
        const JudgeOutcome outcome = executeJudge(build, "", buildLog, buildLog, 60);
        if (outcome.exitCode != 0) {
            std::cout << (original
                ? "[judge] skipped: original does not build with the judge compiler\n"
                : "[judge] failed: inlined output does not build with the judge compiler\n");
            printJudgeLog(buildLog, original ? 10 : 20, outcome);
            return original;
        }
    }

    string input = pathConcat(testDirectory, "input.txt");
    if (!ifstream{input.c_str()})
        input.clear(); // An empty redirect disconnects stdin portably.
    const string originalStdout = pathConcat(tempDirectory, "judge-original.stdout");
    const string inlinedStdout = pathConcat(tempDirectory, "judge-inlined.stdout");
    const JudgeOutcome original = executeJudge({originalExe}, input, originalStdout,
        pathConcat(tempDirectory, "judge-original.stderr"), 10);
    const JudgeOutcome inlined = executeJudge({inlinedExe}, input, inlinedStdout,
        pathConcat(tempDirectory, "judge-inlined.stderr"), 10);
    const bool sameStdout = compareJudgeStdout(originalStdout, inlinedStdout);
    // LLVM uses -2 for both crashes and timeouts; its error text distinguishes them.
    const bool sameOutcome = original.exitCode == inlined.exitCode &&
        (original.exitCode >= 0 || original.error == inlined.error);
    if (!sameOutcome || !sameStdout || original.exitCode == -1 || inlined.exitCode == -1) {
        std::cout << "[judge] original exit code: " << original.exitCode
                  << (original.error.empty() ? "" : " (" + original.error + ")") << '\n'
                  << "[judge] inlined exit code: " << inlined.exitCode
                  << (inlined.error.empty() ? "" : " (" + inlined.error + ")") << '\n';
        return false;
    }
    return true;
}

static bool runTest(const string& testDirectory, const string& tempDirectory, caide::CppInliner inliner) {
    // Setup
    vector<string> cppFiles = readNonEmptyLines(pathConcat(testDirectory, "fileList.txt"));
    for (string& s : cppFiles)
        s = pathConcat(testDirectory, s);

    for (int i = 1; i <= 9; ++i) {
        std::ostringstream fileName;
        fileName << i << ".cpp";
        const string filePath = pathConcat(testDirectory, fileName.str());
        ifstream file{filePath.c_str()};
        if (file)
            cppFiles.push_back(filePath);
    }

    if (cppFiles.empty()) {
        throw std::runtime_error("No source files found in test directory");
    }

    // A test may opt into exercising runtime auto-detection of compilation
    // options (the same code path as `cmd -a`) by placing an autoDetect.txt
    // marker file in its directory. This replaces the pre-detected options with
    // freshly detected ones; the per-test clangOptions.txt below is still
    // appended on top, just as `cmd -a` appends user-supplied options.
    {
        ifstream autoDetectMarker{pathConcat(testDirectory, "autoDetect.txt").c_str()};
        if (autoDetectMarker)
            inliner.autoDetectCompilationOptions();
    }

    // A test may opt into the first-stage-only mode (no removal of unreachable
    // code, the same as `cmd -n`) with a noOptimize.txt marker, and/or comment
    // stripping (`cmd -c`) with a stripComments.txt marker.
    {
        ifstream noOptimizeMarker{pathConcat(testDirectory, "noOptimize.txt").c_str()};
        if (noOptimizeMarker)
            inliner.removeUnusedCode = false;
        ifstream stripCommentsMarker{pathConcat(testDirectory, "stripComments.txt").c_str()};
        if (stripCommentsMarker)
            inliner.removeComments = true;
    }

    vector<string> additionalOptions = readNonEmptyLines(pathConcat(testDirectory, "clangOptions.txt"));
    for (string& opt : additionalOptions) {
        const static string TEST_ROOT_MARKER = "TEST_ROOT";
        auto p = opt.find(TEST_ROOT_MARKER);
        if (p != string::npos) {
            opt.replace(p, TEST_ROOT_MARKER.length(), testDirectory);
        }

        inliner.clangCompilationOptions.push_back(opt);
    }

    const char* verbose = std::getenv("CAIDE_TEST_VERBOSE");
    if (verbose && *verbose == '1')
        inliner.clangCompilationOptions.push_back("-v");

    inliner.macrosToKeep = readNonEmptyLines(pathConcat(testDirectory, "macrosToKeep.txt"));
    inliner.identifiersToKeep = readNonEmptyLines(pathConcat(testDirectory, "identifiersToKeep.txt"));

    const string outputFilePath = pathConcat(tempDirectory, "result.cpp");

    // A test may assert that inlining FAILS GRACEFULLY on invalid input — i.e.
    // reports a compilation error (throws) rather than crashing — by including
    // an expectError.txt file. Any non-empty lines in it are substrings the
    // error message must contain. A crash (e.g. SIGSEGV) kills this process, so
    // ctest reports the test as failed; a clean throw passes.
    {
        ifstream expectErrorMarker{pathConcat(testDirectory, "expectError.txt").c_str()};
        if (expectErrorMarker) {
            const vector<string> requiredSubstrings =
                readNonEmptyLines(pathConcat(testDirectory, "expectError.txt"));
            try {
                inliner.inlineCode(cppFiles, outputFilePath);
            } catch (const std::exception& e) {
                const string what = e.what();
                for (const string& sub : requiredSubstrings) {
                    if (what.find(sub) == string::npos) {
                        std::cout << "Error message missing expected substring \""
                                  << sub << "\":\n" << what << "\n";
                        return false;
                    }
                }
                return true; // failed as expected
            }
            std::cout << "Expected inlining to fail with a compilation error, but it succeeded\n";
            return false;
        }
    }

    // Run
    inliner.inlineCode(cppFiles, outputFilePath);

    // Assert
    const string etalonFilePath = pathConcat(testDirectory, "etalon.cpp");
    const vector<string> output = readNonEmptyLines(outputFilePath);
    const vector<string> etalon = readNonEmptyLines(etalonFilePath);

    const int minLength = (int)std::min(output.size(), etalon.size());
    bool etalonMatches = true;
    for (int i = 0; i < minLength; ++i) {
        // TODO: Print line numbers
        if (output[i] != etalon[i]) {
            std::cout
                << "< " << etalon[i] << "\n"
                << "> " << output[i] << "\n";
            etalonMatches = false;
            break;
        }
    }

    if (etalonMatches && output.size() < etalon.size()) {
        std::cout << "Unexpected end of file: " << outputFilePath << "\n";
        etalonMatches = false;
    }

    if (etalonMatches && output.size() > etalon.size()) {
        std::cout << "Unexpected end of file: " << etalonFilePath << "\n";
        etalonMatches = false;
    }

    if (!etalonMatches) {
        const string actualDirectory = pathConcat(tempDirectory, "actual");
        const string directory = testDirectory.substr(0, testDirectory.find_last_not_of("/\\") + 1);
        const string testName = llvm::sys::path::filename(directory).str();
        std::error_code error = llvm::sys::fs::create_directories(actualDirectory);
        if (!error)
            error = llvm::sys::fs::copy_file(outputFilePath, pathConcat(actualDirectory, testName + ".cpp"));
        if (error)
            std::cout << "Could not save actual output: " << error.message() << '\n';
    }

    const bool judgeMatches = checkJudge(testDirectory, tempDirectory, cppFiles, additionalOptions);
    return etalonMatches && judgeMatches;
}


int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Usage: test-tool <temp-directory> <compilation-options-file> [<test-directory>...]\n";
        return 1;
    }

    const string tempDirectory{argv[1]};

    caide::CppInliner inliner{tempDirectory};

    if (string(argv[2]) == "--prepare") {
        inliner.autoDetectCompilationOptions();
        std::ofstream os(argv[3]);
        for (const auto& option : inliner.clangCompilationOptions)
            os << option << '\n';
        return 0;
    }

    inliner.clangCompilationOptions = readNonEmptyLines(argv[2]);

    int numFailedTests = 0;
    for (int i = 3; i < argc; ++i) {
        try {
            if (!runTest(argv[i], tempDirectory, inliner)) {
                ++numFailedTests;
            }
        } catch (const std::exception& e) {
            ++numFailedTests;
            std::cout << e.what() << "\n";
        }
    }

    return numFailedTests;
}

