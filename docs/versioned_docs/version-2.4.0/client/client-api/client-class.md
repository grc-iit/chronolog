---
sidebar_position: 1
title: "Client Class"
---

# Client Class

The class used to manage the metadata interaction with ChronoLog through the ChronoLog Client Library.

```cpp
Client::Client(ChronoLog::ConfigurationManager const &confManager)
```

- The constructor requires an object of ConfigurationManager to initialize the client object.

```cpp
int Client::Connect()
```

- Connect to a ChronoLog deployment
- Return `CL_SUCCESS` on success, error code otherwise

```cpp
int Client::Disconnect()
```

- Disconnect from a ChronoLog deployment
- Return `CL_SUCCESS` on success, error code otherwise

```cpp
int Client::CreateChronicle(std::string const &chronicle_name, std::map <std::string, std::string> const &attrs, int &flags)
```

- Create a Chronicle named `chronicle_name`
- Return `CL_SUCCESS` on success, error code otherwise
- `attrs` is used to fine tune the property of the Chronicle such as the tiering policy
- `flags` is used to enable or disable configuration flags during the creation
- If there is no existing Chronicle with the same name, Chronicle `chronicle_name` will be created. Otherwise, an error code `CL_ERR_CHRONICLE_EXISTS` will be returned

```cpp
int Client::DestroyChronicle(std::string const &chronicle_name)
```

- Destroy the Chronicle named `chronicle_name`
- Return `CL_SUCCESS` on success, error code otherwise
- If the Chronicle has no Story being acquired, it will be destroyed. Otherwise, an error code `CL_ERR_ACQUIRED` will be returned

```cpp
std::pair <int, StoryHandle*> Client::AcquireStory(std::string const &chronicle_name, std::string const &story_name, const std::map <std::string, std::string> &attrs, int &flags)
```

- Acquire the Story with the name story_name under the Chronicle named chronicle_name to get ready for data access
- A `pair<int, StoryHandle *>` will be returned on success. The first variable is the return value which should be `CL_SUCCESS` on success. The second one is a pointer to a StoryHandle object which manages access to the acquired Story.
- If the Chronicle or the Story does not exist, an error code `CL_ERR_NOT_EXIST` will be returned

```cpp
int Client::ReleaseStory(std::string const &chronicle_name, std::string const &story_name)
```

- Release the Story with the name `story_name` under the Chronicle named `chronicle_name`
- Return `CL_SUCCESS` on success, error code otherwise

```cpp
int Client::DestroyStory(std::string const &chronicle_name, std::string const &story_name)
```

- Destroy the Story with the name of `story_name` under the Chronicle named `chronicle_name`
- Return `CL_SUCCESS` on success, error code otherwise
- If the Story is being acquired, an error code `CL_ERR_ACQUIRED` will be returned
