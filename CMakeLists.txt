cmake_minimum_required(VERSION 3.16)
project(libnetease C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra)
endif()

# ── libnetease: static library ──────────────────────────
find_package(CURL REQUIRED)
# zlib optional: used for compressed PNG IDAT; without it qrenc falls back to
# stored (uncompressed) deflate — the PNG stays valid either way.
find_package(ZLIB)
add_library(netease STATIC
  # vendor: zero-dependency crypto primitives (weapi/eapi spec)
  src/vendor/base64.c
  src/vendor/hex.c
  src/vendor/md5.c
  src/vendor/aes.c
  src/vendor/bignum.c
  src/vendor/rsa.c
  # vendor: go-qrcode port (Medium level)
  src/vendor/qrenc.c
  # core
  src/core/util.c
  src/core/rand.c
  src/core/jmap.c
  src/core/jval.c
  src/core/deviceids.c
  src/core/crypto.c
  src/core/http.c
  src/core/cookiejar.c
  src/core/request.c
  # services
  src/service/qr.c
  src/service/services.c
)
target_include_directories(netease PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(netease PUBLIC CURL::libcurl)
if(ZLIB_FOUND)
  target_compile_definitions(netease PRIVATE NE_HAVE_ZLIB)
  target_link_libraries(netease PUBLIC ZLIB::ZLIB)
endif()

# ── netease-cli: drop-in replacement shell (same name & protocol as the Go build)
add_executable(netease-cli src/cli/main.c)
target_link_libraries(netease-cli PRIVATE netease)

# ── tests ───────────────────────────────────────────────
enable_testing()
add_executable(test_crypto tests/test_crypto.c)
target_link_libraries(test_crypto PRIVATE netease)
add_test(NAME crypto COMMAND test_crypto)

add_executable(test_jval tests/test_jval.c)
target_link_libraries(test_jval PRIVATE netease)
add_test(NAME jval COMMAND test_jval)

add_executable(test_request tests/test_request.c)
target_link_libraries(test_request PRIVATE netease)
add_test(NAME rewrite COMMAND test_request)

add_executable(test_qr tests/test_qr.c)
target_link_libraries(test_qr PRIVATE netease)
add_test(NAME qr COMMAND test_qr)

# differential harness: stub music.163.com + 40 cases covering every CLI
# command, error paths and cookie-jar state; --go <binary> adds a byte
# for-byte dual-run against the Go shell (see tests/dualrun.py)
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME dualrun
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/dualrun.py
            --cli $<TARGET_FILE:netease-cli>)
endif()
