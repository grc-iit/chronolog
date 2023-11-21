//
// Created by jaime on 2/10/2022.
//

#ifndef CHRONOLOG__SERVECTOR_H_
#define CHRONOLOG__SERVECTOR_H_

#include <vector>
#include "TimeRecord.h"
#include <cereal/types/vector.hpp>
#include <cereal/archives/binary.hpp>

//TODO: Extend to a template<class T>
class SerVector
{
private:
    std::vector <TimeRecord> vec;
public:
    std::vector <TimeRecord> get()
    {
        return vec;
    }

    /**************************************
     Constructors
    ***************************************/

    /**************************************
     Element Access
    ***************************************/
    //Returning by reference allows for using as lvalue on assignments
    TimeRecord &at(size_t i)
    {
        return vec.at(i);
    }

    TimeRecord &front()
    {
        return vec.front();
    }

    TimeRecord &back()
    {
        return vec.back();
    }

    TimeRecord*data()
    {
        return vec.data();
    }

    auto &operator[](std::size_t idx)
    { return vec[idx]; }

    /**************************************
     Iterators. Missing constant iterators
    ***************************************/
    std::vector <TimeRecord>::iterator begin()
    {
        return vec.begin();
    }

    std::vector <TimeRecord>::reverse_iterator rbegin()
    {
        return vec.rbegin();
    }

    std::vector <TimeRecord>::iterator end()
    {
        return vec.end();
    }

    std::vector <TimeRecord>::reverse_iterator rend()
    {
        return vec.rend();
    }

    /**************************************
     Capacity operators. No shrink_to_fit
    ***************************************/
    bool empty()
    {
        return vec.empty();
    }

    size_t size()
    {
        return vec.size();
    }

    size_t max_size()
    {
        return vec.max_size();
    }

    void reserve(size_t new_size)
    {
        vec.reserve(new_size);
    }

    size_t capacity()
    {
        return vec.capacity();
    }

    /**************************************
     Modifiers
    **************************************/
    void clear()
    {
        vec.clear();
    }

    void insert()
    {
    }

    void emplace()
    {

    }

    void erase()
    {

    }

    void push_back()
    {

    }

    void emplace_back()
    {

    }

    void pop_back()
    {

    }

    void resize(size_t new_size)
    {
        vec.resize(new_size);
    }

    void swap()
    {

    }

    /**************************************
     Modifiers
    **************************************/
    template <class Archive>
    void serialize(Archive &archive)
    {
        archive(vec); // serialize things by passing them to the archive
    }

    void load()
    {

    }
};

#endif //CHRONOLOG__SERVECTOR_H_
