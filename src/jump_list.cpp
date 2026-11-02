#include "jump_list.h"
#include "startup.h"

#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <winrt/base.h>

namespace refarter {

bool registerJumpList(const std::filesystem::path& executable) {
    try {
        const auto list = winrt::create_instance<ICustomDestinationList>(CLSID_DestinationList);
        UINT slots = 0;
        winrt::com_ptr<IObjectArray> removed;
        winrt::check_hresult(list->BeginList(&slots, IID_PPV_ARGS(removed.put())));
        const auto tasks = winrt::create_instance<IObjectCollection>(CLSID_EnumerableObjectCollection);
        for (const auto& entry : launchCommands) {
            const auto link = winrt::create_instance<IShellLinkW>(CLSID_ShellLink);
            winrt::check_hresult(link->SetPath(executable.c_str()));
            winrt::check_hresult(link->SetArguments(entry.flag));
            winrt::check_hresult(link->SetIconLocation(executable.c_str(), 0));
            // A task's name is its title property, not a file name.
            const auto properties = link.as<IPropertyStore>();
            PROPVARIANT title{};
            winrt::check_hresult(InitPropVariantFromString(entry.title, &title));
            const auto set = properties->SetValue(PKEY_Title, title);
            PropVariantClear(&title);
            winrt::check_hresult(set);
            winrt::check_hresult(properties->Commit());
            winrt::check_hresult(tasks->AddObject(link.get()));
        }
        winrt::check_hresult(list->AddUserTasks(tasks.as<IObjectArray>().get()));
        winrt::check_hresult(list->CommitList());
        return true;
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

} // namespace refarter
