#ifndef MISRC_LIBUSB_COMPAT_H
#define MISRC_LIBUSB_COMPAT_H

/*
 * Portable libusb include shim:
 * - Most Linux/macOS distro pkg-config setups expose <libusb-1.0/libusb.h>.
 * - Some Windows toolchains/packages expose <libusb.h>.
 */
#if defined(__has_include)
#if __has_include(<libusb-1.0/libusb.h>)
#include <libusb-1.0/libusb.h>
#elif __has_include(<libusb.h>)
#include <libusb.h>
#else
#error "libusb header not found (tried <libusb-1.0/libusb.h> and <libusb.h>)"
#endif
#elif defined(_WIN32)
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#endif /* MISRC_LIBUSB_COMPAT_H */
