#include "Icons.h"
#include "MapMesh.h"
#include "Menu.h"
#include "Overlay.h"
#include "MiniMap.h"
#include "Settings.h"

namespace
{
    void SetupLog()
    {
        auto path = logger::log_directory();
        if (!path) {
            return;
        }
        *path /= "DetailedMiniMap.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));
        log->set_level(spdlog::level::warn);  // warnings and errors only, unless the detailed log is on (Settings)
        log->flush_on(spdlog::level::warn);
        log->set_pattern("[%H:%M:%S] [%l] %v");
        spdlog::set_default_logger(std::move(log));
        spdlog::flush_every(std::chrono::seconds(3));  // the detailed log: out every few seconds, not a disk write a line
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg)
    {
        if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
            MapMesh::Prepare();
            Overlay::Prepare();
            Menu::Register();
            MiniMap::Register();
        } else if (a_msg->type == SKSE::MessagingInterface::kPostLoadGame || a_msg->type == SKSE::MessagingInterface::kNewGame) {
            MiniMap::OnGameLoaded();
            MapMesh::ReloadFilter();
        }
    }
}

// nothing goes into the save or to the disk: the map is what the game has loaded around the character
SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    SetupLog();
    Settings::Load();
    Icons::UseStyle(Settings::Map().look.iconStyle);
    MiniMap::Install();
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
