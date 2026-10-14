#include <cerrno>
#include <climits>
#include <string>
#include <vector>

#include <pwd.h>
#include <unistd.h>

#include "ClientLoginName.h"

std::string chronolog::client_login_name(GetLoginFunction get_login, GetPasswdFunction get_passwd)
{
    // One byte more than getlogin_r is told about, so the name always ends in '\0'.
    char login_name[LOGIN_NAME_MAX + 1] = {0};
    if(get_login(login_name, LOGIN_NAME_MAX) == 0)
    {
        return login_name;
    }

    // No login name: getlogin_r wrote nothing into the buffer. The size sysconf
    // suggests is only a hint; an entry from LDAP or SSSD can need more, and
    // getpwuid_r then fails with ERANGE, so retry with a larger buffer.
    uid_t const uid = geteuid();
    long const suggested = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::size_t size = suggested > 0 ? static_cast<std::size_t>(suggested) : 1024;
    std::size_t const max_size = 1 << 20;
    std::vector<char> buffer;
    struct passwd entry;
    struct passwd* found = nullptr;
    int error;
    while(true)
    {
        buffer.resize(size);
        error = get_passwd(uid, &entry, buffer.data(), buffer.size(), &found);
        if(error != ERANGE || size >= max_size)
        {
            break;
        }
        size *= 2;
    }
    if(error == 0 && found != nullptr && found->pw_name != nullptr)
    {
        return found->pw_name;
    }
    return std::to_string(uid);
}

std::string chronolog::client_login_name() { return client_login_name(::getlogin_r); }
