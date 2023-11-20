#include<iostream>

#ifndef EVENT_H
#define EVENT_H
#define DATA_SIZE 100

typedef struct Event
{
    __uint64_t timeStamp;
    char data[DATA_SIZE];
} Event;

#endif // EVENT_H