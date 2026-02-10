#include "pthread_lock.h"

PthreadLock::PthreadLock()
{
   pthread_mutex_init(&_mutx, NULL);
}

PthreadLock::~PthreadLock()
{
   pthread_mutex_destroy(&_mutx);
}

void PthreadLock::acquire()
{
   pthread_mutex_lock(&_mutx);
}

void PthreadLock::release()
{
   pthread_mutex_unlock(&_mutx);
}

PthreadRWLock::PthreadRWLock()
{
   pthread_rwlock_init(&_rwlock, NULL);
}

PthreadRWLock::~PthreadRWLock()
{
   pthread_rwlock_destroy(&_rwlock);
}

void PthreadRWLock::acquire()
{
   pthread_rwlock_wrlock(&_rwlock);
}

void PthreadRWLock::release()
{
   pthread_rwlock_unlock(&_rwlock);
}

void PthreadRWLock::acquire_read()
{
   pthread_rwlock_rdlock(&_rwlock);
}

void PthreadRWLock::release_read()
{
   pthread_rwlock_unlock(&_rwlock);
}

__attribute__((weak)) LockImplementation* LockCreator_Default::create()
{
    return new PthreadLock();
}

__attribute__((weak)) LockImplementation* LockCreator_RwLock::create()
{
    return new PthreadRWLock();
}

__attribute__((weak)) LockImplementation* LockCreator_Spinlock::create()
{
    return new PthreadLock();
}
