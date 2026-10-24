#include "installer_lease.hpp"
#import <AppKit/AppKit.h>

namespace marksix::maintenance {
void launchNativeInstaller(const fs::path& package, InstallerCompletion completed) {
    const auto text = pathUtf8(package);
    NSString* path = [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding];
    NSURL* installer = [NSURL fileURLWithPath:@"/System/Library/CoreServices/Installer.app"];
    auto configuration = [NSWorkspaceOpenConfiguration configuration];
    configuration.activates = YES;
    [[NSWorkspace sharedWorkspace] openURLs:@[[NSURL fileURLWithPath:path]] withApplicationAtURL:installer
        configuration:configuration completionHandler:^(NSRunningApplication* application, NSError* error) {
            if (error || !application) { completed({false, {}, "The native Installer could not open this package."}); return; }
            completed({true, processIdentity(std::uint32_t(application.processIdentifier)), {}});
        }];
}
}
