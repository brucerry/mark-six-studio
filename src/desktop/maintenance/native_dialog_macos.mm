#include "native_dialog.hpp"
#include "storage_inventory.hpp"
#import <AppKit/AppKit.h>

namespace marksix::maintenance {
namespace {
NSString* ns(const std::string& value) {
    NSString* text = [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
    if (text) return text;
    const auto escaped = quotedReportField(value);
    return [[NSString alloc] initWithBytes:escaped.data() length:escaped.size() encoding:NSUTF8StringEncoding];
}
}
void nativeStartupBlocked(const std::string& message) {
    const auto text = CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(message.data()), CFIndex(message.size()), kCFStringEncodingUTF8, false);
    CFOptionFlags response = 0;
    (void)CFUserNotificationDisplayAlert(0, kCFUserNotificationNoteAlertLevel, nullptr, nullptr, nullptr,
        CFSTR("Mark Six Studio cannot start"), text ? text : CFSTR("System maintenance state could not be verified."), CFSTR("Close"), nullptr, nullptr, &response);
    if (text) CFRelease(text);
}
bool nativeReview(const std::string& title, const std::string& report, const std::string& approve, const std::string& cancel) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = ns(title);
        alert.informativeText = @"Review the details below. Select text to copy full paths and inspection notes.";
        if (!approve.empty()) [alert addButtonWithTitle:ns(approve)];
        [alert addButtonWithTitle:ns(cancel)];
        NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 660, 380)];
        scroll.hasVerticalScroller = YES;
        NSTextView* text = [[NSTextView alloc] initWithFrame:scroll.contentView.bounds];
        text.editable = NO; text.selectable = YES;
        text.font = [NSFont systemFontOfSize:13]; text.string = ns(report);
        text.autoresizingMask = NSViewWidthSizable;
        text.textContainer.containerSize = NSMakeSize(scroll.contentSize.width, CGFLOAT_MAX);
        text.textContainer.widthTracksTextView = YES;
        [text setAccessibilityLabel:@"Maintenance details and preserved legacy files"];
        scroll.documentView = text; alert.accessoryView = scroll;
        alert.window.restorable = NO;
        if (!approve.empty()) {
            NSButton* approveButton = alert.buttons.firstObject;
            NSButton* cancelButton = alert.buttons.lastObject;
            approveButton.keyEquivalent = @"";
            cancelButton.keyEquivalent = @"\r";
            alert.window.defaultButtonCell = cancelButton.cell;
            alert.window.initialFirstResponder = cancelButton;
        }
        [NSApp activateIgnoringOtherApps:YES];
        const auto result = [alert runModal];
        return !approve.empty() && result == NSAlertFirstButtonReturn;
    }
}
struct NativeProgress::Impl {
    NSWindow* window;
    NSTextField* label;
    NSProgressIndicator* indicator;
};
NativeProgress::NativeProgress() : impl_(std::make_unique<Impl>()) {
    [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    impl_->window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 600, 170)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    impl_->window.title = @"Mark Six Studio cleanup in progress";
    impl_->window.restorable = NO; impl_->window.releasedWhenClosed = NO;
    impl_->label = [NSTextField wrappingLabelWithString:@"Preparing native maintenance. Keep this helper open."];
    impl_->label.frame = NSMakeRect(24, 54, 552, 92); impl_->label.selectable = YES;
    [impl_->label setAccessibilityLabel:@"Cleanup progress"];
    [impl_->window.contentView addSubview:impl_->label];
    impl_->indicator = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(24, 24, 552, 20)];
    impl_->indicator.style = NSProgressIndicatorStyleBar; impl_->indicator.indeterminate = NO;
    [impl_->window.contentView addSubview:impl_->indicator];
    [impl_->window center]; [impl_->window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
}
NativeProgress::~NativeProgress() { [impl_->window close]; }
void NativeProgress::update(const std::string& message, std::uint64_t completed, std::uint64_t total) {
    impl_->label.stringValue = ns(message + "\n" + std::to_string(completed) + " / " + std::to_string(total) +
        "\nThe final result will list preserved legacy files and remaining work.");
    impl_->indicator.maxValue = double(total ? total : 1); impl_->indicator.doubleValue = double(completed);
    // Pump only the helper's native UI. No Qt/runtime or workspace is reopened.
    for (unsigned i = 0; i < 64; ++i) {
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES];
        if (!event) break; [NSApp sendEvent:event];
    }
    [NSApp updateWindows]; [impl_->window displayIfNeeded];
}

}
