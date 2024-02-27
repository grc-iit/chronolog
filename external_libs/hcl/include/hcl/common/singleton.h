/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Distributed under BSD 3-Clause license.                                   *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Illinois Institute of Technology.                        *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of Hermes. The full Hermes copyright notice, including  *
 * terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the top directory. If you do not  *
 * have access to the file, you may request a copy from help@hdfgroup.org.   *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*-------------------------------------------------------------------------
 *
 * Created: singleton.h
 * June 5 2018
 * Hariharan Devarajan <hdevarajan@hdfgroup.org>
 *
 * Purpose:Define singleton template for making a Singleton instances of
 * certain classes in HCL.
 *
 *-------------------------------------------------------------------------
 */

#ifndef INCLUDE_HCL_COMMON_SINGLETON_H_
#define INCLUDE_HCL_COMMON_SINGLETON_H_
#include <iostream>
#include <memory>
#include <utility>
/**
 * Make a class singleton when used with the class. format for class name T
 * Singleton<T>::GetInstance()
 * @tparam T
 */
namespace hcl {
template <typename T>
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
  template <typename... Args>
  static std::shared_ptr<T> GetInstance(Args... args) {
    if (instance == nullptr)
      instance = std::shared_ptr<T>(new T(std::forward<Args>(args)...));
    return instance;
  }

  /**
   * Operators
   */
  Singleton& operator=(const Singleton) = delete; /* deleting = operatos*/
                                                  /**
                                                   * Constructor
                                                   */
 public:
  Singleton(const Singleton&) = delete; /* deleting copy constructor. */

 protected:
  static std::shared_ptr<T> instance;
  Singleton() {} /* hidden default constructor. */
};

template <typename T>
std::shared_ptr<T> Singleton<T>::instance = nullptr;
}  // namespace hcl
#endif  // INCLUDE_HCL_COMMON_SINGLETON_H_
