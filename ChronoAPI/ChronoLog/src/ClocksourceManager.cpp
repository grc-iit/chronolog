//
// Created by kfeng on 2/8/22.
//

#include "ClocksourceManager.h"

Clocksource *Clocksource::Create(ClocksourceType type) {
    switch (type) {
        case ClocksourceType::C_STYLE:
            return new ClocksourceCStyle();
        case ClocksourceType::CPP_STYLE:
            return new ClocksourceCPPStyle();
        case ClocksourceType::TSC:
            return new ClocksourceTSC();
        default:
            return nullptr;
    }
}