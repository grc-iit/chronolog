---
sidebar_position: 3
title: "Installation"
---

# Installation

## 2.1 Post-Build Installation

![High-level components](/img/HighLevelComponents.jpg)

### 2.1.1 Install Commands

From the build directory, execute:

```bash
make install
```

to install executables and dependencies into the default install directory (**`~/chronolog`**).

## 2.2 Customizing Installation Path

The installation copies and renames `default_conf.json.in` to `conf/default_conf.json` in the installation directory by default. You can pass `-DCMAKE_INSTALL_PREFIX=/new/installation/directory` to CMake to change it.
