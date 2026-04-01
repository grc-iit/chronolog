FROM ghcr.io/grc-iit/chronolog:latest
ENV DEBIAN_FRONTEND="noninteractive"

# SSH server and configuration are already set up in the base image.
# Just expose port 22 and start sshd as grc-iit (via sudo).

EXPOSE 22
CMD ["/usr/bin/sudo", "/usr/sbin/sshd", "-D"]
