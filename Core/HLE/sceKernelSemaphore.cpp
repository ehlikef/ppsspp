// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>
#include "HLE.h"
#include "../MIPS/MIPS.h"
#include "../../Core/CoreTiming.h"
#include "sceKernel.h"
#include "sceKernelThread.h"
#include "sceKernelSemaphore.h"

#define PSP_SEMA_ATTR_FIFO 0
#define PSP_SEMA_ATTR_PRIORITY 0x100

/** Current state of a semaphore.
* @see sceKernelReferSemaStatus.
*/

struct NativeSemaphore
{
	/** Size of the ::SceKernelSemaInfo structure. */
	SceSize 	size;
	/** NUL-terminated name of the semaphore. */
	char 		name[32];
	/** Attributes. */
	SceUInt 	attr;
	/** The initial count the semaphore was created with. */
	int 		initCount;
	/** The current count. */
	int 		currentCount;
	/** The maximum count. */
	int 		maxCount;
	/** The number of threads waiting on the semaphore. */
	int 		numWaitThreads;
};


struct Semaphore : public KernelObject 
{
	const char *GetName() {return ns.name;}
	const char *GetTypeName() {return "Semaphore";}

	static u32 GetMissingErrorCode() { return SCE_KERNEL_ERROR_UNKNOWN_SEMID; }
	int GetIDType() const { return SCE_KERNEL_TMID_Semaphore; }

	NativeSemaphore ns;
	std::vector<SceUID> waitingThreads;
	int waitTimer;
};

// Resume all waiting threads (for delete / cancel.)
// Returns true if it woke any threads.
bool __KernelClearSemaThreads(Semaphore *s, int reason)
{
	bool wokeThreads = false;

	// TODO: PSP_SEMA_ATTR_PRIORITY
	std::vector<SceUID>::iterator iter;
	for (iter = s->waitingThreads.begin(); iter!=s->waitingThreads.end(); iter++)
	{
		SceUID threadID = *iter;

		u32 error;
		u32 timeoutPtr = __KernelGetWaitTimeoutPtr(threadID, error);
		if (timeoutPtr != 0 && s->waitTimer != 0)
		{
			// Remove any event for this thread.
			int cyclesLeft = CoreTiming::UnscheduleEvent(s->waitTimer, threadID);
			Memory::Write_U32(cyclesToUs(cyclesLeft), timeoutPtr);
		}

		__KernelResumeThreadFromWait(threadID, reason);
		wokeThreads = true;
		// TODO: set timeoutPtr.
	}
	s->waitingThreads.empty();

	// TODO: Any way to erase the CoreTiming event type?  We leak.

	return wokeThreads;
}

// int sceKernelCancelSema(SceUID id, int newCount, int *numWaitThreads);
// void because it changes threads.
void sceKernelCancelSema(SceUID id, int newCount, u32 numWaitThreadsPtr)
{
	DEBUG_LOG(HLE,"sceKernelCancelSema(%i)", id);

	u32 error;
	Semaphore *s = kernelObjects.Get<Semaphore>(id, error);
	if (s)
	{
		if (newCount > s->ns.maxCount)
		{
			RETURN(SCE_KERNEL_ERROR_ILLEGAL_COUNT);
			return;
		}

		if (numWaitThreadsPtr)
		{
			u32* numWaitThreads = (u32*)Memory::GetPointer(numWaitThreadsPtr);
			*numWaitThreads = s->ns.numWaitThreads;
		}

		if (newCount < 0)
			s->ns.currentCount = s->ns.initCount;
		else
			s->ns.currentCount = newCount;
		s->ns.numWaitThreads = 0;

		// We need to set the return value BEFORE rescheduling threads.
		RETURN(0);

		__KernelClearSemaThreads(s, SCE_KERNEL_ERROR_WAIT_CANCEL);
		__KernelReSchedule("semaphore cancelled");
	}
	else
	{
		ERROR_LOG(HLE, "sceKernelCancelSema : Trying to cancel invalid semaphore %i", id);
		RETURN(error);
	}
}

//SceUID sceKernelCreateSema(const char *name, SceUInt attr, int initVal, int maxVal, SceKernelSemaOptParam *option);
// void because it changes threads.
void sceKernelCreateSema(const char* name, u32 attr, int initVal, int maxVal, u32 optionPtr)
{
	if (!name)
	{
		RETURN(SCE_KERNEL_ERROR_ERROR);
		return;
	}

	Semaphore *s = new Semaphore;
	SceUID id = kernelObjects.Create(s);

	s->ns.size = sizeof(NativeSemaphore);
	strncpy(s->ns.name, name, 31);
	s->ns.name[31] = 0;
	s->ns.attr = attr;
	s->ns.initCount = initVal;
	s->ns.currentCount = s->ns.initCount;
	s->ns.maxCount = maxVal;
	s->ns.numWaitThreads = 0;
	s->waitTimer = 0;

	DEBUG_LOG(HLE,"%i=sceKernelCreateSema(%s, %08x, %i, %i, %08x)", id, s->ns.name, s->ns.attr, s->ns.initCount, s->ns.maxCount, optionPtr);

	if (optionPtr != 0)
		WARN_LOG(HLE,"sceKernelCreateSema(%s) unsupported options parameter.", name);

	RETURN(id);

	__KernelReSchedule("semaphore created");
}

//int sceKernelDeleteSema(SceUID semaid);
// void because it changes threads.
void sceKernelDeleteSema(SceUID id)
{
	DEBUG_LOG(HLE,"sceKernelDeleteSema(%i)", id);

	u32 error;
	Semaphore *s = kernelObjects.Get<Semaphore>(id, error);
	if (s)
	{
		__KernelClearSemaThreads(s, SCE_KERNEL_ERROR_WAIT_DELETE);
		RETURN(kernelObjects.Destroy<Semaphore>(id));
		__KernelReSchedule("semaphore deleted");
	}
	else
	{
		ERROR_LOG(HLE, "sceKernelDeleteSema : Trying to delete invalid semaphore %i", id);
		RETURN(error);
	}
}

//int sceKernelDeleteSema(SceUID semaid, SceKernelSemaInfo *info);
// void because it changes threads.
void sceKernelReferSemaStatus(SceUID id, u32 infoPtr)
{
	u32 error;
	Semaphore *s = kernelObjects.Get<Semaphore>(id, error);
	if (s)
	{
		DEBUG_LOG(HLE,"sceKernelReferSemaStatus(%i, %08x)", id, infoPtr);
		Memory::WriteStruct(infoPtr, &s->ns);
		RETURN(0);
		__KernelReSchedule("semaphore refer status");
	}
	else
	{
		ERROR_LOG(HLE,"Error %08x", error);
		RETURN(error);
	}
}
	
//int sceKernelSignalSema(SceUID semaid, int signal);
// void because it changes threads.
void sceKernelSignalSema(SceUID id, int signal)
{
	//TODO: check that this thing really works :)
	u32 error;
	Semaphore *s = kernelObjects.Get<Semaphore>(id, error);
	if (s)
	{
		if (s->ns.currentCount + signal > s->ns.maxCount)
		{
			RETURN(SCE_KERNEL_ERROR_SEMA_OVF);
			return;
		}

		int oldval = s->ns.currentCount;
		s->ns.currentCount += signal;
		DEBUG_LOG(HLE,"sceKernelSignalSema(%i, %i) (old: %i, new: %i)", id, signal, oldval, s->ns.currentCount);

		// We need to set the return value BEFORE processing other threads.
		RETURN(0);

		bool wokeThreads = false;
retry:
		// TODO: PSP_SEMA_ATTR_PRIORITY
		std::vector<SceUID>::iterator iter;
		for (iter = s->waitingThreads.begin(); iter!=s->waitingThreads.end(); iter++)
		{
			SceUID threadID = *iter;

			int wVal = (int)__KernelGetWaitValue(threadID, error);
			u32 timeoutPtr = __KernelGetWaitTimeoutPtr(threadID, error);

			if (wVal <= s->ns.currentCount)
			{
				s->ns.currentCount -= wVal;
				s->ns.numWaitThreads--;

				if (timeoutPtr != 0 && s->waitTimer != 0)
				{
					// Remove any event for this thread.
					int cyclesLeft = CoreTiming::UnscheduleEvent(s->waitTimer, threadID);
					Memory::Write_U32(cyclesToUs(cyclesLeft), timeoutPtr);
				}

				__KernelResumeThreadFromWait(threadID, 0);
				wokeThreads = true;
				s->waitingThreads.erase(iter);
				goto retry;
			}
			else
			{
				break;
			}
		}

		__KernelReSchedule("semaphore signalled");
	}
	else
	{
		ERROR_LOG(HLE, "sceKernelSignalSema : Trying to signal invalid semaphore %i", id);
		RETURN(error;)
	}
}

void __KernelSemaTimeout(u64 userdata, int cycleslate)
{
	SceUID threadID = (SceUID)userdata;

	u32 error;
	u32 timeoutPtr = __KernelGetWaitTimeoutPtr(threadID, error);
	if (timeoutPtr != 0)
		Memory::Write_U32(0, timeoutPtr);

	SceUID semaID = __KernelGetWaitID(threadID, error);
	Semaphore *s = kernelObjects.Get<Semaphore>(semaID, error);
	if (s)
	{
		// This thread isn't waiting anymore.
		s->waitingThreads.erase(std::remove(s->waitingThreads.begin(), s->waitingThreads.end(), threadID), s->waitingThreads.end());
		s->ns.numWaitThreads--;
	}

	__KernelResumeThreadFromWait(threadID, SCE_KERNEL_ERROR_WAIT_TIMEOUT);
}

void __KernelSetSemaTimeout(Semaphore *s, u32 timeoutPtr)
{
	if (timeoutPtr == 0)
		return;

	if (s->waitTimer == 0)
		s->waitTimer = CoreTiming::RegisterEvent("SemaphoreTimeout", &__KernelSemaTimeout);

	// This should call __KernelMutexTimeout() later, unless we cancel it.
	int micro = (int) Memory::Read_U32(timeoutPtr);
	CoreTiming::ScheduleEvent(usToCycles(micro), s->waitTimer, __KernelGetCurThread());
}

void __KernelWaitSema(SceUID id, int wantedCount, u32 timeoutPtr, const char *badSemaMessage, bool processCallbacks)
{
	u32 error;
	Semaphore *s = kernelObjects.Get<Semaphore>(id, error);
	if (s)
	{
		if (wantedCount > s->ns.maxCount || wantedCount <= 0)
		{
			RETURN(SCE_KERNEL_ERROR_ILLEGAL_COUNT);
			return;
		}

		// We need to set the return value BEFORE processing callbacks / etc.
		RETURN(0);

		if (s->ns.currentCount >= wantedCount)
			s->ns.currentCount -= wantedCount;
		else
		{
			s->ns.numWaitThreads++;
			s->waitingThreads.push_back(__KernelGetCurThread());
			__KernelSetSemaTimeout(s, timeoutPtr);
			__KernelWaitCurThread(WAITTYPE_SEMA, id, wantedCount, timeoutPtr, processCallbacks);
			if (processCallbacks)
				__KernelCheckCallbacks();
		}

		__KernelReSchedule("semaphore waited");
	}
	else
	{
		ERROR_LOG(HLE, badSemaMessage, id);
		RETURN(error);
	}
}

//int sceKernelWaitSema(SceUID semaid, int signal, SceUInt *timeout);
// void because it changes threads.
void sceKernelWaitSema(SceUID id, int wantedCount, u32 timeoutPtr)
{
	DEBUG_LOG(HLE,"sceKernelWaitSema(%i, %i, %i)", id, wantedCount, timeoutPtr);

	__KernelWaitSema(id, wantedCount, timeoutPtr, "sceKernelWaitSema: Trying to wait for invalid semaphore %i", false);
} 

//int sceKernelWaitSemaCB(SceUID semaid, int signal, SceUInt *timeout);
// void because it changes threads.
void sceKernelWaitSemaCB(SceUID id, int wantedCount, u32 timeoutPtr)
{
	DEBUG_LOG(HLE,"sceKernelWaitSemaCB(%i, %i, %i)", id, wantedCount, timeoutPtr);

	__KernelWaitSema(id, wantedCount, timeoutPtr, "sceKernelWaitSemaCB: Trying to wait for invalid semaphore %i", true);
}

// Should be same as WaitSema but without the wait, instead returning SCE_KERNEL_ERROR_SEMA_ZERO
void sceKernelPollSema(SceUID id, int wantedCount)
{
	DEBUG_LOG(HLE,"sceKernelPollSema(%i, %i)", id, wantedCount);

	if (wantedCount <= 0)
	{
		RETURN(SCE_KERNEL_ERROR_ILLEGAL_COUNT);
		return;
	}

	u32 error;
	Semaphore *s = kernelObjects.Get<Semaphore>(id, error);
	if (s)
	{
		if (s->ns.currentCount >= wantedCount)
		{
			s->ns.currentCount -= wantedCount;
			RETURN(0);
		}
		else
			RETURN(SCE_KERNEL_ERROR_SEMA_ZERO);

		__KernelReSchedule("semaphore polled");
	}
	else
	{
		ERROR_LOG(HLE, "sceKernelPollSema: Trying to poll invalid semaphore %i", id);
		RETURN(error);
	}
}

