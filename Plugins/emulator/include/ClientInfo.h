#ifndef __CLIENTINFO_H_
#define __CLIENTINFO_H_

#include <string.h>

class ClientInfo
{
private:
    std::string name;

public:
    ClientInfo()
    {

    }

    ~ClientInfo()
    {
    }

    void setname(std::string &s)
    {
        name = s;
    }

    std::string &getname()
    {
        return name;
    }

};

#endif
