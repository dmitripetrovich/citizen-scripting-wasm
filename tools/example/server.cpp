#include <include/CppSdk.h>
#include <src/natives/all.h>
#include <cstdlib>
#include <memory>
#include <unordered_map>

static std::string extractId(const std::string& source)
{
        auto pos = source.find(':');
        return pos != std::string::npos ? source.substr(pos + 1) : source;
}

Server
{
        cfx::trace("Instantiated instance of script %s.\n", cfx::getCurrentResourceName().c_str());

        auto pending = std::make_shared<std::unordered_map<std::string, std::string>>();
        auto players = std::make_shared<std::unordered_map<std::string, std::string>>();

        cfx::onStop([players]()
        {
                cfx::trace("Resource stopping, %zu players tracked.\n", players->size());
        });

        cfx::on("playerConnecting", [pending](const std::string& source, std::string name)
        {
                (*pending)[extractId(source)] = name;
        });

        cfx::on("playerJoining", [pending, players](const std::string& source, std::string oldId)
        {
                auto it = pending->find(oldId);
                std::string name = (it != pending->end()) ? it->second : "Unknown";
                if (it != pending->end())
                        pending->erase(it);
                std::string id = extractId(source);
                (*players)[id] = name;
                // typed wrapper from src/natives/all.h
                std::string license = cfx::natives::cfx::GetPlayerIdentifierByType(id.c_str(), "license2");
                cfx::trace("%s [%s] (%s) has connected. Players Online: %zu\n", name.c_str(), license.c_str(), id.c_str(), players->size());
        });

        cfx::on("playerDropped", [players](const std::string& source, std::string reason)
        {
                std::string id = extractId(source);
                std::string name = players->count(id) ? (*players)[id] : "Unknown";
                std::string license = cfx::natives::cfx::GetPlayerIdentifierByType(id.c_str(), "license2");
                players->erase(id);
                cfx::trace("%s [%s] (%s) has disconnected (%s). Players Online: %zu\n", name.c_str(), license.c_str(), id.c_str(), reason.c_str(), players->size());
        });

        cfx::onNet("chatMessage", [players](const std::string& source, int author, std::string name, std::string message)
        {
                cfx::trace("%s (%d) has sent a chat message: %s\n", name.c_str(), author, message.c_str());
        });

        cfx::addStateBagChangeHandler("", "", [](const std::string& bagName, const std::string& key, const cfx::json::Value& value, int source, bool replicated)
        {
                cfx::trace("[cfx::addStateBagChangeHandler] %s:%s changed (source=%d, replicated=%s)\n", bagName.c_str(), key.c_str(), source, replicated ? "true" : "false");
        });

        cfx::addExport("getPlayerInfo", [](cfx::EventArgs args) -> cfx::json::Value
        {
                if (args.size() == 0)
                        return "unknown";
                std::string src = std::to_string(args.get<int>(0));
                // string-based alternative (works without native includes)
                std::string name = cfx::native<std::string>("GET_PLAYER_NAME", src.c_str());
                int ping = cfx::native<int>("GET_PLAYER_PING", src.c_str());
                return name + " (ping=" + std::to_string(ping) + "ms)";
        });

        cfx::onCommand("c++", [](const std::string& source, const std::vector<std::string>&)
        {
                if (source.empty() || source == "0")
                        return;
                int src = static_cast<int>(std::strtol(source.c_str(), nullptr, 10));
                std::string name = cfx::getCurrentResourceName();
                cfx::json::Value info = cfx::callExport(name, "getPlayerInfo", { src });
                cfx::trace("[cfx::addExport] %s\n", info.asStr("failed").c_str());
                cfx::callExport("fw", "SendChatMessage", { src, "^#f0a0e4[INFO] ^#ffffffHello from ^#f0a0e4C++^#ffffff!" });
        });
}
