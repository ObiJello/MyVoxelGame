// File: src/common/network/AsioInclude.hpp
// Compatibility header for Boost.Asio and standalone Asio
#pragma once

// Windows-specific configuration for ASIO
#ifdef _WIN32
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0601  // Windows 7 minimum
    #endif
    #ifndef WINVER
        #define WINVER 0x0601
    #endif
#endif

#ifdef ASIO_STANDALONE
    // Using standalone Asio
    #include <asio.hpp>
    #include <asio/strand.hpp>
    namespace net = asio;
    using error_code = asio::error_code;
    namespace sys = asio;
#else
    // Using Boost.Asio
    #include <boost/asio.hpp>
    #include <boost/asio/strand.hpp>
    namespace net = boost::asio;
    using error_code = boost::system::error_code;
    namespace sys = boost::system;
#endif