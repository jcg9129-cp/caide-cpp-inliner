//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#include "caideInliner.hpp"
#include "caideInliner.h"

#include "detect_options.h"
#include "inliner.h"
#include "optimizer.h"
#include "util.h"
#include "Verifier.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>


using std::istringstream;
using std::ofstream;
using std::string;
using std::vector;


namespace caide {

static string trimEndPathSeparators(const string& path) {
    string result{path};
    auto lastSymbol = result.find_last_not_of("/\\");
    if (lastSymbol != string::npos)
        result.erase(lastSymbol + 1);
    return result;
}

static bool startsWith(const string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

CppInliner::CppInliner(const string& temporaryDirectory_)
    : clangCompilationOptions{}
    , macrosToKeep{"__cplusplus", "__STDC_VERSION__",
        "__ANDROID__", "__MINGW32__", "__MINGW64__",
        "_WIN32", "_WIN64", "_M_AMD64", "__linux", "__linux__", "__APPLE__",
        "__GNUC__", "__GLIBC__", "__clang__", "_MSC_VER"}
    , maxConsequentEmptyLines{2}
    , removeUnusedCode{true}
    , removeComments{false}
    , verifyOutput{true}
    , verifyCommand{}
    , temporaryDirectory{trimEndPathSeparators(temporaryDirectory_)}
{
}

static void concatFiles(const vector<string>& cppFilePaths, const string& outputFilePath) {
    ofstream out{outputFilePath};
    for (const string& filePath : cppFilePaths) {
        std::ifstream in{filePath};
        if (!in)
            throw std::runtime_error(string("File not found: " + filePath));
        out << in.rdbuf();
        out << '\n'; // in case there was no return at end of file
    }
}

// Certain directives become invalid after the first stage (inliner) runs. Those include:
// * #pragma once (used to be in a header, now in the inlined source file).
// * #line number [file name] (became incorrect due to inlining header files).
//
// Ideally, the inliner would not emit these directives. However, it may be hard to do
// with currently available clang API. Instead, we use a simplistic postprocessing that
// should work in most cases.
static bool isInvalidDirective(string line) {
    auto it = std::remove_if(line.begin(), line.end(),
                [](char c) { return c == ' ' || c == '\t' || c == '\r'; });
    line.erase(it, line.end());
    // This is technically incorrect due to multiline directives and strings.
    return line == "#pragmaonce" || startsWith(line, "#line");
}

static string removeInvalidDirectives(const string& textInBinaryMode, const string& outputFilePath) {
    istringstream in{textInBinaryMode};
    ofstream out{outputFilePath, std::ios::binary};
    string line;
    string result;
    while (std::getline(in, line)) {
        if (!isInvalidDirective(line)) {
            out << line << '\n';
            result += line;
            result += '\n';
        }
    }
    return result;
}

static bool isWhitespaceOnly(const string& text) {
    return text.find_first_not_of(" \t\r") == string::npos;
}

static string removeEmptyLines(const string& textInBinaryMode,
                               int maxConsequentEmptyLines)
{
    if (maxConsequentEmptyLines < 0)
        maxConsequentEmptyLines = std::numeric_limits<int>::max();
    istringstream in{textInBinaryMode};
    std::ostringstream out;
    int currentConsequentEmptyLines = 0;
    bool readNonEmptyLine = false;
    string line;
    while (std::getline(in, line)) {
        if (isWhitespaceOnly(line))
            ++currentConsequentEmptyLines;
        else {
            currentConsequentEmptyLines = 0;
            readNonEmptyLine = true;
        }

        if (readNonEmptyLine && currentConsequentEmptyLines <= maxConsequentEmptyLines)
            out << line << '\n';
    }
    return out.str();
}

static string pathConcat(const string& path, const string& fileName) {
    string result{path};
    result.push_back('/');
    result += fileName;
    return result;
}

void CppInliner::inlineCode(const vector<string>& cppFilePaths, const string& outputFilePath) const {
    const string concatStage{pathConcat(temporaryDirectory, "concat.cpp")};
    const string inlinedStage{pathConcat(temporaryDirectory, "inlined.cpp")};

    concatFiles(cppFilePaths, concatStage);

    internal::Inliner inliner{clangCompilationOptions};
    std::string inlinedCode{inliner.doInline(concatStage)};
    const string unoptimizedCode{removeInvalidDirectives(inlinedCode, inlinedStage)};
    string code{unoptimizedCode};

    if (removeUnusedCode) {
        internal::Optimizer optimizer{inliner.getResultingCommandLineOptions(), macrosToKeep, identifiersToKeep};
        code = optimizer.doOptimize(inlinedStage);
    }

    if (removeComments)
        code = internal::stripComments(code);

    // Verify the exact text to be written, including comment and blank-line
    // processing, so diagnostic locations remain useful to a future repair loop.
    code = removeEmptyLines(code, maxConsequentEmptyLines);
    if (removeUnusedCode && verifyOutput) {
        const internal::VerificationResult result = internal::verifyCode(code,
            inliner.getResultingCommandLineOptions(), verifyCommand, temporaryDirectory);
        if (!result.passed) {
            std::cerr << "caide inliner: warning: optimized output failed verification ("
                      << result.reason << "); writing unoptimized output\n";
            istringstream diagnostics(result.output);
            string line;
            for (unsigned i = 0; i < 20 && std::getline(diagnostics, line); ++i)
                std::cerr << line << '\n';

            code = removeComments ? internal::stripComments(unoptimizedCode) : unoptimizedCode;
            code = removeEmptyLines(code, maxConsequentEmptyLines);
            if (result.judgeFailed &&
                !internal::verifyWithJudge(code, verifyCommand, temporaryDirectory).passed) {
                std::cerr << "caide inliner: warning: unoptimized output also fails with the "
                             "judge compiler (a stage-1 or environment problem)\n";
            }
        }
    }

    ofstream out{outputFilePath, std::ios::binary};
    out << code;
}

void CppInliner::autoDetectCompilationOptions() {
    clangCompilationOptions = internal::detectClangOptions(temporaryDirectory);
}

} // namespace caide

static vector<string> arrayToCppVector(const char** array, int size) {
    vector<string> res(size);
    for (int i = 0; i < size; ++i)
        res[i].assign(array[i]);
    return res;
}

extern "C" int caideInlineCppCode(
        const CaideCppInlinerOptions* options,
        const char** cppFilePaths,
        int numCppFiles,
        const char* outputFilePath)
{
    return caideInlineCppCodeVerified(options, nullptr, 0, cppFilePaths, numCppFiles, outputFilePath);
}

extern "C" int caideInlineCppCodeVerified(
        const CaideCppInlinerOptions* options,
        const char** verifyCommand,
        int numVerifyArgs,
        const char** cppFilePaths,
        int numCppFiles,
        const char* outputFilePath)
{
    try {
        caide::CppInliner inliner(options->temporaryDirectory);
        inliner.clangCompilationOptions = arrayToCppVector(
            options->clangCompilationOptions, options->numClangOptions);
        inliner.macrosToKeep = arrayToCppVector(options->macrosToKeep, options->numMacrosToKeep);
        inliner.identifiersToKeep = arrayToCppVector(options->identifiersToKeep, options->numIdentifiersToKeep);
        inliner.maxConsequentEmptyLines = options->maxConsequentEmptyLines;
        inliner.verifyOutput = true;
        inliner.verifyCommand = arrayToCppVector(verifyCommand, numVerifyArgs);
        vector<string> files = arrayToCppVector(cppFilePaths, numCppFiles);
        inliner.inlineCode(files, outputFilePath);
        return 0;
    } catch (const std::exception& e) {
        // TODO: Expose error message in C interface.
        std::cerr << e.what() << std::endl;
        return 1;
    } catch (...) {
        return 2;
    }
}

