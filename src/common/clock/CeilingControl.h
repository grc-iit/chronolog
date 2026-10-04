#pragma once
#include "chronolog/types.h"
namespace chronolog
{
class CeilingControl
{
public:
    virtual ~CeilingControl() = default;
    virtual void setCeiling(Hlc ceiling) = 0;
};
} // namespace chronolog
