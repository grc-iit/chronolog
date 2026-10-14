#include <climits>
#include <string>
#include <vector>

#include <pwd.h>
#include <unistd.h>

#include "ClientLoginName.h"

std::string chronolog::client_login_name(GetLoginFunction get_login)
{
    // One byte more than getlogin_r is told about, so the name always ends in '\0'.
    char login_name[LOGIN_NAME_MAX + 1] = {0};
    if(get_login(login_name, LOGIN_NAME_MAX) == 0)
    {
        return login_name;
    }

    // No controlling terminal: getlogin_r wrote nothing into the buffer.
    long size = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(size > 0 ? static_cast<std::size_t>(size) : 16384);
    struct passwd entry;
    struct passwd* found = nullptr;
    if(getpwuid_r(geteuid(), &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr &&
       found->pw_name != nullptr)
    {
        return found->pw_name;
    }
    return std::string();
}

std::string chronolog::client_login_name() { return client_login_name(::getlogin_r); }
