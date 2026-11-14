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
#include "Report.h"
#include "util.h"
#include "Verifier.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
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
    , verifyInputs{}
    , reportFile{}
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

static string formatNames(const std::unordered_set<string>& names) {
    vector<string> sorted(names.begin(), names.end());
    std::sort(sorted.begin(), sorted.end());
    string result;
    for (const string& name : sorted) {
        if (!result.empty())
            result += ", ";
        result += name;
    }
    return result;
}

InlineOutcome CppInliner::inlineCode(const vector<string>& cppFilePaths, const string& outputFilePath) const {
    const string concatStage{pathConcat(temporaryDirectory, "concat.cpp")};
    const string inlinedStage{pathConcat(temporaryDirectory, "inlined.cpp")};

    concatFiles(cppFilePaths, concatStage);

    internal::Inliner inliner{clangCompilationOptions};
    std::string inlinedCode{inliner.doInline(concatStage)};
    const string unoptimizedCode{removeInvalidDirectives(inlinedCode, inlinedStage)};
    const auto prepareOutput = [&](const string& text) {
        return removeEmptyLines(removeComments ? internal::stripComments(text) : text,
                                maxConsequentEmptyLines);
    };
    string code;
    InlineOutcome outcome = removeUnusedCode ? InlineOutcome::Optimized : InlineOutcome::Unoptimized;

    std::unique_ptr<internal::Report> report;
    if (removeUnusedCode && !reportFile.empty())
        report.reset(new internal::Report);
    string reportOutcome;

    if (removeUnusedCode) {
        internal::Optimizer optimizer{inliner.getResultingCommandLineOptions(), macrosToKeep, identifiersToKeep};
        std::unordered_set<string> namesToRestore;
        internal::VerificationResult result;
        const unsigned kMaxRepairRounds = 3;
        for (unsigned round = 0; round <= kMaxRepairRounds; ++round) {
            std::unordered_set<string> removedNames;
            // Every attempt starts from the same stage-1 file. Restoration is
            // through graph roots and their dependencies, never text edits.
            code = prepareOutput(optimizer.doOptimize(inlinedStage, namesToRestore, removedNames, report.get()));
            if (report) {
                reportOutcome = round == 0 ? "optimized" :
                    "optimized after " + std::to_string(round) + " repair round(s); restored: " +
                    formatNames(namesToRestore);
            }
            if (!verifyOutput)
                break;

            result = internal::verifyCode(code, inliner.getResultingCommandLineOptions(),
                                          verifyCommand, temporaryDirectory);
            if (result.passed) {
                if (round > 0)
                    std::cerr << "caide inliner: note: optimized output needed " << round
                              << " repair round(s); restored: " << formatNames(namesToRestore) << '\n';
                break;
            }
            if (round == kMaxRepairRounds)
                break;

            const auto found = internal::findRepairNames(code, result, removedNames);
            bool added = false;
            for (const string& name : found)
                added = namesToRestore.insert(name).second || added;
            if (!added)
                break;
        }

        if (verifyOutput && !result.passed) {
            std::cerr << "caide inliner: warning: optimized output failed verification ("
                      << result.reason << "); writing unoptimized output\n";
            if (!namesToRestore.empty())
                std::cerr << "caide inliner: warning: repair tried restoring: "
                          << formatNames(namesToRestore) << '\n';
            istringstream diagnostics(result.output);
            string line;
            for (unsigned i = 0; i < 20 && std::getline(diagnostics, line); ++i)
                std::cerr << line << '\n';

            code = prepareOutput(unoptimizedCode);
            outcome = InlineOutcome::FellBack;
            if (report)
                reportOutcome = "fell back to unoptimized output: " + result.reason;
            if (result.judgeFailed &&
                !internal::verifyWithJudge(code, verifyCommand, temporaryDirectory).passed) {
                std::cerr << "caide inliner: warning: unoptimized output also fails with the "
                             "judge compiler (a stage-1 or environment problem)\n";
            }
        }

        if (verifyOutput && result.passed && !verifyCommand.empty() && !verifyInputs.empty()) {
            const string fallback = prepareOutput(unoptimizedCode);
            const auto behaviour = internal::verifyBehaviour(code, fallback, cppFilePaths,
                clangCompilationOptions, verifyCommand, verifyInputs, temporaryDirectory);
            std::cerr << behaviour.warnings;
            if (behaviour.useUnoptimized) {
                code = fallback;
                outcome = InlineOutcome::FellBack;
                if (report)
                    reportOutcome = "fell back to unoptimized output: " +
                        internal::runtimeFallbackReason(behaviour.warnings);
            }
        }
    } else {
        code = prepareOutput(unoptimizedCode);
    }

    ofstream out{outputFilePath, std::ios::binary};
    out << code;
    out.close();
    if (report) {
        if (!out)
            throw std::runtime_error("Cannot write output file: " + outputFilePath);
        report->write(reportFile, reportOutcome);
    }
    return outcome;
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
    const int result = caideInlineCppCodeVerified(options, nullptr, 0, cppFilePaths, numCppFiles, outputFilePath);
    return result == 3 ? 0 : result;
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
        const vector<string> macrosToKeep = arrayToCppVector(options->macrosToKeep, options->numMacrosToKeep);
        inliner.macrosToKeep.insert(inliner.macrosToKeep.end(),
            macrosToKeep.begin(), macrosToKeep.end());
        inliner.identifiersToKeep = arrayToCppVector(options->identifiersToKeep, options->numIdentifiersToKeep);
        inliner.maxConsequentEmptyLines = options->maxConsequentEmptyLines;
        inliner.verifyOutput = true;
        inliner.verifyCommand = arrayToCppVector(verifyCommand, numVerifyArgs);
        vector<string> files = arrayToCppVector(cppFilePaths, numCppFiles);
        const auto outcome = inliner.inlineCode(files, outputFilePath);
        return outcome == caide::InlineOutcome::FellBack ? 3 : 0;
    } catch (const std::exception& e) {
        // TODO: Expose error message in C interface.
        std::cerr << e.what() << std::endl;
        return 1;
    } catch (...) {
        return 2;
    }
}

