#ifndef CHRONOLOG_CLIENT_LOGIN_NAME_H
#define CHRONOLOG_CLIENT_LOGIN_NAME_H

#include <cstddef>
#include <string>

#include <pwd.h>
#include <sys/types.h>

namespace chronolog
{

// The signatures of getlogin_r and getpwuid_r, which the tests replace.
using GetLoginFunction = int (*)(char* name, std::size_t size);
using GetPasswdFunction =
        int (*)(uid_t uid, struct passwd* entry, char* buffer, std::size_t size, struct passwd** found);

/**
 * The user name a client logs when it starts: the login name getlogin_r finds,
 * and otherwise the name of the effective user, or the effective uid as a
 * number when the password database has no entry for it. glibc's getlogin_r
 * reads /proc/self/loginuid, which pam_loginuid sets for ssh and console
 * logins, and falls back to the utmp entry of the terminal on standard input.
 * It fails when neither is there, as for a process started by docker exec or
 * in a container whose sshd cannot set loginuid, and then leaves its buffer
 * unwritten.
 */
std::string client_login_name();

// The same, with get_login in place of getlogin_r and get_passwd in place of
// getpwuid_r.
std::string client_login_name(GetLoginFunction get_login, GetPasswdFunction get_passwd = ::getpwuid_r);

} // namespace chronolog

#endif // CHRONOLOG_CLIENT_LOGIN_NAME_H
