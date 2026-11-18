cmake_minimum_required(VERSION 3.20)
project(SkywardAirways LANGUAGES CXX)

option(SKYWARD_WITH_POSTGRES "Enable PostgreSQL persistence" OFF)
option(SKYWARD_BUILD_TESTS "Build the backend tests" ON)

find_package(Threads REQUIRED)

add_library(skyward_core
    backend/AirlineReservationSystem.cpp
    backend/PostgresStore.cpp
)
target_include_directories(skyward_core PUBLIC backend)
target_compile_features(skyward_core PUBLIC cxx_std_17)
target_link_libraries(skyward_core PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(skyward_core PUBLIC bcrypt)
else()
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    target_link_libraries(skyward_core PUBLIC OpenSSL::Crypto)
endif()

if(SKYWARD_WITH_POSTGRES)
    find_package(PostgreSQL REQUIRED)
    target_compile_definitions(skyward_core PRIVATE SKYWARD_WITH_POSTGRES)
    target_link_libraries(skyward_core PUBLIC PostgreSQL::PostgreSQL)
endif()

add_executable(airline_reservation_server backend/main.cpp)
target_link_libraries(airline_reservation_server PRIVATE skyward_core)

if(WIN32)
    target_link_libraries(airline_reservation_server PRIVATE ws2_32 shell32)
endif()

if(MSVC)
    target_compile_options(skyward_core PRIVATE /W4 /permissive-)
    target_compile_options(airline_reservation_server PRIVATE /W4 /permissive-)
else()
    target_compile_options(skyward_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(airline_reservation_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SKYWARD_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        add_executable(airline_reservation_tests tests/backend_tests.cpp)
        target_link_libraries(airline_reservation_tests PRIVATE skyward_core)
        add_test(NAME airline_reservation_tests COMMAND airline_reservation_tests)
        set_tests_properties(airline_reservation_tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif()
endif()
