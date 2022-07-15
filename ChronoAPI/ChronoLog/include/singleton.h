//
// Created by kfeng on 3/30/22.
//

#ifndef CHRONOLOG_SINGLETON_H
#define CHRONOLOG_SINGLETON_H

#include <iostream>
#include <memory>
#include <utility>
/**
 * Make a class singleton when used with the class. format for class name T
 * Singleton<T>::GetInstance()
 * @tparam T
 */
namespace ChronoLog {
    template<typename T>
    class Singleton {
    public:
        /**
         * Members of Singleton Class
         */
        /**
         * Uses unique pointer to build a static global instance of variable.
         * @tparam T
         * @return instance of T
         */
        template<typename... Args>
        static std::shared_ptr<T> GetInstance(Args... args) {
            if (instance == nullptr)
                instance = std::shared_ptr<T>(new T(std::forward<Args>(args)...));
            return instance;
        }

        /**
         * Operators
         */
        Singleton &operator=(const Singleton) = delete; /* deleting = operatos*/
        /**
         * Constructor
         */
    public:
        Singleton(const Singleton &) = delete; /* deleting copy constructor. */

    protected:
        static std::shared_ptr<T> instance;

        Singleton() {} /* hidden default constructor. */
    };

    template<typename T>
    std::shared_ptr<T> Singleton<T>::instance = nullptr;
}

#endif //CHRONOLOG_SINGLETON_H
