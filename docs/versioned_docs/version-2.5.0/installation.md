---
sidebar_position: 3
title: "Installation"
---

# Installation

## Post-Build Installation

![High-level components](/img/HighLevelComponents.jpg)

### Install Commands

From the build directory, execute:

```bash
make install
```

to install executables and dependencies into the default install directory (**`~/chronolog`**).

## Customizing Installation Path

The installation copies and renames `default_conf.json.in` to `conf/default_conf.json` in the installation directory by default. You can pass `-DCMAKE_INSTALL_PREFIX=/new/installation/directory` to CMake to change it.
