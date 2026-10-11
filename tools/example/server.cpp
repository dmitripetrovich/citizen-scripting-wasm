#include <include/CppScriptRuntime.h>
#include <src/natives/cfx.h>
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
        fx::trace("Instantiated instance of script %s.\n", fx::getCurrentResourceName().c_str());

        auto pending = std::make_shared<std::unordered_map<std::string, std::string>>();
        auto players = std::make_shared<std::unordered_map<std::string, std::string>>();

        fx::onStop([players]()
        {
                fx::trace("Resource stopping, %zu players tracked.\n", players->size());
        });

        fx::on("playerConnecting", [pending](const std::string& source, std::string name)
        {
                (*pending)[extractId(source)] = name;
        });

        fx::on("playerJoining", [pending, players](const std::string& source, std::string oldId)
        {
                auto it = pending->find(oldId);
                std::string name = (it != pending->end()) ? it->second : "Unknown";
                if (it != pending->end())
                        pending->erase(it);
                std::string id = extractId(source);
                (*players)[id] = name;
                // typed wrapper from src/natives/cfx.h
                std::string license = fx::natives::cfx::GetPlayerIdentifierByType(id.c_str(), "license2");
                fx::trace("%s [%s] (%s) has connected. Players Online: %zu\n", name.c_str(), license.c_str(), id.c_str(), players->size());
        });

        fx::on("playerDropped", [players](const std::string& source, std::string reason)
        {
                std::string id = extractId(source);
                std::string name = players->count(id) ? (*players)[id] : "Unknown";
                std::string license = fx::natives::cfx::GetPlayerIdentifierByType(id.c_str(), "license2");
                players->erase(id);
                fx::trace("%s [%s] (%s) has disconnected (%s). Players Online: %zu\n", name.c_str(), license.c_str(), id.c_str(), reason.c_str(), players->size());
        });

        fx::onNet("chatMessage", [players](const std::string& source, int author, std::string name, std::string message)
        {
                fx::trace("%s (%d) has sent a chat message: %s\n", name.c_str(), author, message.c_str());
        });

        fx::addStateBagChangeHandler("", "", [](const std::string& bagName, const std::string& key, const fx::json::Value& value, int source, bool replicated)
        {
                fx::trace("[fx::addStateBagChangeHandler] %s:%s changed (source=%d, replicated=%s)\n", bagName.c_str(), key.c_str(), source, replicated ? "true" : "false");
        });

        fx::addExport("getPlayerInfo", [](fx::EventArgs args) -> fx::json::Value
        {
                if (args.size() == 0)
                        return "unknown";
                std::string src = std::to_string(args.get<int>(0));
                // string-based alternative (works without native includes)
                std::string name = fx::native<std::string>("GET_PLAYER_NAME", src.c_str());
                int ping = fx::native<int>("GET_PLAYER_PING", src.c_str());
                return name + " (ping=" + std::to_string(ping) + "ms)";
        });

        fx::onCommand("c++", [](const std::string& source, const std::vector<std::string>&)
        {
                if (source.empty() || source == "0")
                        return;
                int src = static_cast<int>(std::strtol(source.c_str(), nullptr, 10));
                std::string name = fx::getCurrentResourceName();
                fx::json::Value info = fx::callExport(name, "getPlayerInfo", { src });
                fx::trace("[fx::addExport] %s\n", info.asStr("failed").c_str());
                fx::callExport("fw", "SendChatMessage", { src, "^#f0a0e4[INFO] ^#ffffffHello from ^#f0a0e4C++^#ffffff!" });
        });
}
