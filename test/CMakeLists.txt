if (BUILD_TEST)
    # Copy test files
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/resources")
    file(GLOB test_files "resources/*")
    file(COPY ${test_files} DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/resources")

    # Find test code
    file(GLOB_RECURSE test_codes "*.c" "*.cc" "*.cpp" "*.h" "*.hpp")

    find_package(Threads)

    add_definitions(-D UNIT_TEST)

    add_executable(unit_test ${test_codes} ${src_files})
    target_include_directories(unit_test PUBLIC
            ../src)
    target_link_libraries(unit_test gtest ${CMAKE_THREAD_LIBS_INIT})
    add_test(NAME "RunUnitTest"
            COMMAND "./unit_test")
    # Link
    target_link_libraries(unit_test
            Threads::Threads
            json
            ${glog_LIBRARY}
            ${Unwind_LIBRARY}
            )
    target_include_directories(unit_test PUBLIC
            ${glog_INCLUDE}
            )
    # Coverage test
    add_coverage(unit_test)

    if (MSVC)
        target_compile_options(unit_test PRIVATE /W4 /WX-)

        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Ox")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /Od")

        # VS 2017 removed tr1
        add_definitions(-DGTEST_HAS_TR1_TUPLE=0)
    else ()
        target_compile_options(unit_test PRIVATE -g -Wall -Wextra -Wpedantic)
    endif ()
endif ()
