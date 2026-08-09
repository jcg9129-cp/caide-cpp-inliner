//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#include "../caideInliner.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
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

        inliner.clangCompilationOptions.push_back(std::move(opt));
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
    for (int i = 0; i < minLength; ++i) {
        // TODO: Print line numbers
        if (output[i] != etalon[i]) {
            std::cout
                << "< " << etalon[i] << "\n"
                << "> " << output[i] << "\n";
            return false;
        }
    }

    if (output.size() < etalon.size()) {
        std::cout << "Unexpected end of file: " << outputFilePath << "\n";
        return false;
    }

    if (output.size() > etalon.size()) {
        std::cout << "Unexpected end of file: " << etalonFilePath << "\n";
        return false;
    }

    return true;
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

