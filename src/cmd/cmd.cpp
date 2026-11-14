//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#include "../caideInliner.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/StringSaver.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>


using namespace std;

static void printUsage(std::ostream& os, const char* prog) {
    os <<
        "Usage: " << prog << " [clang options...] [@response-file ...] -- [options] <source files...>\n"
        "\n"
        "Combines multiple C++ source files and user headers into a single file,\n"
        "keeping only code reachable from main (plus anything explicitly kept).\n"
        "\n"
        "Everything before '--' is passed to clang as compilation options. An\n"
        "argument of the form @FILE reads additional clang options, one per line,\n"
        "from FILE.\n"
        "\n"
        "Options after '--':\n"
        "  -d <dir>           Temporary directory for intermediate files (default ./caide-tmp)\n"
        "  -o <file>          Output file (default ./caide-tmp/result.cpp)\n"
        "  -k <macro>         Keep preprocessor branches guarded by <macro> (repeatable)\n"
        "  -i <name>          Keep the fully-qualified declaration <name> even if it is\n"
        "                     unreachable from main (repeatable)\n"
        "  -l <n>             Max consecutive empty lines in output; negative disables (default 2)\n"
        "  -a, --auto-detect  Auto-detect system include paths before applying clang options\n"
        "  -n, --no-optimize  Only inline into a single file; do not remove unreachable code\n"
        "  -c, --strip-comments  Remove all comments from the output\n"
        "  --verify-with <command>  Also compile and link with a quoted judge command\n"
        "                           (e.g. \"g++ -std=c++23 -O2\") after the embedded check\n"
        "  --verify-run <path>  Compare runs on an input file, or sorted .in files in a\n"
        "                       directory (no recursion; repeatable; requires --verify-with)\n"
        "  --no-verify        Disable output verification and fallback\n"
        "  --strict           Exit with code 3 after writing output if verification falls back\n"
        "  -h, --help         Show this help and exit\n"
        "\n"
        "Remaining arguments after '--' are treated as input source files.\n";
}

static vector<string> verificationInputs(const vector<string>& paths) {
    vector<string> inputs;
    for (const string& path : paths) {
        llvm::sys::fs::file_status status;
        const auto error = llvm::sys::fs::status(path, status);
        if (error)
            throw runtime_error("cannot access --verify-run path " + path + ": " + error.message());
        if (llvm::sys::fs::is_regular_file(status)) {
            inputs.push_back(path);
        } else if (llvm::sys::fs::is_directory(status)) {
            vector<string> files;
            std::error_code ec;
            for (llvm::sys::fs::directory_iterator it(path, ec), end;
                 !ec && it != end; it.increment(ec)) {
                const string file = it->path();
                if (file.size() >= 3 && file.compare(file.size() - 3, 3, ".in") == 0 &&
                    llvm::sys::fs::is_regular_file(file))
                    files.push_back(file);
            }
            if (ec)
                throw runtime_error("cannot read --verify-run directory " + path + ": " + ec.message());
            // All paths have the same directory prefix, so this sorts by name.
            sort(files.begin(), files.end());
            inputs.insert(inputs.end(), files.begin(), files.end());
        } else {
            throw runtime_error("--verify-run path is not a regular file or directory: " + path);
        }
    }
    return inputs;
}

int main(int argc, const char* argv[]) {
    // Help is honored wherever it appears, before any other parsing.
    for (int i = 1; i < argc; ++i) {
        const string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(std::cout, argv[0]);
            return 0;
        }
    }
    if (argc < 2) {
        printUsage(std::cerr, argv[0]);
        return 1;
    }

    vector<string> sourceFiles;
    string tmpDirectory = "./caide-tmp";
    string outputFile = "./caide-tmp/result.cpp";
    vector<string> clangOptions;
    vector<string> macrosToKeep;
    vector<string> identifiersToKeep;
    int maxConsecutiveEmptyLines = 2;
    bool autoDetect = false;
    bool noOptimize = false;
    bool stripComments = false;
    bool verifyOutput = true;
    bool strict = false;
    vector<string> verifyCommand;
    vector<string> verifyRunPaths;

    const string clangOptionsEnd = "--";
    const string directoryFlag = "-d";
    const string outputFlag = "-o";
    const string keepMacrosFlag = "-k";
    const string keepIdentifierFlag = "-i";
    const string emptyLinesFlag = "-l";
    const string autoDetectFlag = "-a";
    const string autoDetectFlagLong = "--auto-detect";
    const string noOptimizeFlag = "-n";
    const string noOptimizeFlagLong = "--no-optimize";
    const string stripCommentsFlag = "-c";
    const string stripCommentsFlagLong = "--strip-comments";

    int i = 1;
    for (; i < argc && clangOptionsEnd != argv[i]; ++i) {
        if (argv[i][0] != '@') {
            clangOptions.emplace_back(argv[i]);
        } else {
            const char* responseFile = &argv[i][1];
            ifstream in(responseFile);
            if (!in) {
                std::cerr << "Error: cannot open response file: " << responseFile << "\n";
                return 1;
            }
            string line;
            while (getline(in, line)) {
                clangOptions.emplace_back(line);
            }
        }
    }

    // Returns the argument of a flag that requires one, or exits with an error.
    auto requireArg = [&](int& idx, const string& flag) -> const char* {
        if (idx + 1 >= argc) {
            std::cerr << "Error: " << flag << " requires an argument\n";
            std::exit(1);
        }
        return argv[++idx];
    };

    for (++i; i < argc; ++i) {
        const string flag = argv[i];
        if (directoryFlag == flag) {
            tmpDirectory = requireArg(i, directoryFlag);
        } else if (outputFlag == flag) {
            outputFile = requireArg(i, outputFlag);
        } else if (keepMacrosFlag == flag) {
            macrosToKeep.emplace_back(requireArg(i, keepMacrosFlag));
        } else if (keepIdentifierFlag == flag) {
            identifiersToKeep.emplace_back(requireArg(i, keepIdentifierFlag));
        } else if (emptyLinesFlag == flag) {
            maxConsecutiveEmptyLines = strtol(requireArg(i, emptyLinesFlag), nullptr, 10);
        } else if (autoDetectFlag == flag || autoDetectFlagLong == flag) {
            autoDetect = true;
        } else if (noOptimizeFlag == flag || noOptimizeFlagLong == flag) {
            noOptimize = true;
        } else if (stripCommentsFlag == flag || stripCommentsFlagLong == flag) {
            stripComments = true;
        } else if (flag == "--verify-with") {
            const char* command = requireArg(i, flag);
            llvm::BumpPtrAllocator allocator;
            llvm::StringSaver saver(allocator);
            llvm::SmallVector<const char*, 16> args;
            llvm::cl::TokenizeGNUCommandLine(command, saver, args);
            if (args.empty()) {
                std::cerr << "Error: --verify-with requires a non-empty command\n";
                return 1;
            }
            verifyCommand.assign(args.begin(), args.end());
        } else if (flag == "--verify-run") {
            verifyRunPaths.emplace_back(requireArg(i, flag));
        } else if (flag == "--no-verify") {
            verifyOutput = false;
        } else if (flag == "--strict") {
            strict = true;
        } else {
            sourceFiles.emplace_back(argv[i]);
        }
    }

    if (!verifyRunPaths.empty() && verifyCommand.empty()) {
        std::cerr << "Error: --verify-run requires --verify-with\n";
        return 1;
    }

    if (sourceFiles.empty()) {
        std::cerr << "Error: no input source files\n\n";
        printUsage(std::cerr, argv[0]);
        return 1;
    }

    try {
        const auto error = llvm::sys::fs::create_directories(tmpDirectory);
        if (error)
            throw runtime_error("cannot create temporary directory " + tmpDirectory + ": " + error.message());
        caide::CppInliner inliner(tmpDirectory);
        inliner.verifyInputs = verificationInputs(verifyRunPaths);

        if (autoDetect) {
            // Detection replaces clangCompilationOptions, so apply the user's
            // options afterwards, letting them augment or override detected ones.
            inliner.autoDetectCompilationOptions();
            inliner.clangCompilationOptions.insert(inliner.clangCompilationOptions.end(),
                clangOptions.begin(), clangOptions.end());
        } else {
            inliner.clangCompilationOptions.swap(clangOptions);
        }

        inliner.macrosToKeep.insert(inliner.macrosToKeep.end(),
            macrosToKeep.begin(), macrosToKeep.end());
        inliner.identifiersToKeep.insert(inliner.identifiersToKeep.end(),
            identifiersToKeep.begin(), identifiersToKeep.end());
        inliner.maxConsequentEmptyLines = maxConsecutiveEmptyLines;
        inliner.removeUnusedCode = !noOptimize;
        inliner.removeComments = stripComments;
        inliner.verifyOutput = verifyOutput;
        inliner.verifyCommand = verifyCommand;
        const auto outcome = inliner.inlineCode(sourceFiles, outputFile);
        if (strict && outcome == caide::InlineOutcome::FellBack)
            return 3;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
