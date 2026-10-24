#include "download_inventory.hpp"
#include <stdexcept>
#import <Foundation/Foundation.h>

@interface M6ReleaseRequest : NSObject <NSURLSessionDataDelegate, NSURLSessionTaskDelegate>
@property(nonatomic, strong) NSMutableData* bytes;
@property(nonatomic, strong) NSCondition* condition;
@property(nonatomic, strong) NSError* failure;
@property(nonatomic) BOOL done;
@property(nonatomic) NSInteger responseStatus;
@end
@implementation M6ReleaseRequest
- (instancetype)init {
    self = [super init];
    if (self) { self.bytes = [NSMutableData data]; self.condition = [[NSCondition alloc] init]; }
    return self;
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task willPerformHTTPRedirection:(NSHTTPURLResponse*)response
        newRequest:(NSURLRequest*)request completionHandler:(void (^)(NSURLRequest*))completionHandler {
    (void)session; (void)task; (void)response; (void)request;
    completionHandler(nil); // The fixed GitHub API endpoint must answer directly.
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveResponse:(NSURLResponse*)response
        completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    (void)session; (void)task;
    self.responseStatus = [response isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse*)response).statusCode : 0;
    const bool acceptable = [response isKindOfClass:[NSHTTPURLResponse class]] &&
        ((NSHTTPURLResponse*)response).statusCode == 200 && response.expectedContentLength <= 4 * 1024 * 1024;
    if (!acceptable) {
        [self.condition lock]; self.failure = [NSError errorWithDomain:@"MarkSixReleaseIdentity" code:1 userInfo:nil]; [self.condition unlock];
    }
    completionHandler(acceptable ? NSURLSessionResponseAllow : NSURLSessionResponseCancel);
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data {
    (void)session;
    [self.condition lock];
    if (data.length > 4 * 1024 * 1024 - self.bytes.length) {
        self.failure = [NSError errorWithDomain:@"MarkSixReleaseIdentity" code:2 userInfo:nil]; [task cancel];
    } else [self.bytes appendData:data];
    [self.condition unlock];
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    (void)session; (void)task;
    [self.condition lock];
    if (!self.failure) self.failure = error;
    self.done = YES; [self.condition signal]; [self.condition unlock];
}
@end

namespace marksix::maintenance {
std::string fetchReleasePage(unsigned page) {
    if (!page || page > 10) throw std::runtime_error("Invalid release catalogue page");
    @autoreleasepool {
        auto delegate = [[M6ReleaseRequest alloc] init];
        auto configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        configuration.URLCache = nil; configuration.HTTPCookieStorage = nil; configuration.URLCredentialStorage = nil;
        configuration.HTTPShouldSetCookies = NO;
        configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        configuration.timeoutIntervalForRequest = 15; configuration.timeoutIntervalForResource = 30;
        auto session = [NSURLSession sessionWithConfiguration:configuration delegate:delegate delegateQueue:nil];
        NSString* address = [NSString stringWithFormat:@"https://api.github.com/repos/brucerry/mark-six-studio/releases?per_page=100&page=%u", page];
        auto request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:address]];
        [request setValue:@"MarkSixStudio-maintenance/1" forHTTPHeaderField:@"User-Agent"];
        [request setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
        [request setValue:@"2022-11-28" forHTTPHeaderField:@"X-GitHub-Api-Version"];
        auto task = [session dataTaskWithRequest:request]; [task resume];
        [delegate.condition lock]; NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:31];
        while (!delegate.done && [delegate.condition waitUntilDate:deadline]) {}
        const bool success = delegate.done && !delegate.failure;
        NSData* data = [delegate.bytes copy]; [delegate.condition unlock]; [session invalidateAndCancel];
        if (!success) {
            std::string detail = "HTTP " + std::to_string(delegate.responseStatus);
            if (delegate.failure) detail += ", " + std::string(delegate.failure.domain.UTF8String) + " " + std::to_string(delegate.failure.code);
            else detail += ", request deadline exceeded";
            throw std::runtime_error("Official release metadata is unavailable, oversized or unverified (" + detail + "); downloads are preserved");
        }
        return {static_cast<const char*>(data.bytes), data.length};
    }
}
}
