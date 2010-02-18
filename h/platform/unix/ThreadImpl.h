//
// ThreadImpl.h
//
// PThreads implementation of a cross-platform thread
//
// Copyright (c) 2010, Greg Satz <satz@iranger.com>
// All rights reserved.
//
//	This file is part of OpenZWave.
//
//	OpenZWave is free software: you can redistribute it and/or modify
//	it under the terms of the GNU General Public License as published
//	by the Free Software Foundation, either version 3 of the License,
//	or (at your option) any later version.
//
//	OpenZWave is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//	GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License
//	along with OpenZWave.  If not, see <http://www.gnu.org/licenses/>.
//

#ifndef _ThreadImpl_H
#define _ThreadImpl_H

#include <pthread.h>

namespace OpenZWave
{
  class ThreadImpl
  {
  private:
    friend class Thread;

    ThreadImpl();
    ~ThreadImpl();

    bool Start( Thread::pfnThreadProc_t, void * );
    bool Stop();
    bool IsRunning() const { return m_bIsRunning; }

    void Run();
    static void *ThreadProc (void *parg);

    pthread_t m_hThread;
    Thread::pfnThreadProc_t	m_pfnThreadProc;
    void *m_pContext;
    bool m_bIsRunning;
  };
} // namespace OpenZWave

#endif //_ThreadImpl_H