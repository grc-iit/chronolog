#ifndef KEEPER_REGISTRY_SERVICE_H
#define KEEPER_REGISTRY_SERVICE_H

#include <iostream>
//#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include "KeeperIdCard.h"
#include "KeeperRegistrationMsg.h"
#include "KeeperStatsMsg.h"
#include "log.h"
#include "KeeperRegistry.h"

namespace tl = thallium;

namespace chronolog
{

class KeeperRegistryService: public tl::provider <KeeperRegistryService>
{

private:
    KeeperRegistry &theKeeperProcessRegistry;

    void register_keeper(tl::request const &request, chronolog::KeeperRegistrationMsg const &keeperMsg)
    {
        int return_code = 0;
        std::stringstream ss, s1;
        ss << keeperMsg.getKeeperIdCard();
        s1 << keeperMsg.getAdminServiceId();
        Logger::getLogger()->info("[KeeperRegistryService] New Keeper Registered: KeeperIdCard: {}, AdminServiceId: {}"
                                  , ss.str(), s1.str());
        return_code = theKeeperProcessRegistry.registerKeeperProcess(keeperMsg);
        request.respond(return_code);
    }

    void unregister_keeper(tl::request const &request, chronolog::KeeperIdCard const &keeper_id_card)
    {
        int return_code = 0;
        std::stringstream ss;
        ss << keeper_id_card;
        Logger::getLogger()->info("[KeeperRegistryService] Keeper Unregistered: KeeperIdCard: {}", ss.str());
        return_code = theKeeperProcessRegistry.unregisterKeeperProcess(keeper_id_card);
        request.respond(return_code);
    }

    void handle_stats_msg(chronolog::KeeperStatsMsg const &keeper_stats_msg)
    {
        std::stringstream ss;
        ss << keeper_stats_msg.getKeeperIdCard();
        Logger::getLogger()->debug("[KeeperRegistryService] Keeper Stats: KeeperIdCard: {}, ActiveStoryCount: {}"
                                   , ss.str(), keeper_stats_msg.getActiveStoryCount());
        theKeeperProcessRegistry.updateKeeperProcessStats(keeper_stats_msg);
    }

    KeeperRegistryService(tl::engine &tl_engine, uint16_t service_provider_id, KeeperRegistry &keeperRegistry)
            : tl::provider <KeeperRegistryService>(tl_engine, service_provider_id), theKeeperProcessRegistry(
            keeperRegistry)
    {
        define("register_keeper", &KeeperRegistryService::register_keeper);
        define("unregister_keeper", &KeeperRegistryService::unregister_keeper);
        define("handle_stats_msg", &KeeperRegistryService::handle_stats_msg, tl::ignore_return_value());
        //setup finalization callback in case this ser vice provider is still alive when the engine is finalized
        get_engine().push_finalize_callback(this, [p = this]()
        { delete p; });
    }

    KeeperRegistryService(KeeperRegistryService const &) = delete;

    KeeperRegistryService &operator=(KeeperRegistryService const &) = delete;


public:

    static KeeperRegistryService*
    CreateKeeperRegistryService(tl::engine &tl_engine, uint16_t service_provider_id, KeeperRegistry &keeperRegistry)
    {
        return new KeeperRegistryService(tl_engine, service_provider_id, keeperRegistry);
    }

    ~KeeperRegistryService()
    {
        get_engine().pop_finalize_callback(this);
    }
};
}// namespace chronolog

#endif
