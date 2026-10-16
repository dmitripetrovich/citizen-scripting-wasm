#include <include/CppSdk.h>
#include <src/natives/all.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>

CFX_WORKER(countPrimes)
{
        int limit = std::atoi(std::string(input, static_cast<size_t>(input_len)).c_str());
        int count = 0;
        for (int n = 2; n < limit; ++n)
        {
                bool prime = true;
                for (int d = 2; d * d <= n && prime; ++d)
                        prime = n % d != 0;
                count += prime;
        }
        std::string out = std::to_string(count);
        int32_t len = std::min(static_cast<int32_t>(out.size()), result_max);
        std::memcpy(result, out.data(), static_cast<size_t>(len));
        return len;
}

Server
{
        cfx::trace("Instantiated instance of script %s.\n", cfx::getCurrentResourceName().c_str());

        auto pending = std::make_shared<std::unordered_map<int, std::string>>();
        auto players = std::make_shared<std::unordered_map<int, std::string>>();

        cfx::onStop([players]()
        {
                cfx::trace("Resource stopping, %zu players tracked.\n", players->size());
        });

        cfx::on("playerConnecting", [pending](int source, std::string name)
        {
                (*pending)[source] = name;
        });

        cfx::on("playerJoining", [pending, players](int source, std::string oldId)
        {
                auto it = pending->find(std::atoi(oldId.c_str()));
                std::string name = (it != pending->end()) ? it->second : "Unknown";
                if (it != pending->end())
                        pending->erase(it);
                (*players)[source] = name;
                // typed wrapper from src/natives/all.h
                std::string license = cfx::natives::GetPlayerIdentifierByType(source, "license2");
                cfx::trace("%s [%s] (%d) has connected. Players Online: %zu\n", name.c_str(), license.c_str(), source, players->size());
        });

        cfx::on("playerDropped", [players](int source, std::string reason)
        {
                std::string name = players->count(source) ? (*players)[source] : "Unknown";
                std::string license = cfx::natives::GetPlayerIdentifierByType(source, "license2");
                players->erase(source);
                cfx::trace("%s [%s] (%d) has disconnected (%s). Players Online: %zu\n", name.c_str(), license.c_str(), source, reason.c_str(), players->size());
        });

        cfx::onNet("chatMessage", [players](int source, int author, std::string name, std::string message)
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

        cfx::setTimeout(5000, []()
        {
                cfx::trace("[cfx::setTimeout] 5 seconds since start.\n");
        });

        auto minutes = std::make_shared<int>(0);
        cfx::setInterval(60000, [minutes]()
        {
                cfx::trace("[cfx::setInterval] Up for %d minute(s).\n", ++*minutes);
        });

        int32_t worker = cfx::createWorker("countPrimes", "100000");
        if (worker < 0)
                cfx::trace("[cfx::createWorker] Failed to start worker (%d).\n", worker);
        else
        {
                auto poll = std::make_shared<int32_t>(0);
                *poll = cfx::setInterval(100, [worker, poll]()
                {
                        cfx::WorkerResult result = cfx::pollWorker(worker);
                        if (result.isRunning())
                                return;
                        cfx::clearInterval(*poll);
                        if (result.isDone())
                                cfx::trace("[cfx::createWorker] %s primes below 100000.\n", result.output.c_str());
                        else
                                cfx::trace("[cfx::createWorker] Worker failed (%d).\n", result.status);
                });
        }

        cfx::onCommand("c++", [](int source, const std::vector<std::string>&)
        {
                if (source == 0)
                        return;
                std::string name = cfx::getCurrentResourceName();
                cfx::json::Value info = cfx::callExport(name, "getPlayerInfo", { source });
                cfx::trace("[cfx::addExport] %s\n", info.asStr("failed").c_str());
                cfx::callExport("fw", "SendChatMessage", { source, "^#f0a0e4[INFO] ^#ffffffHello from ^#f0a0e4C++^#ffffff!" });
        }, false);
}
