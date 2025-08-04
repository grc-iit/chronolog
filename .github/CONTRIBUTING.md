
# Git Workflow and Development Process



We assume that our contributors are familiar with Git concepts and Git commands. If anyone needs a good reference we recommend Atlassian Bitbucket tutorial (see the referencelinks provided below).
This document outlines some basic structure we use in our development flow and collaboration so that we can develop the code without stepping on each other’s toes, avoid messy merges, and identify conflicts, bugs, and potential issues as early as possible in the development process.

References:
https://www.atlassian.com/git/tutorials/learn-git-with-bitbucket-cloud

## Common sense approach with Git:

* Don't get carried away with the myriad of features that Git has to offer.  Use the minimal combination of commands that satisfies the need and use them consistently in the same order. 
* Branches in Git are lightweight and efficient; use them to organize your thoughts, merge them often, prune them regularly

## Branching and Releases 
There are many branching practices, a few widely used practices are summarized here:
https://www.atlassian.com/git/tutorials/comparing-workflows

We use a workflow based on GitFlow but with a few modifications.  

* Our main customer-facing gitHub branch houses the code that is our most recent stable release deemed suitable for public consumption. It’s well tested and labeled with the triple number release tag : “major_version.minor_version.bug_fix”. The convention is that versions with the same major number provide the guarantee of syntactic and logical API backward compatibility.  No active development is happening on this branch. 
* Our release branches are not merged into the main branch as traditional GitFlow would dictate but they remain unmerged instead, the latest stable release branch is just promoted to the top of the gitHub branch list and becomes the default customer facing branch. We prefer this method because it allows you to apply bug fixes across multiple releases. When the codebase matures and there are customers actively using it, there are usually 2 -3 release versions that are in use and have to be supported. When there’s a need to apply a hot bug fix to the release that is actively in use but is not the most recent one you can not do this if it has been already merged into the main.
* We support & provide bug fixes to only the two most recent releases. We only have one release candidate at the time.
* Develop branch is our common integration branch for ongoing development. It is based on the main branch or the most recent stable release branch if we choose not to bother with the main branch (see item (2)). The code on this branch is ahead of the most recent stable release. It must build and pass unit tests at any time. This branch  will be used to run our continuous integration build. 
* All new feature branches are based on the Develop branch and are merged into the Develop branch upon feature completion for integration testing.  Active development is happening on the feature branches that can be used by individual developers or shared depending on the scope of work.  In case of a large new feature project feature branch becomes a feature integration branch in its own right while the individual contributors create their own child branches based on this feature branch for the logical chunks of work involved. As the Develop branch moves forward the feature branch may need to be rebased several times within its lifespan before it is merged back to Develop.
* When the collection of features intended for the next release  accumulates on the Develop branch we spawn a new release candidate branch and tag it with the new release tag incrementing the major or the minor number depending on what level of guarantee for API compatibility with the previous release it provides. Any bug fixes that happen at this point on the new release branch would also have to be backported to the Develop branch as well so that they are included in our subsequent development. When we complete the sufficient amount of testing and deem the release candidate branch stable  we either merge it to the main branch or we just promote it to the status of our default customer facing gitHub branch( see item (2))
* Bug fix branch is  based on the Develop or release branch where the bug manifested itself and merged back to that same branch. After the bug is fixed on the branch it was initially found it might also need to be backported to the Develop branch and/or the current stable release branch and/or the release candidate branch
* Merging a feature branch or a bug fix branch into the parent branch (develop or release branch)  is subject to peer code review and is done through the git pull request mechanism. After the feature branch is merged into the parent branch no work should be done on this same feature branch.
* To avoid unnecessary visibility of all the develop/feature/debug branches that are only meaningful to the developers, the developers should fork the shared repository to their own accounts. New branches for feature development and debugging will be created on the developers’ own repository. When everything is ready, the developer can create a PR from the own forked repository. Detailed instructions can be found here: https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/proposing-changes-to-your-work-with-pull-requests/creating-a-pull-request-from-a-fork. Issues and tasks still stay in the shared repository. In the issue page, the developer can link to the branch from the forked repository by selecting the forked repository instead of the shared one in the first step of the drop-down menu. A similar step-by-step instruction can be found here: https://docs.github.com/en/issues/tracking-your-work-with-issues/creating-a-branch-for-an-issue. Make sure the forked repository is selected in step 6.


## Issue Tracking and Project Management
* We will be tracking the progress of the ChronoLog project using gitHib Issues and Projects mechanisms
* We are going to use gitHub Issues to track reported issues and proposed new features
* We will use gitHub Projects to group Issues into Feature Projects and Release Projects 
* It is a good practice to include the issue number in the name of the feature or bug fix branch when the corresponding code changes are made. This way when the changes are merged into the parent branches any reviewer could use the Issue and the Project information to understand what is being merged and the reasoning behind the design and implementation decisions.
* Any Issue created should go into To Do/Backlog Queue for prioritization and assignment
* We should keep the number of Issues assigned to an individual Developer in single digits, few people can realistically work on more than 2-3 issues in parallel.
* The developer assigned the Issue is responsible for keeping the status of the issue (investigation/development/testing/review/etc) and also responsible for creating the corresponding bugfix/feature branch, keeping up to date with the parent branch that is usually  a moving target, testing the code changes and merging them into the parent branch in a timely manner. 

## Continuous Integration 
* GitHub Actions mechanism in combination with Spack will be used to implement Continuous Integration (CI)  based on the Develop branch.  
* The merge into the Develop branch event will be used to trigger the Build/Test action



## Notes:

