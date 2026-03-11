---
sidebar_position: 5
title: "Using Configuration Files"
---

# Using Configuration Files

Once the configuration file has been prepared and customized, pass it to ChronoLog executables using the `--config` flag: `--config default_conf.json`

To handle configurations programmatically, ChronoLog provides the ConfigurationManager class, which reads and validates configuration files for use with ChronoLog.

### ConfigurationManager Class

The ConfigurationManager class is used to parse and load configuration files into ChronoLog.

```cpp
ConfigurationManager::ConfigurationManager(const std::string &conf_file_path)
```

- The constructor reads configurations from the given file path and sets up ChronoLog accordingly.
- Exits with a fatal error if the configuration file is invalid.
- Ensures that key settings such as RPC and logging are correctly initialized.
