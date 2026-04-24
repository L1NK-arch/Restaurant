// basic c stuff we need so the code actually compiles
#include <stdio.h>   // we need this for printing stuff to the terminal (printf) and error messages (perror)
#include <stdlib.h>  // gives us the exit() function so we can kill the program if something breaks
#include <unistd.h>  // we need this specifically to grab our process ID (getpid) for the server later
#include <string.h>  // gives us memset (to wipe structs clean) and strncpy (to copy our matricola strings)

// all the weird system v ipc libraries required to talk to the server
#include <sys/ipc.h> // the core ipc library, gives us the ftok() function to generate our connection keys
#include <sys/msg.h> // lets us use the message queue functions (msgget, msgsnd, msgrcv)
#include <sys/shm.h> // lets us use the shared memory functions (shmget, shmat, shmdt)
#include <sys/sem.h> // lets us use the semaphore functions (semget)

// the header the prof gave us
#include "ipc.h"     // contains all the custom structs, max lengths, and the hex codes (like 0x41) we need

int main(int argc, char **argv) {
    printf("Starting Trattoria Client...\n");

    // 1. C2S QUEUE: trying to connect to the mailbox where WE send messages to the server
    // we use ftok to combine the file path and the specific project ID into a unique key
    // 0666 gives us read and write permissions so linux doesn't block us
    int qid_c2s = msgget(ftok(TRATTORIA_FTOK_PATH, PROJ_MSG_C2S), 0666);
    if (qid_c2s == -1) { 
        // if this returns -1, it means the mailbox doesn't exist yet (aka we forgot to run the server first)
        perror("bruh c2s queue failed (is the server running?)"); 
        exit(1); // crash the program safely instead of continuing and segfaulting
    }

    // 2. S2C QUEUE: trying to connect to the mailbox where the server replies to us
    int qid_s2c = msgget(ftok(TRATTORIA_FTOK_PATH, PROJ_MSG_S2C), 0666);
    if (qid_s2c == -1) { 
        perror("s2c queue failed"); 
        exit(1); 
    }

    // 3. FATIGUE QUEUE: the dedicated mailbox the server uses to spam us when our staff get tired
    int qid_fatigue = msgget(ftok(TRATTORIA_FTOK_PATH, PROJ_MSG_FATIGUE), 0666);
    if (qid_fatigue == -1) { 
        perror("fatigue queue failed"); 
        exit(1); 
    }

    printf("message queues connected!\n");
    
    // 4. DINING ROOM MEMORY: grabbing the ID for the dining room block
    int shm_dining_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_DININGROOM), sizeof(shm_diningroom_t), 0666);
    if (shm_dining_id == -1) { perror("dining room shmget failed"); exit(1); }
    // shmat "attaches" that memory ID to a local pointer, so we can read it like a normal c struct!
    shm_diningroom_t *dining_room = (shm_diningroom_t *) shmat(shm_dining_id, NULL, 0);

    // 5. KITCHEN MEMORY: grabbing the ID and attaching the pointer for the kitchen status
    int shm_kitchen_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_KITCHEN), sizeof(shm_kitchen_t), 0666);
    if (shm_kitchen_id == -1) { perror("kitchen shmget failed"); exit(1); }
    shm_kitchen_t *kitchen = (shm_kitchen_t *) shmat(shm_kitchen_id, NULL, 0);

    // 6. CASH DESK MEMORY: grabbing the ID and attaching the pointer to see pending payments
    int shm_cash_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_CASHDESK), sizeof(shm_cashdesk_t), 0666);
    if (shm_cash_id == -1) { perror("cash desk shmget failed"); exit(1); }
    shm_cashdesk_t *cash_desk = (shm_cashdesk_t *) shmat(shm_cash_id, NULL, 0);

    // 7. BLACKBOARD MEMORY: this is the big one. it's the ONLY memory block we will actually write data to later
    int shm_blackboard_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_BLACKBOARD), sizeof(shm_blackboard_t), 0666);
    if (shm_blackboard_id == -1) { perror("blackboard shmget failed"); exit(1); }
    shm_blackboard_t *blackboard = (shm_blackboard_t *) shmat(shm_blackboard_id, NULL, 0);

    printf("shared memory attached!\n");

    // 8. SEMAPHORES: getting the traffic lights. we aren't locking anything yet, just grabbing the remote control
    int sem_id = semget(ftok(TRATTORIA_FTOK_PATH, PROJ_SEM), SEM_NSEMS, 0666);
    if (sem_id == -1) { 
        perror("semaphores failed"); 
        exit(1); 
    }
    printf("semaphores connected!\n");

    // 9. THE HANDSHAKE (HELLO): setting up our greeting message
    msg_hello_t hello_msg;
    // ALWAYS wipe the struct clean with zeros first, otherwise random garbage memory gets sent
    memset(&hello_msg, 0, sizeof(hello_msg)); 

    hello_msg.mtype = MSGTYPE_HELLO; // required by the server to know this is a greeting
    hello_msg.pid = getpid();        // telling the server our linux process ID
    hello_msg.studentid_n = 2;       // letting it know we are a group of 2 students

    // copying our actual matricola numbers into the array
    strncpy(hello_msg.studentids[0], "VR123456", STUDENTID_MAXLEN); 
    strncpy(hello_msg.studentids[1], "VR654321", STUDENTID_MAXLEN);
    
    // fixing the bug we had earlier: the server REJECTS the hello if we don't pick a strategy in normal mode
    hello_msg.has_strategy = TR_TRUE; 
    hello_msg.strategy = STRATEGY_PROFIT; 

    // system v trap: the size of the message is the size of the struct MINUS the 'long mtype' variable
    size_t hello_size = sizeof(msg_hello_t) - sizeof(long);
    
    // sending the message to the C2S queue!
    if (msgsnd(qid_c2s, &hello_msg, hello_size, 0) == -1) {
        perror("bruh we failed to send hello");
        exit(1);
    }
    printf("sent HELLO to the server!\n");

    // 10. THE HANDSHAKE (WELCOME): waiting for the server to reply
    msg_welcome_t welcome_msg;
    size_t welcome_size = sizeof(msg_welcome_t) - sizeof(long);

    // msgrcv will literally pause our code on this line until the welcome message arrives in the S2C queue
    if (msgrcv(qid_s2c, &welcome_msg, welcome_size, MSGTYPE_WELCOME, 0) == -1) {
        perror("bruh we didn't get the welcome message");
        exit(1);
    }
    printf("received WELCOME! the server gave us %d staff members to manage.\n", welcome_msg.staff_n);

    // 11. THE TEARDOWN: we must detach all our pointers before the program ends so we don't leak RAM
    shmdt(dining_room);
    shmdt(kitchen);
    shmdt(cash_desk);
    shmdt(blackboard);

    printf("detached memory cleanly. friday sprint complete!\n");

    return 0; // success!
}
