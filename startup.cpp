#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <appmodel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include "startup.h"

namespace
{
    using winrt::Windows::ApplicationModel::StartupTask;
    using winrt::Windows::ApplicationModel::StartupTaskState;

    constexpr wchar_t kTaskId[] = L"NightStartup";

    Startup::State Map(StartupTaskState s)
    {
        switch (s)
        {
        case StartupTaskState::Enabled:
        case StartupTaskState::EnabledByPolicy: return Startup::State::Enabled;
        case StartupTaskState::DisabledByUser: return Startup::State::DisabledByUser;
        case StartupTaskState::DisabledByPolicy: return Startup::State::DisabledByPolicy;
        default: return Startup::State::Disabled;
        }
    }
}

namespace Startup
{
    bool Packaged()
    {
        UINT32 len = 0;
        return GetCurrentPackageFullName(&len, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
    }

    State Query()
    {
        try
        {
            winrt::init_apartment();
            StartupTask task = StartupTask::GetAsync(kTaskId).get();
            return Map(task.State());
        }
        catch (...) { return State::Error; }
    }

    State Set(bool on)
    {
        try
        {
            winrt::init_apartment();
            StartupTask task = StartupTask::GetAsync(kTaskId).get();
            if (on) return Map(task.RequestEnableAsync().get());
            task.Disable();
            return Map(task.State());
        }
        catch (...) { return State::Error; }
    }

    int ActivationKind()
    {
        if (!Packaged()) return -1;
        try
        {
            winrt::init_apartment();
            auto args = winrt::Windows::ApplicationModel::AppInstance::GetActivatedEventArgs();
            return args ? (int)args.Kind() : -1;
        }
        catch (...) { return -1; }
    }

    bool LaunchedByStartupTask()
    {
        return ActivationKind() == (int)winrt::Windows::ApplicationModel::Activation::ActivationKind::StartupTask;
    }
}