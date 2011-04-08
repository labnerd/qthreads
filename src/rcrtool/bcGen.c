/**
 * Breadcrumbs generation for RCR Daemon. 
 * By Anirban Mandal
 *
 */

#include "bcGen.h"
#include "qt_rcrtool.h"
#include "triggers.h"
#include <stdio.h>
#include <errno.h>

int getNumTriggers(void) {
    return numTriggers;
}

Trigger** getTriggers(void) {
    return triggerMap;
}

/*!
 * Check if condition for leaving breadcrumbs is met and leave breadcrumbs. Will
 * only put breadcrumbs for meters appearing in the trigger file.
 * 
 * \param triggerType
 * \param socketOrCoreID
 * \param meterName
 * \param currentVal
 * 
 * \return 1 if a a trigger is hit, otherwise return 0.
 */
int putBreadcrumbs(rcrtool_trigger_type triggerType, int socketOrCoreID, const char *meterName, double currentVal){
#ifdef QTHREAD_RCRTOOL
#define MAX_TRIGGERS 64
    static int flipped[MAX_TRIGGERS];// = 0;
#endif
    int i = 0;
    int triggerHit = 0;
    char c;
    // Check if a triggerMap exists; if not, just return
    if (triggerMap == NULL || numTriggers == 0) {
        printf("** BC: triggerMap is NULL; Can't put breadcrumbs\n");
        return 0;
    }

    for (i = 0; i < numTriggers; i++) {
        if ((triggerMap[i]->type == triggerType) && (triggerMap[i]->id == socketOrCoreID) && (strcmp(triggerMap[i]->meterName, meterName) == 0)) {
            if ((currentVal < triggerMap[i]->threshold_lb) || (currentVal > triggerMap[i]->threshold_ub)) {//trigger cond
#ifdef QTHREAD_RCRTOOL
                if (i < MAX_TRIGGERS && flipped[i] != 1) {
                    flipped[i] = 1;
                    //rcrtool_debug(RCR_RATTABLE_DEBUG," Hit trigger for %s on %d\n,", triggerMap[i]->meterName, triggerMap[i]->id);
                }
                dumpAppState(triggerMap[i]->appStateShmKey, triggerType, socketOrCoreID, meterName);
#endif
                triggerHit = 1;
                c = '1'; // Set the flag
                shmPutFlag(triggerMap[i]->flagShmKey , c);
            } else {
#ifdef QTHREAD_RCRTOOL
                if (i < MAX_TRIGGERS && flipped[i] != 0) {
                    flipped[i] = 0;
                    //rcrtool_debug(RCR_RATTABLE_DEBUG," Left trigger for %s on %d\n,", triggerMap[i]->meterName, triggerMap[i]->id);
                }
#endif
                c = '0'; // Unset
                shmPutFlag(triggerMap[i]->flagShmKey , c);
            }
        }
    }
    return triggerHit;
}

/**
 * Check if any breadcrumbs have been dropped. Will only check breadcrumbs for
 * meters appearing in the trigger file.
 * 
 * @param type Type of trigger.  From the <i>rcrtool_trigger_type</i> enum.
 * @param id Corresponding Node, Socket, or Core id number.
 * @param meterName Name of trigger as defined in the triggers definition file.
 *  
 * @return Value of the breadcrumb.  Should normally be either '0' or '1'. 
 *         Returns '0' on failure to read breadcrumb.
 */
char getBreadcrumbs(rcrtool_trigger_type triggerType, int id, char *meterName){

    int i = 0;
    // Check if a triggerMap exists; if not, just return
    if (triggerMap == NULL || numTriggers == 0) {
        printf("** BC: triggerMap is NULL; Can't get breadcrumbs\n");
        return '\0';
    }

    for (i = 0; i < numTriggers; i++) {
        if ((triggerMap[i]->type == triggerType) && (triggerMap[i]->id == id) && (strcmp(triggerMap[i]->meterName, meterName) == 0)) {
            return shmGet(triggerMap[i]->flagShmKey);
        }
    }
    return '\0';
}

/** Shared memory get function. 
 * 
 * @param key 
 * 
 * @return char 
 */
char shmGet(key_t key){

    int shmid;
    char *shm;
    char returnVal;

    if ((shmid = shmget(key, sizeof(char), 0666)) < 0) {
        printf("** BC: Could not locate shared memory segment for key: %d\n", key);
        // shm location for this key doesn't have a shared memory id
        // inserting breadcrumb for the first time
        // create a shared mameory id for this key
        if ((shmid = shmget(key, sizeof(char), IPC_CREAT | 0666)) < 0) {
            printf("** BC: Could not create new shared memory segment for key: %d\n", key);
            perror("shmget");
            return '\0';
        }
    }

    if ((shm = shmat(shmid, NULL, 0)) == (char *) -1) {
        perror("shmat");
        printf("** BC: Could not attach to the shared memory segment with shmid: %d \n ", shmid);
        return '\0';
    }

    returnVal = *shm;

    releaseShmLoc(shm);

    return returnVal;
}

/***********************************************************************************************
* shared memory put function
***********************************************************************************************/

void shmPutFlag(key_t key, char val){

    int shmid;
    char *shm;

    if ((shmid = shmget(key, sizeof(char), 0666)) < 0) {
        printf("** BC: Could not locate shared memory segment for key: %d\n", key);
        // shm location for this key doesn't have a shared memory id
        // inserting breadcrumb for the first time
        // create a shared memory id for this key
        if ((shmid = shmget(key, sizeof(char), IPC_CREAT | 0666)) < 0) {
            printf("** BC: Could not create new shared memory segment for key: %d\n", key);
            perror("shmget");
            return;
        }
    }

    if ((shm = shmat(shmid, NULL, 0)) == (char *) -1) {
        perror("shmat");
        printf("** BC: Could not attach to the shared memory segment with shmid: %d \n ", shmid);
        return;
    }

    // Set the val passed at the shm location
    //printf("** BC: Putting %c in shared memory location with id: %d \n", val, shmid);
    *shm = val;

    releaseShmLoc(shm);
}

char* getShmStringLoc(key_t key, size_t size) {
    key = 9253;
    size = 64;
    //if (size <= allocatedSize) {
    //} else ;
    int shmid;
    char *shm;

    if ((shmid = shmget(key, sizeof(char) * size, 0666)) < 0) {
        printf("** BC: Could not locate shared memory segment for key: %d\n", key);
        if (0 && errno == EINVAL) {
            //exists but size too small.  Get it by asking for smallest size and delete it.
            if ((shmid = shmget(key, 1, 0666)) < 0) {
                //failure
                perror("shmget");
                return 0;
            }
            //struct shmid_ds my_shmid_ds;
            //shmctl(shmid, IPC_RMID, &my_shmid_ds);
            //if (shmdt(shm) == -1) {
            //    printf("shmdt failed\n");
            //    return 0;
            //}
        }
        // shm location for this key doesn't have a shared memory id inserting
        // breadcrumb for the first time or after reallocation create a shared
        // memory id for this key
        if ((shmid = shmget(key, sizeof(char) * size, IPC_CREAT | 0666)) < 0) {
            printf("** BC: Could not create new shared memory segment for key: %d\n", key);
            perror("shmget");
            return 0;
        }
    }

    if ((shm = shmat(shmid, NULL, 0)) == (char *)-1) {
        perror("shmat");
        printf("** BC: Could not attach to the shared memory segment with shmid: %d \n ", shmid);
        return 0;
    }

    return (char*)shm;
}

void releaseShmLoc(const void* shm) {
  if (shmdt(shm) == -1) {
      perror("shmdt");
      printf("shmdt failed\n");
  }
}

# ifdef QTHREAD_RCRTOOL
/*!
 * Write the application state out to shared memory.
 * 
 * \param key Shared memory key.
 * \param triggerType Not currently used.
 * \param socketOrCoreID Not currently used.
 * \param meterName Not currently used.
 */
void dumpAppState(key_t key, rcrtool_trigger_type triggerType, int socketOrCoreID, const char *meterName) {
    size_t appStateSize = 1024;
    int i = 0;
    //printf("%d @@\n", RCRParallelSectionStackPos);
    appStateSize += (RCR_HASH_ENTRY_SIZE + 10) * RCRParallelSectionStackPos + RCR_APP_NAME_MAX_SIZE;
    char *shmDest = getShmStringLoc(key, appStateSize);
    char *shmRover = shmDest;
    strncpy(shmRover, RCRAppName, RCR_APP_NAME_MAX_SIZE);
    shmRover += strlen(RCRAppName);
    for (i = 0; i <= RCRParallelSectionStackPos; i++) {
        //printf("%s %d ^^^^\n", hashTable[RCRParallelSectionStack[i]].funcName, hashTable[RCRParallelSectionStack[i]].count);
        *shmRover++ = '|';
        strncpy(shmRover, hashTable[RCRParallelSectionStack[i]].funcName, RCR_HASH_ENTRY_SIZE);
        shmRover += strlen(hashTable[RCRParallelSectionStack[i]].funcName);
        *shmRover++ = '|';
        sprintf(shmRover, "%8d", hashTable[RCRParallelSectionStack[i]].count);
        shmRover += 8;
    }

    //Do the writing.
    releaseShmLoc(shmDest);
}
# endif

/***********************************************************************************************
* Prints triggerMap
***********************************************************************************************/
void printTriggerMap(){
    int i;
    printf("Number of triggers: %d\n", numTriggers);
    for (i = 0; i < numTriggers; i++) {
        printf("%d \t %d \t %d \t %d \t %s \t %f \t %f \n", triggerMap[i]->type, triggerMap[i]->id, triggerMap[i]->flagShmKey, triggerMap[i]->appStateShmKey, triggerMap[i]->meterName, triggerMap[i]->threshold_lb, triggerMap[i]->threshold_ub);
    }
}