add_executable(test-tool test-tool.cpp)

target_link_libraries(test-tool caideInliner)
target_include_directories(test-tool SYSTEM PRIVATE ${CLANG_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_compile_definitions(test-tool PRIVATE ${CLANG_DEFINITIONS} ${LLVM_DEFINITIONS})

set(CAIDE_TEST_CXX "" CACHE STRING "Judge compiler command for compile-and-run checks")

set(tests_dir "${CMAKE_SOURCE_DIR}/../tests/cases")
set(tests_temp_dir "${CMAKE_SOURCE_DIR}/../tests/temp")
set(clang_options_file "${CMAKE_CURRENT_BINARY_DIR}/clangOptions.txt")

add_custom_command(TARGET test-tool POST_BUILD
    COMMAND test-tool "${tests_temp_dir}" "--prepare" "${clang_options_file}"
    COMMENT "Detecting compilation options")

# To run tests: make test-tool && ctest
# To enable judge checks: cmake -DCAIDE_TEST_CXX="g++-15 -std=c++23" ...
# To run a specific test: ctest -R <test name>
# For verbose output: ctest --verbose

set(test_list actually-written-type aggregate-init-fields alias-in-template-argument base-class-of-template base-initializers compiler-macro-branch caide-concept-comment delayed-parsing error-undefined-type field-layout-sizeof friends gcc-builtins gcc-int128 global-side-effect-ctor global-side-effect-roots github-issue17 github-issue4 ident-to-keep include-option-std include-option-user inheriting-ctor inliner1 inliner2 inliner3 line-directives macros merge-namespaces merge-namespaces-2 merge-namespaces-nested merge-namespaces-nested-then-plain namespace-alias-dangling no-optimize pull-headers-up qualifiers record-layout-special-members references-from-template-arguments remove-comments remove-namespaces remove-template-functions remove-type-alias repair-restores-function repair-restores-type repair-restores-overloads repair-gives-up sizeof source-ranges static-assert static-assert-branch std-namespace stl strip-comments-all template-alias template-if-constexpr-member template-partial-spec-dependent templated-context template-friend template-variables track-parent-decls ull unused-fields verify-fallback-judge-fails verify-fallback-judge-missing using-base-grandparent using-base-member-template using-declarations)

function(add_test_directory test_name)
    add_test(NAME ${test_name}
        COMMAND test-tool "${tests_temp_dir}" "${clang_options_file}" "${tests_dir}/${test_name}")
    set_tests_properties(${test_name} PROPERTIES REQUIRED_FILES "${clang_options_file}")
    if(NOT "${CAIDE_TEST_CXX}" STREQUAL "")
        set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "CAIDE_TEST_CXX=${CAIDE_TEST_CXX}")
    endif()
endfunction()


foreach(test_name IN LISTS test_list)
    add_test_directory(${test_name})
endforeach()

if(NOT APPLE)
    # <bits/stdc++.h> exists only in GCC's libstdc++. On macOS, detection with
    # Homebrew GCC fails (Clang can't parse GCC's own stddef.h there:
    # __float128), so these run on Linux/Docker only.
    add_test_directory(gcc-autodetect)
    add_test_directory(gcc-bits-stdc++)
endif()

if(LLVM_PACKAGE_VERSION VERSION_GREATER_EQUAL "14")
    # Needs https://github.com/llvm/llvm-project/commit/4e4511df8d33a6fc02d5e46c681855db495187cd
    add_test_directory(enums)
endif()

if(LLVM_PACKAGE_VERSION VERSION_GREATER_EQUAL "19")
    # Need Sema APIs that are unavailable or too different in earlier versions.
    add_test_directory(github-issue8)
    add_test_directory(concepts-constraints)
    add_test_directory(concept-constrained-ctad)
    add_test_directory(concept-ctor-requires)
    add_test_directory(error-undefined-concept)
endif()

if(LLVM_PACKAGE_VERSION VERSION_GREATER_EQUAL "22")
    # C++23/C++26 language-feature coverage (-std=c++2c). Expected outputs were
    # generated and verified against clang 22; gate can be lowered per feature
    # once the etalon is confirmed stable on earlier clang versions.
    add_test_directory(cpp23-deducing-this)
    add_test_directory(cpp23-if-consteval)
    add_test_directory(cpp23-multidim-subscript)
    add_test_directory(cpp23-static-operator-call)
    add_test_directory(cpp26-pack-indexing)
endif()
