//
// Created by kfeng on 4/5/22.
//

#ifndef CHRONOLOG_LOG_H
#define CHRONOLOG_LOG_H

#pragma once

#ifdef __cplusplus
#include <cstdio>
#include <cstring>
#else
#include <stdio.h>
#include <string.h>
#endif

// Truncates the full __FILE__ path, only displaying the basename
#define __FILENAME__ \
    (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : strrchr(__FILE__, '\\') ? strrchr(__FILE__, '\\') + 1 : __FILE__)

#define LOG(fmt, ...)           do{ printf(fmt "\n", ##__VA_ARGS__); } while(0)
#define LOGT(tag, fmt, ...)     do{ printf(tag ": " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOGI(fmt, ...)          do{ printf("%s: " fmt "\n", __FILENAME__, ##__VA_ARGS__); } while(0)
#define LOGE(fmt, ...)          do{ printf("\033[31mERROR: %s: %s: %d: " fmt "\033[m\n", \
                                    __FILENAME__, __func__, __LINE__, ##__VA_ARGS__); \
                                } while(0)

//#define NODEBUG 1
#ifdef NODEBUG
#define LOGD(fmt, ...)          ((void)0)
#else
#define LOGD(fmt, ...)          do{ printf("\033[33m%s: " fmt "\033[m\n", __FILENAME__, ##__VA_ARGS__); } while(0)
#endif

#endif //CHRONOLOG_LOG_H
