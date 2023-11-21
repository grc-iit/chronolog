//
// Created by kfeng on 2/8/22.
//

#include "ClocksourceManager.h"

ClocksourceManager*ClocksourceManager::clocksourceManager_ = nullptr;

Clocksource*Clocksource::Create(ClocksourceType type)
{
    switch(type)
    {
        case ClocksourceType::C_STYLE:
            return new ClocksourceCStyle();
        case ClocksourceType::CPP_STYLE:
            return new ClocksourceCPPStyle();
#ifdef TSC_ENABLED
            case ClocksourceType::TSC:
                    return new ClocksourceTSC();
#endif
        default:
            return nullptr;
    }
}
