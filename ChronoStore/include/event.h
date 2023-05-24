#include<iostream>
#ifndef EVENT_H
#define EVENT_H


typedef struct Event {
    __uint64_t timeStamp;
    char data[100];
} Event;

#endif // EVENT_H