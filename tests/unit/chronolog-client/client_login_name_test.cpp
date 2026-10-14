#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>

#include <pwd.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "ClientLoginName.h"

namespace
{

// getlogin_r with a controlling terminal whose login name is "alice".
int login_alice(char* name, std::size_t size)
{
    std::strncpy(name, "alice", size);
    return 0;
}

// getlogin_r without a controlling terminal: it fails and leaves the buffer as
// it was, here filled with bytes that are not a name and hold no '\0'.
int no_terminal(char* name, std::size_t size)
{
    std::memset(name, 'X', size);
    return ENXIO;
}

std::string effective_user_name()
{
    struct passwd const* entry = getpwuid(geteuid());
    return entry != nullptr && entry->pw_name != nullptr ? std::string(entry->pw_name) : std::string();
}

} // namespace

TEST(ClientLoginName, TheTerminalsLoginNameIsUsed) { EXPECT_EQ(chronolog::client_login_name(login_alice), "alice"); }

TEST(ClientLoginName, WithoutATerminalTheEffectiveUsersNameIsUsed)
{
    EXPECT_EQ(chronolog::client_login_name(no_terminal), effective_user_name());
}
