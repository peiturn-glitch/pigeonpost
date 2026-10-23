cmake_minimum_required(VERSION 3.20)
project(pigeonpost_backend CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GMIME REQUIRED IMPORTED_TARGET gmime-3.0)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
pkg_check_modules(MARIADB REQUIRED IMPORTED_TARGET libmariadb)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

set(HTTPLIB_INCLUDE_DIR "" CACHE PATH "Directory containing httplib.h")
find_path(HTTPLIB_HEADER_DIR httplib.h HINTS ${HTTPLIB_INCLUDE_DIR} REQUIRED)

add_executable(brevduva-server
  src/main.cpp
  src/ai.cpp
  src/ai_sort.cpp
  src/api.cpp
  src/auth.cpp
  src/backup.cpp
  src/config.cpp
  src/crypto.cpp
  src/db.cpp
  src/db_cache.cpp
  src/gdrive.cpp
  src/imap.cpp
  src/mail_cache.cpp
  src/mail_service.cpp
  src/mime.cpp
  src/mutf7.cpp
  src/oauth.cpp
  src/push.cpp
  src/raw_cache.cpp
  src/net.cpp
  src/smtp.cpp
  src/text_util.cpp
  src/thunderbird_prefs.cpp
  src/zip_writer.cpp
)

target_include_directories(brevduva-server PRIVATE ${HTTPLIB_HEADER_DIR})
# Gzip-komprimerade svar (och uppackning av komprimerade anrop) i httplib; zlib är redan länkat för mejlcachen.
target_compile_definitions(brevduva-server PRIVATE CPPHTTPLIB_ZLIB_SUPPORT)
target_compile_options(brevduva-server PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(brevduva-server PRIVATE
  PkgConfig::GMIME
  OpenSSL::SSL OpenSSL::Crypto
  CURL::libcurl
  PkgConfig::MARIADB
  nlohmann_json::nlohmann_json
  Threads::Threads
  ZLIB::ZLIB
)
