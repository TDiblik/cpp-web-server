#pragma once

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    #define __IS_BSD__
#elif defined(__linux__)
    #define __IS_LINUX__
#elif defined(_WIN32) || defined(_WIN64)
    #define __IS_WINDOWS__
    #error "Unsupported operating system (Windows). This server requires macOS, BSD, or Linux."
#else
    #error "Unknown and unsupported operating system."
#endif
