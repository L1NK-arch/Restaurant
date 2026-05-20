#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <pthread.h>
#include <errno.h>
#include "ipc.h"

// ========== GLOBAL DATA ==========

shm_diningroom_t *g_dining = NULL;
shm_kitchen_t    *g_kitchen = NULL;
shm_cashdesk_t   *g_cash   = NULL;
shm_blackboard_t *g_board  = NULL;
int g_qid_fatigue;
int g_current_strategy = STRATEGY_NONE;

// --- Staff info saved from WELCOME (needed by threads) ---
staff_member_t g_welcome_staff[MAX_STAFF];
int            g_welcome_staff_n = 0;

// ========== SEMAPHORE HELPERS ==========

void lock_blackboard(int sem_id) {
    struct sembuf sb;
    sb.sem_num = SEMIDX_BLACKBOARD;
    sb.sem_op = -1;
    sb.sem_flg = 0;
    if (semop(sem_id, &sb, 1) == -1) {
        perror("Semaphore Lock Failed");
    }
}

void unlock_blackboard(int sem_id) {
    struct sembuf sb;
    sb.sem_num = SEMIDX_BLACKBOARD;
    sb.sem_op = 1;
    sb.sem_flg = 0;
    if (semop(sem_id, &sb, 1) == -1) {
        perror("Semaphore Unlock Failed");
    }
}

// ========== KHADY'S HELPER FUNCTIONS (FIXED) ==========

// --- State Readers ---
int count_tables_by_state(const shm_diningroom_t *room, table_state_t target) {
    int count = 0;
    for (int i = 0; i < room->tables_n; i++) {
        if (room->tables[i].state == target) count++;
    }
    return count;
}

int count_dirty_tables(const shm_diningroom_t *room) {
    return count_tables_by_state(room, TABLE_FREED);
}

int count_ordering_tables(const shm_diningroom_t *room) {
    return count_tables_by_state(room, TABLE_TAKEN);
}

int count_serving_tables(const shm_diningroom_t *room) {
    return count_tables_by_state(room, TABLE_SERVED);
}

int get_pending_orders(const shm_kitchen_t *kitchen) {
    return kitchen->pending_orders;
}

level_t get_dirty_plates_level(const shm_kitchen_t *kitchen) {
    return kitchen->dirty_plates;
}

level_t get_clean_plates_level(const shm_kitchen_t *kitchen) {
    return kitchen->clean_plates;
}

int is_food_ready_for_table(const shm_kitchen_t *kitchen, int table_idx) {
    if (table_idx < 0 || table_idx >= MAX_TABLES) return 0;
    return kitchen->food_ready[table_idx] == TR_TRUE;
}

// --- Staff Profiling ---
int best_staff_for_role(const staff_member_t *staff, int n_staff, role_t role) {
    if (n_staff <= 0) return -1;
    int best_id = 0;
    param_bucket_t best_skill = PARAM_LOW;
    int skill_idx = -1;
    switch (role) {
        case ROLE_WAITER:     skill_idx = SKILL_WAITER;  break;
        case ROLE_COOK:       skill_idx = SKILL_COOK;    break;
        case ROLE_HELPER:     skill_idx = SKILL_HELPER;  break;
        case ROLE_CASHIER:    skill_idx = SKILL_CASHIER; break;
        case ROLE_DISHWASHER: skill_idx = SKILL_HELPER;  break;
        default: return -1;
    }
    for (int i = 0; i < n_staff; i++) {
        param_bucket_t cur = staff[i].skills[skill_idx];
        if (cur > best_skill) {
            best_skill = cur;
            best_id = i;
        }
    }
    return best_id;
}

int best_waiter(const staff_member_t *staff, int n_staff) {
    return best_staff_for_role(staff, n_staff, ROLE_WAITER);
}
int best_cook(const staff_member_t *staff, int n_staff) {
    return best_staff_for_role(staff, n_staff, ROLE_COOK);
}
int best_helper(const staff_member_t *staff, int n_staff) {
    return best_staff_for_role(staff, n_staff, ROLE_HELPER);
}
int best_cashier(const staff_member_t *staff, int n_staff) {
    return best_staff_for_role(staff, n_staff, ROLE_CASHIER);
}

// --- Fatigue Monitor ---
static level_t fatigue_tracker[MAX_STAFF][NUM_ROLES];

void init_fatigue_tracker(void) {
    for (int i = 0; i < MAX_STAFF; i++)
        for (int r = 0; r < NUM_ROLES; r++)
            fatigue_tracker[i][r] = LVL_NONE;
}

int poll_fatigue_messages(int msqid, int staff_id) {
    msg_fatigue_t msg;
    int count = 0;
    long mtype_filter = (long)(staff_id + 1);
    while (1) {
        ssize_t ret = msgrcv(msqid, &msg, sizeof(msg_fatigue_t) - sizeof(long),
                             mtype_filter, IPC_NOWAIT);
        if (ret == -1) {
            if (errno == ENOMSG || errno == EAGAIN) break;
            perror("poll_fatigue_messages: msgrcv");
            break;
        }
        if (msg.staff_id >= 0 && msg.staff_id < MAX_STAFF &&
            msg.role >= 0 && msg.role < NUM_ROLES) {
            fatigue_tracker[msg.staff_id][msg.role] = msg.new_lvl;
        }
        count++;
    }
    return count;
}

level_t get_fatigue(int staff_id, role_t role) {
    if (staff_id < 0 || staff_id >= MAX_STAFF) return LVL_NONE;
    if (role < 0 || role >= NUM_ROLES) return LVL_NONE;
    return fatigue_tracker[staff_id][role];
}

int is_too_tired(int staff_id, role_t role) {
    return get_fatigue(staff_id, role) == LVL_HIGH;
}

// ========== KHADY'S NEW DECISION HELPERS ==========

/**
 * Write a staff assignment to the blackboard for a common role.
 * (Per‑table assignments will be added later.)
 */
void claim_role(shm_blackboard_t *board, role_t role, int staff_id) {
    switch (role) {
        case ROLE_COOK:       board->cook       = staff_id; break;
        case ROLE_CASHIER:    board->cashier    = staff_id; break;
        case ROLE_DISHWASHER: board->dishwasher = staff_id; break;
        // Per‑table roles (waiter, helper) are not handled yet
        default: break;
    }
}

/**
 * Decide which role this staff member should take next.
 * Currently a simple stub: dishwasher if free, else nothing.
 * Will be extended with the two real strategies.
 */
role_t decide_role(
    int staff_id,
    const shm_blackboard_t  *board,
    const shm_diningroom_t  *dining,
    const shm_kitchen_t     *kitchen,
    const shm_cashdesk_t    *cash,
    const staff_member_t    *staff,
    int                      n_staff,
    strategy_t               strategy
) {
    // Default: only dishwasher for now
    if (board->dishwasher == -1) {
        return ROLE_DISHWASHER;
    }
    return ROLE_NONE;
}

// ========== THREAD DATA ==========

typedef struct {
    int my_id;
} thread_data_t;

// ========== THREAD LOGIC ==========

int global_sem_id;
volatile int instance_active = 0;

void* staff_worker(void* arg) {
    thread_data_t *data = (thread_data_t*) arg;
    int my_id = data->my_id;

    printf("[Thread] Staff member %d has clocked in!\n", my_id);
    while (instance_active) {
        lock_blackboard(global_sem_id);

        // 1. Update fatigue info for this staff member
        poll_fatigue_messages(g_qid_fatigue, my_id);

        // 2. Ask the decision function what role to take
        role_t role = decide_role(
            my_id,
            g_board, g_dining, g_kitchen, g_cash,
            g_welcome_staff, g_welcome_staff_n,
            g_current_strategy
        );

        if (role != ROLE_NONE) {
            claim_role(g_board, role, my_id);
            printf("[Staff %d] claiming role %d\n", my_id, role);
        }

        unlock_blackboard(global_sem_id);

        // Wait a bit before trying again
        usleep(100000);
    }
    printf("[Thread] Staff member %d is clocking out.\n", my_id);
    return NULL;
}

// ========== MAIN ==========

int main(int argc, char **argv) {
    printf("Starting Trattoria Client...\n");

    // message queues
    int qid_c2s = msgget(ftok(TRATTORIA_FTOK_PATH, PROJ_MSG_C2S), 0666);
    if (qid_c2s == -1) { perror("c2s queue failed"); exit(1); }
    int qid_s2c = msgget(ftok(TRATTORIA_FTOK_PATH, PROJ_MSG_S2C), 0666);
    if (qid_s2c == -1) { perror("s2c queue failed"); exit(1); }
    int qid_fatigue = msgget(ftok(TRATTORIA_FTOK_PATH, PROJ_MSG_FATIGUE), 0666);
    if (qid_fatigue == -1) { perror("fatigue queue failed"); exit(1); }
    g_qid_fatigue = qid_fatigue;
    printf("message queues connected!\n");

    // shared memories
    int shm_dining_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_DININGROOM), sizeof(shm_diningroom_t), 0666);
    if (shm_dining_id == -1) { perror("dining room shmget failed"); exit(1); }
    shm_diningroom_t *dining_room = (shm_diningroom_t *) shmat(shm_dining_id, NULL, 0);
    g_dining = dining_room;

    int shm_kitchen_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_KITCHEN), sizeof(shm_kitchen_t), 0666);
    if (shm_kitchen_id == -1) { perror("kitchen shmget failed"); exit(1); }
    shm_kitchen_t *kitchen = (shm_kitchen_t *) shmat(shm_kitchen_id, NULL, 0);
    g_kitchen = kitchen;

    int shm_cash_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_CASHDESK), sizeof(shm_cashdesk_t), 0666);
    if (shm_cash_id == -1) { perror("cash desk shmget failed"); exit(1); }
    shm_cashdesk_t *cash_desk = (shm_cashdesk_t *) shmat(shm_cash_id, NULL, 0);
    g_cash = cash_desk;

    int shm_blackboard_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_BLACKBOARD), sizeof(shm_blackboard_t), 0666);
    if (shm_blackboard_id == -1) { perror("blackboard shmget failed"); exit(1); }
    shm_blackboard_t *blackboard = (shm_blackboard_t *) shmat(shm_blackboard_id, NULL, 0);
    g_board = blackboard;
    printf("shared memory attached!\n");

    // semaphores
    global_sem_id = semget(ftok(TRATTORIA_FTOK_PATH, PROJ_SEM), SEM_NSEMS, 0666);
    if (global_sem_id == -1) { perror("semaphores failed"); exit(1); }
    printf("semaphores connected!\n");

    // HELLO handshake
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

    // WELCOME
    msg_welcome_t welcome_msg;
    size_t welcome_size = sizeof(msg_welcome_t) - sizeof(long);
    if (msgrcv(qid_s2c, &welcome_msg, welcome_size, MSGTYPE_WELCOME, 0) == -1) { perror("welcome failed"); exit(1); }
    printf("received WELCOME! the server gave us %d staff members.\n", welcome_msg.staff_n);

    // Save staff information so threads can use it later
    g_welcome_staff_n = welcome_msg.staff_n;
    memcpy(g_welcome_staff, welcome_msg.staff, sizeof(staff_member_t) * welcome_msg.staff_n);

    printf("\nEntering main simulation loop...\n");

    union {
        long mtype;
        msg_instance_t instance;
        msg_instance_done_t done;
        msg_end_t end;
    } s2c_msg;
    size_t max_msg_size = sizeof(s2c_msg) - sizeof(long);
    pthread_t staff_threads[MAX_STAFF];
    thread_data_t thread_data[MAX_STAFF];

    while (1) {
        if (msgrcv(qid_s2c, &s2c_msg, max_msg_size, 0, 0) == -1) {
            perror("msgrcv failed");
            break;
        }
        if (s2c_msg.mtype == MSGTYPE_INSTANCE) {
            printf("\n[--- NEW ROUND STARTING ---]\n");
            instance_active = 1;

            // Save the strategy for this instance
            g_current_strategy = s2c_msg.instance.strategy;

            for (int i = 0; i < welcome_msg.staff_n; i++) {
                thread_data[i].my_id = i;
                pthread_create(&staff_threads[i], NULL, staff_worker, &thread_data[i]);
            }
        } else if (s2c_msg.mtype == MSGTYPE_INSTANCE_DONE) {
            printf("\n[--- ROUND FINISHED ---]\n");
            instance_active = 0;
            for (int i = 0; i < welcome_msg.staff_n; i++) {
                pthread_join(staff_threads[i], NULL);
            }
            printf("[System] All staff threads closed cleanly.\n");
            printf("Avg Time: %.2f | Avg Reviews: %s\n", s2c_msg.done.total_families_time, s2c_msg.done.average_families_score_review);
        } else if (s2c_msg.mtype == MSGTYPE_END) {
            printf("\n[--- SERVER SHUTTING DOWN ---]\n");
            break;
        }
    }

    // teardown
    shmdt(dining_room);
    shmdt(kitchen);
    shmdt(cash_desk);
    shmdt(blackboard);
    printf("detached memory cleanly. Tuesday sprint complete!\n");
    return 0;
}
