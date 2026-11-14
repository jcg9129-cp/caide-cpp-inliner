//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#include "Verifier.h"
#include "ErrorCollector.h"
#include "clang_version.h"
#include "util.h"

#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>

#if CAIDE_CLANG_VERSION_AT_LEAST(16, 0)
#include <optional>
#elif CAIDE_CLANG_VERSION_AT_LEAST(6, 0)
#include <llvm/ADT/Optional.h>
#endif

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <utility>

using std::string;
using std::vector;

namespace caide {
namespace internal {
namespace {

string normalizePath(const string& file) {
    if (file.empty())
        return {};
    llvm::SmallString<256> path(file);
    if (llvm::sys::fs::make_absolute(path))
        return file;
    llvm::SmallString<256> realPath;
    if (!llvm::sys::fs::real_path(path, realPath))
        return realPath.str().str();
    llvm::sys::path::remove_dots(path, true);
    return path.str().str();
}

struct VerificationFiles {
    string candidate;
    string executable;
    string log;

    explicit VerificationFiles(const string& directory)
        : candidate(directory + "/verify-candidate.cpp")
        , executable(directory + "/verify-candidate")
        , log(directory + "/verify-compiler.log")
    {
#ifdef _WIN32
        executable += ".exe";
#endif
    }

    ~VerificationFiles() {
        for (const string* path : {&candidate, &executable, &log})
            std::remove(path->c_str());
    }
};

bool writeCandidate(const string& code, const VerificationFiles& files,
                    VerificationResult& result) {
    std::ofstream out(files.candidate, std::ios::binary);
    out << code;
    out.close();
    if (out)
        return true;
    result.reason = "could not write verification candidate";
    result.output = files.candidate + ": could not write file\n";
    return false;
}

VerificationResult checkEmbedded(const VerificationFiles& files,
                                const vector<string>& options) {
    auto database = createCompilationDatabaseFromCommandLine(options);
    if (!database) {
        VerificationResult result;
        result.reason = "could not create verification compilation database";
        return result;
    }

    ErrorCollector errors;
    const vector<string> sources{files.candidate};
    clang::tooling::ClangTool tool(*database, sources);
    tool.setDiagnosticConsumer(&errors);
    auto factory = clang::tooling::newFrontendActionFactory<clang::SyntaxOnlyAction>();
    const int status = tool.run(factory.get());
    VerificationResult result;
    result.diagnostics = errors.getDiagnostics();
    result.passed = status == 0 && errors.getErrors().empty();
    if (!result.passed) {
        result.reason = "embedded Clang check failed";
        for (const string& error : errors.getErrors())
            result.output += error + '\n';
    }
    return result;
}

// Compiler output is retained verbatim. Only the file:line:column prefix is
// interpreted; repair must not depend on English diagnostic message text.
void collectJudgeLocations(VerificationResult& result) {
    const std::regex location(R"(^(.*?):([0-9]+):([0-9]+):[ \t]*(.*)$)");
    std::istringstream lines(result.output);
    for (string line; std::getline(lines, line); ) {
        std::smatch match;
        if (!std::regex_match(line, match, location))
            continue;
        VerificationDiagnostic diagnostic;
        diagnostic.file = match[1].str();
        std::istringstream lineNumber(match[2].str());
        std::istringstream columnNumber(match[3].str());
        if (!(lineNumber >> diagnostic.line) || !(columnNumber >> diagnostic.column))
            continue;
        diagnostic.message = match[4].str();
        result.diagnostics.push_back(std::move(diagnostic));
    }
}

VerificationResult checkJudge(const VerificationFiles& files, const vector<string>& command) {
    VerificationResult result;
    result.judgeFailed = true;
    const auto compiler = llvm::sys::findProgramByName(command[0]);
    if (!compiler) {
        result.reason = "judge compiler not found: " + command[0];
        result.output = compiler.getError().message() + '\n';
        return result;
    }

    vector<string> build = command;
    build[0] = *compiler;
    build.push_back(files.candidate);
    build.push_back("-o");
    build.push_back(files.executable);

    // Keep the strings alive through ExecuteAndWait, as in test-tool's judge.
#if CAIDE_CLANG_VERSION_AT_LEAST(7, 0)
    vector<llvm::StringRef> args(build.begin(), build.end());
#else
    vector<const char*> args;
    for (const string& arg : build)
        args.push_back(arg.c_str());
    args.push_back(nullptr);
#endif
    const llvm::StringRef paths[] = {"", files.log, files.log};
#if CAIDE_CLANG_VERSION_AT_LEAST(16, 0)
    std::optional<llvm::StringRef> redirects[] = {paths[0], paths[1], paths[2]};
#elif CAIDE_CLANG_VERSION_AT_LEAST(6, 0)
    llvm::Optional<llvm::StringRef> redirects[] = {paths[0], paths[1], paths[2]};
#else
    const llvm::StringRef* redirects[] = {&paths[0], &paths[1], &paths[2]};
#endif
    string error;
    const int status = llvm::sys::ExecuteAndWait(build[0],
#if CAIDE_CLANG_VERSION_AT_LEAST(7, 0)
        args,
#else
        args.data(),
#endif
        {}, redirects, 60, 0, &error);
    result.passed = status == 0;
    result.judgeFailed = !result.passed;
    if (!result.passed) {
        if (status == -1)
            result.reason = "judge compiler could not be executed";
        else if (status < 0)
            result.reason = "judge compiler crashed or timed out";
        else
            result.reason = "judge compiler exited with code " + std::to_string(status);
        if (!error.empty())
            result.output = error + '\n';
        std::ifstream log(files.log, std::ios::binary);
        std::ostringstream output;
        output << log.rdbuf();
        result.output += output.str();
        collectJudgeLocations(result);
    }
    return result;
}

} // namespace

VerificationResult verifyCode(const string& code, const vector<string>& compilationOptions,
                             const vector<string>& judgeCommand, const string& temporaryDirectory) {
    const VerificationFiles files(temporaryDirectory);
    VerificationResult result;
    if (!writeCandidate(code, files, result))
        return result;
    result = checkEmbedded(files, compilationOptions);
    if (result.passed && !judgeCommand.empty())
        result = checkJudge(files, judgeCommand);
    // Resolve symlinks while the candidate still exists. The returned paths
    // remain comparable after VerificationFiles removes the temporary files.
    result.candidatePath = normalizePath(files.candidate);
    for (auto& diagnostic : result.diagnostics)
        diagnostic.file = normalizePath(diagnostic.file);
    return result;
}

VerificationResult verifyWithJudge(const string& code, const vector<string>& judgeCommand,
                                  const string& temporaryDirectory) {
    VerificationResult result;
    if (judgeCommand.empty()) {
        result.passed = true;
        return result;
    }
    const VerificationFiles files(temporaryDirectory);
    result.judgeFailed = true;
    if (!writeCandidate(code, files, result))
        return result;
    return checkJudge(files, judgeCommand);
}

std::unordered_set<string> findRepairNames(const string& code,
        const VerificationResult& result, const std::unordered_set<string>& removedNames) {
    std::unordered_set<unsigned> lines;
    for (const auto& diagnostic : result.diagnostics) {
        if (!result.candidatePath.empty() && diagnostic.file == result.candidatePath && diagnostic.line)
            lines.insert(diagnostic.line);
    }

    std::unordered_set<string> found;
    if (lines.empty() || removedNames.empty())
        return found;

    const auto isStart = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    const auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    unsigned line = 1;
    std::size_t pos = 0;
    const auto skipTo = [&](std::size_t end) {
        if (end == string::npos)
            end = code.size();
        while (pos < end) {
            if (code[pos++] == '\n')
                ++line;
        }
    };

    // Scan in source order so multiline comments and raw strings are skipped
    // even when they start before a diagnostic line. Columns are never used.
    while (pos < code.size()) {
        if (code.compare(pos, 2, "//") == 0) {
            skipTo(code.find('\n', pos));
        } else if (code.compare(pos, 2, "/*") == 0) {
            const auto end = code.find("*/", pos + 2);
            skipTo(end == string::npos ? end : end + 2);
        } else if (code[pos] == '"' || code[pos] == '\'') {
            const char quote = code[pos];
            std::size_t end = pos + 1;
            while (end < code.size()) {
                const char c = code[end++];
                if (c == '\\' && end < code.size())
                    ++end;
                else if (c == quote)
                    break;
            }
            skipTo(end);
        } else if (isDigit(code[pos])) {
            // In a number, an apostrophe is a digit separator, not a quote.
            while (pos < code.size() && (isStart(code[pos]) || isDigit(code[pos]) ||
                                        code[pos] == '\'' || code[pos] == '.'))
                ++pos;
        } else if (isStart(code[pos])) {
            const auto begin = pos++;
            while (pos < code.size() && (isStart(code[pos]) || isDigit(code[pos])))
                ++pos;
            const string name = code.substr(begin, pos - begin);
            if (pos < code.size() && (code[pos] == '"' || code[pos] == '\'') &&
                (name == "u8" || name == "u" || name == "U" || name == "L"))
                continue; // The next iteration skips the prefixed literal.
            if (pos < code.size() && code[pos] == '"' &&
                (name == "R" || name == "u8R" || name == "uR" || name == "UR" || name == "LR")) {
                const auto open = code.find('(', pos + 1);
                if (open != string::npos && open - pos <= 17) {
                    const string close = ")" + code.substr(pos + 1, open - pos - 1) + '"';
                    const auto end = code.find(close, open + 1);
                    skipTo(end == string::npos ? end : end + close.size());
                    continue;
                }
            }
            if (lines.count(line) && removedNames.count(name))
                found.insert(name);
        } else {
            skipTo(pos + 1);
        }
    }
    return found;
}

} // namespace internal
} // namespace caide
