#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>

#include <pwd.h>
#include <sys/types.h>
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

// getpwuid_r for a user "bob" whose entry needs a 64 KiB buffer, more than
// sysconf(_SC_GETPW_R_SIZE_MAX) suggests: like glibc, it reports ERANGE for
// any smaller buffer.
int large_entry_bob(uid_t, struct passwd* entry, char* buffer, std::size_t size, struct passwd** found)
{
    *found = nullptr;
    if(size < 65536)
    {
        return ERANGE;
    }
    std::strcpy(buffer, "bob");
    entry->pw_name = buffer;
    *found = entry;
    return 0;
}

// getpwuid_r for a uid the password database has no entry for, such as a
// container user that is not in /etc/passwd.
int no_entry(uid_t, struct passwd*, char*, std::size_t, struct passwd** found)
{
    *found = nullptr;
    return 0;
}

std::string effective_user_name()
{
    struct passwd const* entry = getpwuid(geteuid());
    return entry != nullptr && entry->pw_name != nullptr ? std::string(entry->pw_name) : std::to_string(geteuid());
}

} // namespace

TEST(ClientLoginName, TheTerminalsLoginNameIsUsed) { EXPECT_EQ(chronolog::client_login_name(login_alice), "alice"); }

TEST(ClientLoginName, WithoutATerminalTheEffectiveUsersNameIsUsed)
{
    std::string const name = chronolog::client_login_name(no_terminal);
    EXPECT_FALSE(name.empty());
    EXPECT_EQ(name, effective_user_name());
}

TEST(ClientLoginName, AnEntryLargerThanTheSuggestedBufferIsStillFound)
{
    EXPECT_EQ(chronolog::client_login_name(no_terminal, large_entry_bob), "bob");
}

TEST(ClientLoginName, WithoutAPasswordEntryTheEffectiveUidIsUsed)
{
    EXPECT_EQ(chronolog::client_login_name(no_terminal, no_entry), std::to_string(geteuid()));
}
