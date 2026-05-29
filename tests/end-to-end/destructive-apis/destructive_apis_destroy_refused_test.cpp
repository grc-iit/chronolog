// End-to-end: Destroy refuses when another client holds the story (#574).
//
// Visor derives ClientId from PID, so two chronolog::Client instances in the
// same process collide on the server side. We fork() so the two "clients"
// see distinct ClientIds and the eligibility check in
// ChronicleMetaDirectory::evaluate_story_destroy actually sees a holder
// other than the requester.
//
// Coordination is simple: two pipes between parent and child for ready/go
// handshakes. Parent plays Client A (acquires the story), child plays
// Client B (attempts destroys).

#include "destructive_apis_common.h"

#include <cerrno>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

#include <client_errcode.h>

namespace
{
ssize_t write_full(int fd, void const* buf, size_t n) { return ::write(fd, buf, n); }

bool read_byte(int fd)
{
    char c;
    return ::read(fd, &c, 1) == 1;
}

bool send_byte(int fd, char c) { return write_full(fd, &c, 1) == 1; }
} // namespace

// Returns 0 on pass, non-zero on fail (printed via the helpers).
static int run_child_clientB(int from_parent_fd,
                             int to_parent_fd,
                             destructive_apis::Args const& args,
                             std::string const& chronicle,
                             std::string const& story)
{
    using namespace destructive_apis;

    // Writer-only ctor (no query service) so the child doesn't try to bind
    // the same query port the parent already owns.
    chronolog::Client clientB(load_portal_conf(args.clientConf));
    if(clientB.Connect() != chronolog::CL_SUCCESS)
        return fail("[child] Connect failed");

    // Wait for parent to signal it has acquired.
    if(!read_byte(from_parent_fd))
        return fail("[child] failed to read 'parent-acquired' byte");

    int rc = clientB.DestroyStory(chronicle, story);
    if(rc != chronolog::CL_ERR_ACQUIRED)
    {
        fail("[child] expected DestroyStory to return CL_ERR_ACQUIRED, got " + std::to_string(rc));
        send_byte(to_parent_fd, 'F');
        return 1;
    }
    rc = clientB.DestroyChronicle(chronicle);
    if(rc != chronolog::CL_ERR_ACQUIRED)
    {
        fail("[child] expected DestroyChronicle to return CL_ERR_ACQUIRED, got " + std::to_string(rc));
        send_byte(to_parent_fd, 'F');
        return 1;
    }

    // Tell parent: refusal verified, please release.
    send_byte(to_parent_fd, 'R');
    // Wait for parent to confirm release done.
    if(!read_byte(from_parent_fd))
        return fail("[child] failed to read 'parent-released' byte");

    if(clientB.DestroyStory(chronicle, story) != chronolog::CL_SUCCESS)
        return fail("[child] DestroyStory failed after parent released");
    if(clientB.DestroyChronicle(chronicle) != chronolog::CL_SUCCESS)
        std::cerr << "[child] cleanup DestroyChronicle failed (non-fatal)" << std::endl;

    clientB.Disconnect();
    return 0;
}

int main(int argc, char** argv)
{
    using namespace destructive_apis;

    Args args;
    if(!parse_args(argc, argv, args))
        return 2;

    std::string const suffix = unique_suffix();
    std::string const chronicle = "refuse_chr_" + suffix;
    std::string const story = "refuse_sty_" + suffix;

    int parent_to_child[2];
    int child_to_parent[2];
    if(pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0)
        return fail(std::string("pipe failed: ") + std::strerror(errno));

    pid_t pid = fork();
    if(pid < 0)
        return fail(std::string("fork failed: ") + std::strerror(errno));

    if(pid == 0)
    {
        // Child = Client B (would-be destroyer).
        ::close(parent_to_child[1]);
        ::close(child_to_parent[0]);
        int rc = run_child_clientB(parent_to_child[0], child_to_parent[1], args, chronicle, story);
        ::close(parent_to_child[0]);
        ::close(child_to_parent[1]);
        return rc;
    }

    // Parent = Client A (holder).
    ::close(parent_to_child[0]);
    ::close(child_to_parent[1]);

    chronolog::Client clientA(load_portal_conf(args.clientConf), load_query_conf(args.clientConf));
    if(clientA.Connect() != chronolog::CL_SUCCESS)
    {
        ::close(parent_to_child[1]);
        ::close(child_to_parent[0]);
        waitpid(pid, nullptr, 0);
        return fail("[parent] Connect failed");
    }
    if(clientA.CreateChronicle(chronicle) != chronolog::CL_SUCCESS)
    {
        ::close(parent_to_child[1]);
        ::close(child_to_parent[0]);
        waitpid(pid, nullptr, 0);
        return fail("[parent] CreateChronicle failed");
    }
    auto [rcA, handleA] = clientA.AcquireStory(chronicle, story);
    if(rcA != chronolog::CL_SUCCESS || handleA == nullptr)
    {
        ::close(parent_to_child[1]);
        ::close(child_to_parent[0]);
        waitpid(pid, nullptr, 0);
        return fail("[parent] AcquireStory failed");
    }

    // Signal child: I have acquired. Go test the refusals.
    send_byte(parent_to_child[1], 'A');

    // Wait for child to confirm refusals were correct.
    char ack = 0;
    if(::read(child_to_parent[0], &ack, 1) != 1 || ack != 'R')
    {
        ::close(parent_to_child[1]);
        ::close(child_to_parent[0]);
        waitpid(pid, nullptr, 0);
        return fail("[parent] child did not confirm refusals; ack='" + std::string(1, ack) + "'");
    }

    if(clientA.ReleaseStory(chronicle, story) != chronolog::CL_SUCCESS)
    {
        ::close(parent_to_child[1]);
        ::close(child_to_parent[0]);
        waitpid(pid, nullptr, 0);
        return fail("[parent] ReleaseStory failed");
    }

    // Signal child: released. Go destroy.
    send_byte(parent_to_child[1], 'R');

    int status = 0;
    waitpid(pid, &status, 0);
    ::close(parent_to_child[1]);
    ::close(child_to_parent[0]);

    clientA.Disconnect();

    if(!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("[parent] child exited with status " + std::to_string(status));

    return pass("Destroy correctly refused while another client held the story");
}
