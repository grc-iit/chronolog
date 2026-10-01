#include <chronolog/client/client.h>
#include <type_traits>
static_assert(!std::is_copy_constructible_v<chronolog::client::Client>);
static_assert(!std::is_copy_constructible_v<chronolog::client::Writer>);
static_assert(!std::is_copy_constructible_v<chronolog::client::ReadStream>);
static_assert(!std::is_copy_constructible_v<chronolog::client::TailStream>);
int main()
{
    chronolog::client::ChronoClock clock;
    if(!clock.now().ok())
        return 1;
    auto client = chronolog::client::Client::Connect({});
    return client.status().code() == absl::StatusCode::kInvalidArgument ? 0 : 1;
}
