#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
// Ask IOKit to re-probe the Realtek PCI device so driver matching runs again.
static uint16_t u16prop(io_registry_entry_t e, const char *key) {
    CFDataRef d = IORegistryEntryCreateCFProperty(e, CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8), NULL, 0);
    if (!d || CFGetTypeID(d) != CFDataGetTypeID() || CFDataGetLength(d) < 2) return 0;
    const UInt8 *p = CFDataGetBytePtr(d); uint16_t v = p[0] | (p[1] << 8); CFRelease(d); return v;
}
int main(int argc, char **argv) {
    uint32_t options = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 0;
    io_iterator_t it; if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOPCIDevice"), &it) != KERN_SUCCESS) return 1;
    io_service_t dev; int n = 0;
    while ((dev = IOIteratorNext(it))) {
        if (u16prop(dev, "vendor-id") == 0x10ec) {
            uint16_t id = u16prop(dev, "device-id");
            kern_return_t kr = IOServiceRequestProbe(dev, options);
            printf("device 10ec:%04x options 0x%x -> IOServiceRequestProbe = 0x%x (%s)\n", id, options, kr,
                   kr == kIOReturnSuccess ? "success" : kr == kIOReturnUnsupported ? "unsupported" : kr == kIOReturnNotPrivileged ? "not privileged" : "other");
            n++;
        }
        IOObjectRelease(dev);
    }
    IOObjectRelease(it);
    if (!n) printf("no Realtek PCI device found\n");
    return 0;
}
