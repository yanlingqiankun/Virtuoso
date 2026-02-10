#ifndef PTHREAD_LOCK_H
#define PTHREAD_LOCK_H

#include "lock.h"

#include <pthread.h>

class PthreadLock : public LockImplementation
{
public:
   PthreadLock();
   ~PthreadLock();

   void acquire();
   void release();

private:
   pthread_mutex_t _mutx;
};

class PthreadRWLock : public LockImplementation
{
public:
   PthreadRWLock();
   ~PthreadRWLock();

   void acquire();       // Write lock
   void release();       // Write unlock
   void acquire_read();  // Read lock
   void release_read();  // Read unlock

private:
   pthread_rwlock_t _rwlock;
};

#endif // PTHREAD_LOCK_H
