---
sidebar_position: 2
title: "Contributors Guidelines"
---

# Contributors Guidelines

## 10.1 Introduction

Welcome to the ChronoLog project! This section is designed to help both new and existing contributors understand our development processes and collaboration practices. Whether you're an experienced developer or new to Git, you'll find essential information here to get started and contribute effectively.

## 10.2 Git Workflow and Development Process

Our project utilizes Git for version control and collaborative development. We assume that our contributors are familiar with Git. If you need a refresher or are new to Git, we recommend starting with the Atlassian Bitbucket tutorial.

## 10.3 Git Workflow Strategy

### 10.3.1 Branching and Releases

We employ a modified GitFlow workflow. Key aspects include:

- **Main Branch:** Houses stable releases, with each major version ensuring API backward compatibility. No active development occurs on this branch.
- **Develop Branch:** Serves as the integration branch for ongoing development, always ahead of the most recent stable release and must always build successfully with passing unit tests.
- **Feature and Bug Fix Branches:** Based on the Develop branch for ongoing development and bug fixes. Each is merged back after completion and reviewed through pull requests.
- **Release Branches:** Managed uniquely by promoting the latest stable release branch to the top of the GitHub branch list instead of merging it back to the main branch. This approach supports applying bug fixes across multiple active releases.

For more detailed practices and branching strategies, visit [Comparing Workflows](https://www.atlassian.com/git/tutorials/comparing-workflows).

### 10.3.2 Common Sense Approach with Git

- Use a minimal set of Git commands consistently to avoid complexity.
- Utilize branches to organize thoughts, merging them frequently and pruning them regularly to maintain a clean repository.

## 10.4 GitHub Usage

### 10.4.1 Forking and Pull Requests

For detailed instructions on forking the repository and managing your branches, please refer to [Creating a Pull Request from a Fork](https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/proposing-changes-to-your-work-with-pull-requests/creating-a-pull-request-from-a-fork). Always fork the repository to your account for development and debugging, creating pull requests to the main repository when ready.

### 10.4.2 Issue Tracking and Project Management

We use GitHub Issues to track bugs and feature requests and GitHub Projects to organize these into relevant projects:

- **Issue Management:** When creating branches for bug fixes or features, include the issue number in the branch name for easy reference.
- **Project Boards:** Used to visualize the progress of feature and release projects.

## 10.5 Code Review and Integration

### 10.5.1 Pull Request Process

All changes must be reviewed before being merged. Ensure that your pull requests are comprehensive, well-documented, and pass all integration tests.

### 10.5.2 Continuous Integration (CI)

We use GitHub Actions in combination with Spack to implement CI, which is triggered by merges into the Develop branch. This ensures that all changes are tested and meet our quality standards before integration.

## 10.6 Project Management Practices

### 10.6.1 Managing Issues

Keep the number of issues assigned to an individual developer manageable, ideally not exceeding two or three active issues at once. Update issue statuses regularly to reflect current progress.

### 10.6.2 Release Management

We support only the two most recent releases with bug fixes. Older versions are phased out unless critical maintenance is required.

## 10.7 Notes

Stay updated with any project-specific announcements, version updates, or changes in practices here.
