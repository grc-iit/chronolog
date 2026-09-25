---
sidebar_position: 0
title: "Overview"
---

# Guidelines Overview

The Guidelines section documents the conventions and processes that keep ChronoLog contributions consistent. Following these guidelines helps maintain a clean codebase, streamlines code review, and ensures that every pull request integrates smoothly with the existing project infrastructure.

## What's in this section

### Git Workflow

ChronoLog uses a modified GitFlow branching strategy built around a forking workflow. This page covers branch naming, issue tracking integration, and how feature branches flow from fork to upstream.

See [Git Workflow](./git-workflow.md) for the full branching strategy.

### Code Style

All C++ code is formatted with **clang-format-18** and checked automatically in CI. Beyond formatting, this page describes naming conventions, header organization, error handling patterns, and general coding practices.

See [Code Style](./code-style.md) for the complete style guide.

### CI/CD

GitHub Actions powers ChronoLog's continuous integration and deployment pipeline. This page explains the workflows that run on each push and pull request, what they check, and how to troubleshoot common failures.

See [CI/CD](./ci-cd.md) for workflow details and troubleshooting tips.

### Testing Guidelines

Tests are expected for all new functionality. This page covers the test categories (unit, integration, end-to-end), naming conventions, directory layout, and what reviewers look for when evaluating test coverage.

See [Testing Guidelines](./testing-guidelines.md) for expectations and examples.

### License

ChronoLog is released under the **BSD 2-Clause License**. This page includes the full license text and explains what it means for contributors.

See [License](./license.md) for the license details.

### Code of Conduct

ChronoLog follows the Contributor Covenant v2.0. This page summarizes the key expectations for community behavior and how to report violations.

See [Code of Conduct](./code-of-conduct.md) for the full details.
