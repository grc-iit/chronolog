//
// Created by kfeng on 12/7/21.
//

#include <SerDe.h>

SerDe*SerDe::Create(SerDeType type)
{
    if(type == SerDeType::CEREAL)
        return new SerDeCereal();
    else return nullptr;
}
