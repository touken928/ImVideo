// macOS native file open panel (Objective-C++)
#import <AppKit/AppKit.h>

#include "file_dialog.hpp"

namespace imv {

bool OpenVideoFiles(std::vector<std::string>& out_paths) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.allowsMultipleSelection = NO;
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.title = @"Open Video File";
        panel.prompt = @"Open";

        if ([panel runModal] == NSModalResponseOK) {
            for (NSURL* url in panel.URLs) {
                if (url.path)
                    out_paths.push_back(std::string(url.path.UTF8String));
            }
            return !out_paths.empty();
        }
        return false;
    }
}

} // namespace imv
