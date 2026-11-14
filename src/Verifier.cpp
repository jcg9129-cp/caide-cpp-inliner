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
    string output;
    string error;

    explicit VerificationFiles(const string& directory, const string& name = "verify-candidate")
        : candidate(directory + "/" + name + ".cpp")
        , executable(directory + "/" + name)
        , log(directory + "/" + (name == "verify-candidate" ? "verify-compiler" : name + "-compiler") + ".log")
        , output(directory + "/" + name + ".stdout")
        , error(directory + "/" + name + ".stderr")
    {
#ifdef _WIN32
        executable += ".exe";
#endif
    }

    ~VerificationFiles() {
        for (const string* path : {&candidate, &executable, &log, &output, &error})
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

struct ExecutionResult {
    int exitCode;
    string error;
};

ExecutionResult execute(const vector<string>& command, const string& input,
                        const string& output, const string& error, unsigned timeout) {
    // Keep the strings alive through ExecuteAndWait, as in test-tool's judge.
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
    ExecutionResult result;
    result.exitCode = llvm::sys::ExecuteAndWait(command[0],
#if CAIDE_CLANG_VERSION_AT_LEAST(7, 0)
        args,
#else
        args.data(),
#endif
        {}, redirects, timeout, 0, &result.error);
    return result;
}

VerificationResult checkJudge(const VerificationFiles& files, const vector<string>& command,
                              const vector<string>& sources = {}) {
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
    if (sources.empty())
        build.push_back(files.candidate);
    else
        build.insert(build.end(), sources.begin(), sources.end());
    build.push_back("-o");
    build.push_back(files.executable);

    const auto outcome = execute(build, "", files.log, files.log, 60);
    const int status = outcome.exitCode;
    const string& error = outcome.error;
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

vector<string> originalJudgeCommand(const vector<string>& command, const vector<string>& options) {
    vector<string> result = command;
    for (std::size_t i = 0; i < options.size(); ++i) {
        const string& option = options[i];
        // Only user include paths and macro options are portable to the judge.
        // In particular, auto-detected system paths belong to embedded Clang.
        if (option.compare(0, 2, "-I") != 0 && option.compare(0, 7, "-iquote") != 0 &&
            option.compare(0, 2, "-D") != 0 && option.compare(0, 2, "-U") != 0)
            continue;
        result.push_back(option);
        if ((option == "-I" || option == "-iquote" || option == "-D" || option == "-U") &&
            i + 1 < options.size())
            result.push_back(options[++i]);
    }
    return result;
}

string firstLines(const string& text, unsigned count) {
    std::istringstream lines(text);
    string result, line;
    for (unsigned i = 0; i < count && std::getline(lines, line); ++i)
        result += line + '\n';
    return result;
}

string behaviourDifference(const ExecutionResult& original, const ExecutionResult& candidate,
                           const string& originalOutput, const string& candidateOutput) {
    // LLVM uses -2 for both crashes and timeouts. Match the judge test helper,
    // including its refusal to accept two failures to launch as equivalent.
    if (original.exitCode != candidate.exitCode || original.exitCode == -1 ||
        (original.exitCode < 0 && original.error != candidate.error))
        return "exit code " + std::to_string(original.exitCode) + " vs " +
            std::to_string(candidate.exitCode);

    std::ifstream a(originalOutput, std::ios::binary);
    std::ifstream b(candidateOutput, std::ios::binary);
    // Compare bytes, including final newlines and embedded NULs. Count lines
    // only for the diagnostic; do not normalize whitespace or line endings.
    std::size_t line = 1;
    if (a && b) {
        for (;;) {
            const auto x = a.get();
            const auto y = b.get();
            if (x != y)
                break;
            if (x == std::char_traits<char>::eof()) {
                if (!a.bad() && !b.bad())
                    return {};
                break;
            }
            if (x == '\n')
                ++line;
        }
    }
    return "stdout differs at line " + std::to_string(line);
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

BehaviourVerificationResult verifyBehaviour(const string& code, const string& unoptimizedCode,
        const vector<string>& cppFilePaths, const vector<string>& compilationOptions,
        const vector<string>& judgeCommand, const vector<string>& inputs,
        const string& temporaryDirectory) {
    BehaviourVerificationResult result;
    const VerificationFiles original(temporaryDirectory, "verify-original");
    const VerificationFiles candidate(temporaryDirectory);
    const VerificationFiles fallback(temporaryDirectory, "verify-unoptimized");
    VerificationResult build = checkJudge(original,
        originalJudgeCommand(judgeCommand, compilationOptions), cppFilePaths);
    if (!build.passed) {
        result.warnings = "caide inliner: warning: runtime verification was skipped: original "
            "does not build (" + build.reason + ")\n" + firstLines(build.output, 10);
        return result;
    }

    if (!writeCandidate(code, candidate, build) ||
        !(build = checkJudge(candidate, judgeCommand)).passed) {
        result.useUnoptimized = true;
        result.warnings = "caide inliner: warning: runtime verification candidate build failed (" +
            build.reason + "); writing unoptimized output\n" + firstLines(build.output, 10);
        return result;
    }

    for (const string& input : inputs) {
        std::ifstream sample(input, std::ios::binary);
        // Directories can be opened as ifstreams on some systems.
        if (!sample || !llvm::sys::fs::is_regular_file(input)) {
            result.warnings += "caide inliner: warning: cannot read runtime verification input " +
                input + "; skipping input\n";
            continue;
        }
        sample.close();
        const auto reference = execute({original.executable}, input, original.output, original.error, 10);
        const auto optimized = execute({candidate.executable}, input, candidate.output, candidate.error, 10);
        const string difference = behaviourDifference(reference, optimized, original.output, candidate.output);
        if (difference.empty())
            continue;

        result.useUnoptimized = true;
        result.warnings += "caide inliner: warning: optimized output behaves differently on " +
            input + " (" + difference + "); writing unoptimized output\n";
        if (writeCandidate(unoptimizedCode, fallback, build) &&
            (build = checkJudge(fallback, judgeCommand)).passed) {
            const auto unoptimized = execute({fallback.executable}, input, fallback.output, fallback.error, 10);
            if (!behaviourDifference(reference, unoptimized, original.output, fallback.output).empty())
                result.warnings += "caide inliner: warning: unoptimized output also behaves differently "
                    "(a stage-1 problem, or the program is nondeterministic)\n";
        } else {
            result.warnings += "caide inliner: warning: unoptimized output also fails with the "
                "judge compiler (a stage-1 or environment problem)\n" + firstLines(build.output, 10);
        }
        break;
    }
    return result;
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
