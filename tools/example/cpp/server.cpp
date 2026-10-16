#include <include/CppSdk.h>
#include <src/natives/all.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <print>
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
        std::println("Instantiated instance of script {}.", cfx::getCurrentResourceName());

        auto pending = std::make_shared<std::unordered_map<int, std::string>>();
        auto players = std::make_shared<std::unordered_map<int, std::string>>();

        cfx::onStop([players]()
        {
                std::println("Resource stopping, {} players tracked.", players->size());
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
                std::println("{} [{}] ({}) has connected. Players Online: {}", name, license, source, players->size());
        });

        cfx::on("playerDropped", [players](int source, std::string reason)
        {
                std::string name = players->count(source) ? (*players)[source] : "Unknown";
                std::string license = cfx::natives::GetPlayerIdentifierByType(source, "license2");
                players->erase(source);
                std::println("{} [{}] ({}) has disconnected ({}). Players Online: {}", name, license, source, reason, players->size());
        });

        cfx::onNet("chatMessage", [players](int source, int author, std::string name, std::string message)
        {
                std::println("{} ({}) has sent a chat message: {}", name, author, message);
        });

        cfx::addStateBagChangeHandler("", "", [](const std::string& bagName, const std::string& key, const cfx::json::Value& value, int source, bool replicated)
        {
                std::println("[cfx::addStateBagChangeHandler] {}:{} changed (source={}, replicated={})", bagName, key, source, replicated);
        });

        cfx::addExport("getPlayerInfo", [](int source)
        {
                std::string src = std::to_string(source);
                // string-based alternative (works without native includes)
                std::string name = cfx::native<std::string>("GET_PLAYER_NAME", src.c_str());
                int ping = cfx::native<int>("GET_PLAYER_PING", src.c_str());
                return name + " (ping=" + std::to_string(ping) + "ms)";
        });

        cfx::setTimeout(5000, []()
        {
                std::println("[cfx::setTimeout] 5 seconds since start.");
        });

        auto minutes = std::make_shared<int>(0);
        cfx::setInterval(60000, [minutes]()
        {
                std::println("[cfx::setInterval] Up for {} minute(s).", ++*minutes);
        });

        cfx::createThread([]() -> cfx::ScriptTask
        {
                cfx::WorkerResult result = co_await cfx::runWorker("countPrimes", "100000");
                if (result.isDone())
                        std::println("[cfx::runWorker] {} primes below 100000.", result.output);
                else
                        std::println("[cfx::runWorker] Worker failed ({}).", result.status);
        });

        cfx::onCommand("c++", [](int source, const std::vector<std::string>&)
        {
                if (source == 0)
                        return;
                std::string name = cfx::getCurrentResourceName();
                cfx::json::Value info = cfx::callExport(name, "getPlayerInfo", { source });
                std::println("[cfx::addExport] {}", info.asStr("failed"));
                cfx::callExport("fw", "SendChatMessage", { source, "^#f0a0e4[INFO] ^#ffffffHello from ^#f0a0e4C++^#ffffff!" });
        }, false);
}
