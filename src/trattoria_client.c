// basic c stuff we need so the code actually compiles
#include <stdio.h>   
#include <stdlib.h>  
#include <unistd.h>  
#include <string.h>  

// all the weird system v ipc libraries required to talk to the server
#include <sys/ipc.h> 
#include <sys/msg.h> 
#include <sys/shm.h> 
#include <sys/sem.h> 

// thread library!
#include <pthread.h> 

// the header the prof gave us
#include "ipc.h"     

// =========================================================================
// TASK 3: SEMAPHORE WRAPPERS (THE TRAFFIC LIGHT)
// =========================================================================

/**
 * Lock: Sets the semaphore to "Red". If it's already Red, the thread waits.
 * We use sem_op = -1 to "decrement" (wait/lock).
 */
void lock_blackboard(int sem_id) {
    struct sembuf sb;
    sb.sem_num = SEMIDX_BLACKBOARD; // Index 0 as defined in ipc.h
    sb.sem_op = -1;                 // The "Wait" operation
    sb.sem_flg = 0;
    if (semop(sem_id, &sb, 1) == -1) {
        perror("Semaphore Lock Failed");
    }
}

/**
 * Unlock: Sets the semaphore back to "Green".
 * We use sem_op = 1 to "increment" (signal/unlock).
 */
void unlock_blackboard(int sem_id) {
    struct sembuf sb;
    sb.sem_num = SEMIDX_BLACKBOARD;
    sb.sem_op = 1;                  // The "Signal" operation
    sb.sem_flg = 0;
    if (semop(sem_id, &sb, 1) == -1) {
        perror("Semaphore Unlock Failed");
    }
}

// =========================================================================
// THE STAFF THREAD LOGIC (THE BRAIN)
// =========================================================================

// Global variable for the semaphore ID so threads can see it
int global_sem_id;
volatile int instance_active = 0; 

void* staff_worker(void* arg) {
    int my_id = *((int*)arg); 
    
    printf("[Thread] Staff member %d has clocked in!\n", my_id);

    while (instance_active) {
        
        // --- TEST TASK 3: Safe Blackboard Access ---
        // Every thread will try to grab the lock, print a message, and release it.
        lock_blackboard(global_sem_id);
        
        // Technically, this is where we would write to the shm_blackboard_t struct.
        // For now, we just prove we have exclusive access.
        printf("[Staff %d] I have the talking stick! Checking the blackboard...\n", my_id);
        usleep(50000); // Hold the lock for a tiny bit to simulate writing
        
        unlock_blackboard(global_sem_id);
        // --------------------------------------------

        // Wait a bit before trying to "work" again to let other threads have a turn
        usleep(200000); 
    }

    printf("[Thread] Staff member %d is clocking out.\n", my_id);
    return NULL;
}

int main(int argc, char **argv) {
    printf("Starting Trattoria Client...\n");

    // 1. C2S QUEUE
    int qid_c2s = msgget(ftok(TRATTORIA_FTOK_PATH, PROJ_MSG_C2S), 0666);
    if (qid_c2s == -1) { perror("c2s queue failed"); exit(1); }

    // 2. S2C QUEUE
    int qid_s2c = msgget(ftok(TRATTORIA_FTOK_PATH, PROJ_MSG_S2C), 0666);
    if (qid_s2c == -1) { perror("s2c queue failed"); exit(1); }

    // 3. FATIGUE QUEUE
    int qid_fatigue = msgget(ftok(TRATTORIA_FTOK_PATH, PROJ_MSG_FATIGUE), 0666);
    if (qid_fatigue == -1) { perror("fatigue queue failed"); exit(1); }

    printf("message queues connected!\n");
    
    // 4. DINING ROOM MEMORY
    int shm_dining_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_DININGROOM), sizeof(shm_diningroom_t), 0666);
    if (shm_dining_id == -1) { perror("dining room shmget failed"); exit(1); }
    shm_diningroom_t *dining_room = (shm_diningroom_t *) shmat(shm_dining_id, NULL, 0);

    // 5. KITCHEN MEMORY
    int shm_kitchen_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_KITCHEN), sizeof(shm_kitchen_t), 0666);
    if (shm_kitchen_id == -1) { perror("kitchen shmget failed"); exit(1); }
    shm_kitchen_t *kitchen = (shm_kitchen_t *) shmat(shm_kitchen_id, NULL, 0);

    // 6. CASH DESK MEMORY
    int shm_cash_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_CASHDESK), sizeof(shm_cashdesk_t), 0666);
    if (shm_cash_id == -1) { perror("cash desk shmget failed"); exit(1); }
    shm_cashdesk_t *cash_desk = (shm_cashdesk_t *) shmat(shm_cash_id, NULL, 0);

    // 7. BLACKBOARD MEMORY
    int shm_blackboard_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_BLACKBOARD), sizeof(shm_blackboard_t), 0666);
    if (shm_blackboard_id == -1) { perror("blackboard shmget failed"); exit(1); }
    shm_blackboard_t *blackboard = (shm_blackboard_t *) shmat(shm_blackboard_id, NULL, 0);

    printf("shared memory attached!\n");

    // 8. SEMAPHORES
    global_sem_id = semget(ftok(TRATTORIA_FTOK_PATH, PROJ_SEM), SEM_NSEMS, 0666);
    if (global_sem_id == -1) { perror("semaphores failed"); exit(1); }
    printf("semaphores connected!\n");

    // 9. THE HANDSHAKE (HELLO)
    msg_hello_t hello_msg;
    memset(&hello_msg, 0, sizeof(hello_msg)); 
    hello_msg.mtype = MSGTYPE_HELLO; 
    hello_msg.pid = getpid();        
    hello_msg.studentid_n = 2;       
    strncpy(hello_msg.studentids[0], "VR123456", STUDENTID_MAXLEN); 
    strncpy(hello_msg.studentids[1], "VR654321", STUDENTID_MAXLEN);
    hello_msg.has_strategy = TR_TRUE; 
    hello_msg.strategy = STRATEGY_PROFIT; 

    size_t hello_size = sizeof(msg_hello_t) - sizeof(long);
    if (msgsnd(qid_c2s, &hello_msg, hello_size, 0) == -1) { perror("send hello failed"); exit(1); }
    printf("sent HELLO to the server!\n");

    // 10. THE HANDSHAKE (WELCOME)
    msg_welcome_t welcome_msg;
    size_t welcome_size = sizeof(msg_welcome_t) - sizeof(long);
    if (msgrcv(qid_s2c, &welcome_msg, welcome_size, MSGTYPE_WELCOME, 0) == -1) { perror("welcome failed"); exit(1); }
    printf("received WELCOME! the server gave us %d staff members.\n", welcome_msg.staff_n);

    // =========================================================================
    // MAIN LOOP (TASK 1, 2)
    // =========================================================================
    printf("\nEntering main simulation loop...\n");

    union {
        long mtype;                      
        msg_instance_t instance;         
        msg_instance_done_t done;        
        msg_end_t end;                   
    } s2c_msg;

    size_t max_msg_size = sizeof(s2c_msg) - sizeof(long);
    pthread_t staff_threads[MAX_STAFF];
    int staff_ids[MAX_STAFF];

    while (1) {
        if (msgrcv(qid_s2c, &s2c_msg, max_msg_size, 0, 0) == -1) {
            perror("msgrcv failed");
            break; 
        }

        if (s2c_msg.mtype == MSGTYPE_INSTANCE) {
            printf("\n[--- NEW ROUND STARTING ---]\n");
            instance_active = 1; 
            for (int i = 0; i < welcome_msg.staff_n; i++) {
                staff_ids[i] = i;
                pthread_create(&staff_threads[i], NULL, staff_worker, &staff_ids[i]);
            }
        } 
        else if (s2c_msg.mtype == MSGTYPE_INSTANCE_DONE) {
            printf("\n[--- ROUND FINISHED ---]\n");
            instance_active = 0; 
            for (int i = 0; i < welcome_msg.staff_n; i++) {
                pthread_join(staff_threads[i], NULL);
            }
            printf("[System] All staff threads closed cleanly.\n");
            printf("Avg Time: %.2f | Avg Reviews: %s\n", s2c_msg.done.total_families_time, s2c_msg.done.average_families_score_review);
        } 
        else if (s2c_msg.mtype == MSGTYPE_END) {
            printf("\n[--- SERVER SHUTTING DOWN ---]\n");
            break; 
        }
    }

    // 11. THE TEARDOWN
    shmdt(dining_room);
    shmdt(kitchen);
    shmdt(cash_desk);
    shmdt(blackboard);

    printf("detached memory cleanly. Tuesday sprint complete!\n");

    return 0;
}
