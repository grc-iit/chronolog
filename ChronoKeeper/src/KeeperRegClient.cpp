#include <iostream>
#include <cstdlib>
#include <thallium.hpp>

int main(int argc, char**argv)
{
    if(argc != 3)
    {
        std::cerr << "Usage: " << argv[0] << " <address> <provider_id>" << std::endl;
        exit(0);
    }
    tl::engine myEngine("ofi+sockets", THALLIUM_CLIENT_MODE);
}