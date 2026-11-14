add_executable(cmd cmd.cpp)
target_link_libraries(cmd caideInliner)
target_include_directories(cmd SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(cmd PRIVATE ${LLVM_DEFINITIONS})

