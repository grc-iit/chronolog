//
// Created by kfeng on 3/30/22.
//

#include <rpc.h>
#include "ChronicleMetadataRPCProxy.h"
#include "log.h"

int main() {
    CHRONOLOG_CONF->ConfigureDefaultServer("../../../test/communication/server_list");
    ChronicleMetadataRPCProxy rpcProxy;

    return 0;
}