// Experiment (9 Oct 2026): can user space take the card away from Apple's
// AppleEthernetRL so this project's dext re-matches it? Needs root.
#include <IOKit/IOKitLib.h>
#include <IOKit/IOKitServer.h>
#include <stdio.h>
#include <string.h>
// Terminate every instance of a driver class (default AppleEthernetRL) so the
// device is re-matched; then ask the PCI device for a rescan.
int main(int argc, char **argv) {
    const char *cls = argc > 1 ? argv[1] : "AppleEthernetRL";
    io_name_t name; strlcpy(name, cls, sizeof(name));
    kern_return_t kr = IOCatalogueTerminate(kIOMainPortDefault, kIOCatalogServiceTerminate, name);
    printf("IOCatalogueTerminate(%s) = 0x%x (%s)\n", cls, kr,
           kr == kIOReturnSuccess ? "success" : kr == kIOReturnNotPrivileged ? "not privileged" :
           kr == kIOReturnUnsupported ? "unsupported" : "other");
    return kr == kIOReturnSuccess ? 0 : 1;
}
