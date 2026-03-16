---
sidebar_position: 1
title: "Git Workflow"
---

# Git Workflow

ChronoLog uses Git for version control and collaborative development. This page describes the branching strategy, forking workflow, and issue tracking conventions that contributors should follow. If you need a refresher on Git, the [Atlassian Git tutorials](https://www.atlassian.com/git/tutorials) are a good starting point.

## Branching Strategy

ChronoLog employs a modified GitFlow workflow. Key aspects include:

- **Main Branch:** Houses stable releases, with each major version ensuring API backward compatibility. No active development occurs on this branch.
- **Develop Branch:** Serves as the integration branch for ongoing development, always ahead of the most recent stable release and must always build successfully with passing unit tests.
- **Feature and Bug Fix Branches:** Based on the Develop branch for ongoing development and bug fixes. Each is merged back after completion and reviewed through pull requests.
- **Release Branches:** Managed uniquely by promoting the latest stable release branch to the top of the GitHub branch list instead of merging it back to the main branch. This approach supports applying bug fixes across multiple active releases.

For more detailed practices and branching strategies, visit [Comparing Workflows](https://www.atlassian.com/git/tutorials/comparing-workflows).

### Best practices

- Use a minimal set of Git commands consistently to avoid complexity.
- Merge branches frequently and prune them regularly to keep the repository clean.
- Write clear, descriptive commit messages that reference the relevant issue number.

## Forking Workflow

Always fork the repository to your own GitHub account for development and debugging. When your work is ready, open a pull request from your fork back to the upstream repository. For step-by-step instructions, see GitHub's guide on [Creating a Pull Request from a Fork](https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/proposing-changes-to-your-work-with-pull-requests/creating-a-pull-request-from-a-fork).

## Issue Tracking

We use GitHub Issues to track bugs and feature requests and GitHub Projects to organize them:

- **Branch naming:** When creating branches for bug fixes or features, include the issue number in the branch name (e.g., `42-fix-connection-timeout`) for easy reference.
- **Project boards:** Used to visualize the progress of feature and release projects.
- **Issue limits:** Keep no more than two or three issues actively assigned to yourself at once. Update issue statuses regularly to reflect current progress.

## Release Management

We support only the two most recent releases with bug fixes. Older versions are phased out unless critical maintenance is required. Release branches are managed by promoting the latest stable release branch to the top of the GitHub branch list rather than merging it back to `main`.

## Related pages

- [CI/CD](./ci-cd.md) — GitHub Actions workflows triggered by pushes and merges
