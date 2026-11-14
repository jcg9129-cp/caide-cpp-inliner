/*                        Caide C++ inliner

   This file is distributed under the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or (at your
   option) any later version. See LICENSE.TXT for details.
*/

#pragma once

/*!
    \file caideInliner.h
    \brief C interface for caide inliner API

    See documentation in caideInliner.hpp file for details.
*/

#ifdef __cplusplus
extern "C" {
#endif

struct CaideCppInlinerOptions {
    const char* temporaryDirectory;

    const char** clangCompilationOptions;
    int numClangOptions;

    /*! Macro names to add to the default compiler and OS macros to keep. */
    const char** macrosToKeep;
    int numMacrosToKeep;

    const char** identifiersToKeep;
    int numIdentifiersToKeep;

    int maxConsequentEmptyLines;
};

/*! Returns 0 on success (including verification fallback), 1 on a standard
    exception, or 2 on an unknown exception.
*/
int caideInlineCppCode(
        const struct CaideCppInlinerOptions* options,
        const char** cppFilePaths,
        int numCppFiles,
        const char* outputFilePath);

/*! Inline with embedded verification and an optional judge compiler command.
    verifyCommand contains numVerifyArgs argv entries; source and output paths
    are appended by the library. Pass zero arguments for embedded checking only.
    Returns 3 when verification forces unoptimized output, after writing it.
    Other return codes are the same as caideInlineCppCode.
*/
int caideInlineCppCodeVerified(
        const struct CaideCppInlinerOptions* options,
        const char** verifyCommand,
        int numVerifyArgs,
        const char** cppFilePaths,
        int numCppFiles,
        const char* outputFilePath);

#ifdef __cplusplus
} /* extern "C" */
#endif
