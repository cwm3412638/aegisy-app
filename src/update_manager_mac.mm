#include "update_manager.h"

#import <Sparkle/Sparkle.h>

@interface AegisyUpdaterBridge : NSObject

@property(nonatomic, strong) SPUStandardUpdaterController *controller;

@end

@implementation AegisyUpdaterBridge
@end

namespace {

AegisyUpdaterBridge *bridgeFor(void *platformUpdater)
{
    return (__bridge AegisyUpdaterBridge *)platformUpdater;
}

} // namespace

UpdateManager::UpdateManager(QObject *parent)
    : QObject(parent)
{
    @autoreleasepool {
        AegisyUpdaterBridge *bridge = [[AegisyUpdaterBridge alloc] init];
        bridge.controller = [[SPUStandardUpdaterController alloc]
            initWithStartingUpdater:YES
            updaterDelegate:nil
            userDriverDelegate:nil];
        m_platformUpdater = (__bridge_retained void *)bridge;
    }
}

UpdateManager::~UpdateManager()
{
    @autoreleasepool {
        if (m_platformUpdater) {
            AegisyUpdaterBridge *bridge =
                (__bridge_transfer AegisyUpdaterBridge *)m_platformUpdater;
            bridge.controller = nil;
            m_platformUpdater = nullptr;
        }
    }
}

bool UpdateManager::isSupported() const
{
    return m_platformUpdater != nullptr;
}

bool UpdateManager::automaticallyChecksForUpdates() const
{
    @autoreleasepool {
        AegisyUpdaterBridge *bridge = bridgeFor(m_platformUpdater);
        return bridge && bridge.controller.updater.automaticallyChecksForUpdates;
    }
}

void UpdateManager::checkForUpdates()
{
    @autoreleasepool {
        AegisyUpdaterBridge *bridge = bridgeFor(m_platformUpdater);
        [bridge.controller checkForUpdates:nil];
    }
}

void UpdateManager::setAutomaticallyChecksForUpdates(bool enabled)
{
    @autoreleasepool {
        AegisyUpdaterBridge *bridge = bridgeFor(m_platformUpdater);
        if (!bridge) {
            return;
        }
        bridge.controller.updater.automaticallyChecksForUpdates = enabled;
        emit automaticChecksChanged(enabled);
    }
}
