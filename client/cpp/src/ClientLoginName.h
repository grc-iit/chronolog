#ifndef CHRONOLOG_CLIENT_LOGIN_NAME_H
#define CHRONOLOG_CLIENT_LOGIN_NAME_H

#include <cstddef>
#include <string>

namespace chronolog
{

// The signature of getlogin_r, which the tests replace.
using GetLoginFunction = int (*)(char* name, std::size_t size);

/**
 * The user name a client logs when it starts: the login name of the process's
 * controlling terminal when getlogin_r finds one, and otherwise the name of the
 * effective user, or an empty string when the password database has no entry
 * for it. getlogin_r fails for a client with no controlling terminal, such as
 * one started over ssh with a command, by mpiexec or by a batch scheduler, and
 * then leaves its buffer unwritten.
 */
std::string client_login_name();

// The same, with get_login in place of getlogin_r.
std::string client_login_name(GetLoginFunction get_login);

} // namespace chronolog

#endif // CHRONOLOG_CLIENT_LOGIN_NAME_H
