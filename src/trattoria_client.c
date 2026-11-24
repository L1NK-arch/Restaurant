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
#include <stdatomic.h>
#include "ipc.h"

// ========== GLOBAL DATA ==========

shm_diningroom_t *g_dining = NULL;
shm_kitchen_t    *g_kitchen = NULL;
shm_cashdesk_t   *g_cash   = NULL;
shm_blackboard_t *g_board  = NULL;
int g_qid_fatigue;
int g_current_strategy = STRATEGY_PROFIT;

staff_member_t g_welcome_staff[MAX_STAFF];
int            g_welcome_staff_n = 0;

int global_sem_id;
atomic_int instance_active = 0;

// Pause between thread cycles, in REAL microseconds.
// Recalculated for each instance based on speed (see main)
int g_poll_us = 2000;

// After how many cycles the cleaner stops waiting and leaves the table.
// Also depends on speed.
int g_cleaner_cycles = 100;

// Tables we've given up on (server cleaned but didn't transition state).
// Protected by SEMIDX_BLACKBOARD. Cleared when state != TABLE_FREED so the
// table can be re-claimed on a later cleaning pass in the same instance.
static unsigned int g_timeout_cleaned_mask = 0;

// ========== SEMAPHORE HELPERS ==========

void lock_blackboard(int sem_id) {
    struct sembuf sb = {SEMIDX_BLACKBOARD, -1, 0};
    if (semop(sem_id, &sb, 1) == -1) perror("Semaphore Lock Failed");
}

void unlock_blackboard(int sem_id) {
    struct sembuf sb = {SEMIDX_BLACKBOARD, 1, 0};
    if (semop(sem_id, &sb, 1) == -1) perror("Semaphore Unlock Failed");
}

// ========== FATIGUE TRACKING ==========

static level_t fatigue_tracker[MAX_STAFF][NUM_ROLES];

void init_fatigue_tracker(void) {
    for (int i = 0; i < MAX_STAFF; i++)
        for (int r = 0; r < NUM_ROLES; r++)
            fatigue_tracker[i][r] = LVL_NONE;
}

void poll_fatigue_messages(int msqid, int staff_id) {
    msg_fatigue_t msg;
    long mtype_filter = (long)(staff_id + 1);
    while (1) {
        ssize_t ret = msgrcv(msqid, &msg, sizeof(msg_fatigue_t) - sizeof(long), mtype_filter, IPC_NOWAIT);
        if (ret == -1) break;
        if (msg.staff_id >= 0 && msg.staff_id < MAX_STAFF && msg.role >= 0 && msg.role < NUM_ROLES) {
            fatigue_tracker[msg.staff_id][msg.role] = msg.new_lvl;
        }
    }
}

int is_too_tired(int staff_id, role_t role) {
    if (staff_id < 0 || staff_id >= MAX_STAFF || role < 0 || role >= NUM_ROLES) return 0;
    return fatigue_tracker[staff_id][role] == LVL_HIGH;
}

// ========== STAFF PROFILING ==========

int best_staff_for_role(const staff_member_t *staff, int n_staff, role_t role, int use_trait) {
    if (n_staff <= 0) return -1;
    int best_id = 0;
    int best_val = -1;
    int idx = 0;

    if (use_trait) {
        if (role == ROLE_WAITER) idx = TRAIT_PATIENCE;
        else if (role == ROLE_CASHIER) idx = TRAIT_SOCIABILITY;
        else if (role == ROLE_COOK) idx = TRAIT_PROFESSIONALITY;
        else idx = TRAIT_RESISTANCE;

        for (int i = 0; i < n_staff; i++) {
            if (staff[i].traits[idx] > best_val) {
                best_val = staff[i].traits[idx];
                best_id = i;
            }
        }
    } else {
        if (role == ROLE_WAITER) idx = SKILL_WAITER;
        else if (role == ROLE_COOK) idx = SKILL_COOK;
        else if (role == ROLE_HELPER || role == ROLE_DISHWASHER) idx = SKILL_HELPER;
        else if (role == ROLE_CASHIER) idx = SKILL_CASHIER;

        for (int i = 0; i < n_staff; i++) {
            if (staff[i].skills[idx] > best_val) {
                best_val = staff[i].skills[idx];
                best_id = i;
            }
        }
    }
    return best_id;
}

// ========== DECISION LOGIC ==========

typedef struct {
    int my_id;
    role_t current_role;
    int assigned_table;
    int role_cycles;   // cycles spent in current role
} thread_data_t;

void release_role(thread_data_t *data) {
    // Only clear the slot if this thread still owns it.
    if (data->current_role == ROLE_HELPER && data->assigned_table >= 0) {
        if (g_board->tables[data->assigned_table].cleaner == data->my_id)
            g_board->tables[data->assigned_table].cleaner = -1;
    } else if (data->current_role == ROLE_WAITER && data->assigned_table >= 0) {
        if (g_board->tables[data->assigned_table].waiter == data->my_id)
            g_board->tables[data->assigned_table].waiter = -1;
    } else if (data->current_role == ROLE_COOK) {
        if (g_board->cook == data->my_id)
            g_board->cook = -1;
    } else if (data->current_role == ROLE_CASHIER) {
        if (g_board->cashier == data->my_id)
            g_board->cashier = -1;
    } else if (data->current_role == ROLE_DISHWASHER) {
        if (g_board->dishwasher == data->my_id)
            g_board->dishwasher = -1;
    }
    data->current_role = ROLE_NONE;
    data->assigned_table = -1;
}

int is_assignment_valid(thread_data_t *data) {
    // Ownership check first: if the blackboard no longer names this thread
    // for the slot, the role is invalid regardless of world state.
    if (data->current_role == ROLE_HELPER) {
        if (g_board->tables[data->assigned_table].cleaner != data->my_id) return 0;
        return g_dining->tables[data->assigned_table].state == TABLE_FREED;
    }
    if (data->current_role == ROLE_WAITER) {
        if (g_board->tables[data->assigned_table].waiter != data->my_id) return 0;
        if (g_dining->tables[data->assigned_table].state != TABLE_TAKEN) return 0;
        if (g_dining->tables[data->assigned_table].food_qty == LVL_NONE) return 1;
        if (g_kitchen->food_ready[data->assigned_table] == TR_TRUE) return 1;
        return 0;
    }
    if (data->current_role == ROLE_COOK) {
        if (g_board->cook != data->my_id) return 0;
        if (g_kitchen->pending_orders > 0) return 1;
        for (int i = 0; i < g_dining->tables_n; i++) {
            if (g_dining->tables[i].state == TABLE_TAKEN &&
                g_dining->tables[i].food_qty > LVL_NONE &&
                g_kitchen->food_ready[i] == TR_FALSE) {
                return 1;
            }
        }
        return 0;
    }
    if (data->current_role == ROLE_CASHIER) {
        if (g_board->cashier != data->my_id) return 0;
        return g_cash->pending_payments > 0;
    }
    if (data->current_role == ROLE_DISHWASHER) {
        if (g_board->dishwasher != data->my_id) return 0;
        return g_kitchen->dirty_plates > LVL_LOW;
    }
    return 0;
}

void assign_role(thread_data_t *data) {
    // Priority 1: Cashier
    if (g_cash->pending_payments > 0 && g_board->cashier == -1) {
        g_board->cashier = data->my_id;
        data->current_role = ROLE_CASHIER;
        return;
    }

    // Priority 2: Cook
    if (g_board->cook == -1) {
        int needs_cooking = (g_kitchen->pending_orders > 0);
        if (!needs_cooking) {
            for (int i = 0; i < g_dining->tables_n; i++) {
                if (g_dining->tables[i].state == TABLE_TAKEN &&
                    g_dining->tables[i].food_qty > LVL_NONE &&
                    g_kitchen->food_ready[i] == TR_FALSE) {
                    needs_cooking = 1;
                    break;
                }
            }
        }
        if (needs_cooking) {
            g_board->cook = data->my_id;
            data->current_role = ROLE_COOK;
            return;
        }
    }

    // Priority 3: Cleaning — skip tables we already gave up on
    for (int i = 0; i < g_dining->tables_n; i++) {
        if (g_dining->tables[i].state == TABLE_FREED &&
            g_board->tables[i].cleaner == -1 &&
            !(g_timeout_cleaned_mask & (1u << i))) {
            g_board->tables[i].cleaner = data->my_id;
            data->current_role = ROLE_HELPER;
            data->assigned_table = i;
            return;
        }
    }

    // Priority 4: Delivery
    for (int i = 0; i < g_dining->tables_n; i++) {
        if (g_dining->tables[i].state == TABLE_TAKEN &&
            g_dining->tables[i].food_qty > LVL_NONE &&
            g_kitchen->food_ready[i] == TR_TRUE &&
            g_board->tables[i].waiter == -1) {
            g_board->tables[i].waiter = data->my_id;
            data->current_role = ROLE_WAITER;
            data->assigned_table = i;
            return;
        }
    }

    // Priority 5: Ordering
    for (int i = 0; i < g_dining->tables_n; i++) {
        if (g_dining->tables[i].state == TABLE_TAKEN &&
            g_dining->tables[i].food_qty == LVL_NONE &&
            g_board->tables[i].waiter == -1) {
            g_board->tables[i].waiter = data->my_id;
            data->current_role = ROLE_WAITER;
            data->assigned_table = i;
            return;
        }
    }

    // Priority 6: Dishwasher
    if (g_kitchen->dirty_plates > LVL_LOW && g_board->dishwasher == -1) {
        g_board->dishwasher = data->my_id;
        data->current_role = ROLE_DISHWASHER;
        return;
    }
}

// ========== THREAD LOGIC ==========

void* staff_worker(void* arg) {
    thread_data_t *data = (thread_data_t*) arg;
    data->current_role = ROLE_NONE;
    data->assigned_table = -1;
    data->role_cycles = 0;

    while (instance_active) {
        lock_blackboard(global_sem_id);
        poll_fatigue_messages(g_qid_fatigue, data->my_id);

        // Clear "gave-up-cleaning" marks for tables that are no longer FREED,
        // so a fresh dirty table in a later pass can still be cleaned.
        for (int i = 0; i < g_dining->tables_n; i++) {
            if (g_dining->tables[i].state != TABLE_FREED) {
                g_timeout_cleaned_mask &= ~(1u << i);
            }
        }

        if (g_current_strategy == STRATEGY_REPUTATION && data->current_role != ROLE_NONE) {
            if (is_too_tired(data->my_id, data->current_role)) {
                release_role(data);
                data->role_cycles = 0;
                unlock_blackboard(global_sem_id);
                usleep(g_poll_us * 25);
                continue;
            }
        }

        if (data->current_role != ROLE_NONE) {
            data->role_cycles++;

            if (data->current_role == ROLE_HELPER && data->role_cycles > g_cleaner_cycles) {
                // Server finished cleaning but didn't transition state
                // (it waits for a new family to take the table).
                // Mark and release so we can serve pending deliveries.
                g_timeout_cleaned_mask |= (1u << data->assigned_table);
                release_role(data);
                data->role_cycles = 0;
            } else if (!is_assignment_valid(data)) {
                release_role(data);
                data->role_cycles = 0;
            }
        }

        if (data->current_role == ROLE_NONE) {
            assign_role(data);
            if (data->current_role != ROLE_NONE) {
                data->role_cycles = 0;
            }
        }

        unlock_blackboard(global_sem_id);
        usleep(g_poll_us);
    }
    return NULL;
}

// ========== MAIN ==========

int main(int argc, char **argv) {
    printf("Starting Trattoria Client...\n");

    strategy_t requested_strategy = STRATEGY_PROFIT;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--strategy") == 0 && i + 1 < argc) {
            if (strcmp(argv[i+1], "reputation") == 0) requested_strategy = STRATEGY_REPUTATION;
        }
    }

    int qid_c2s = msgget(ftok(TRATTORIA_FTOK_PATH, PROJ_MSG_C2S), 0666);
    int qid_s2c = msgget(ftok(TRATTORIA_FTOK_PATH, PROJ_MSG_S2C), 0666);
    int qid_fatigue = msgget(ftok(TRATTORIA_FTOK_PATH, PROJ_MSG_FATIGUE), 0666);

    if (qid_c2s == -1 || qid_s2c == -1 || qid_fatigue == -1) {
        perror("Queue connection failed. Is the server running?");
        exit(1);
    }
    g_qid_fatigue = qid_fatigue;

    int shm_dining_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_DININGROOM), sizeof(shm_diningroom_t), 0666);
    int shm_kitchen_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_KITCHEN), sizeof(shm_kitchen_t), 0666);
    int shm_cash_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_CASHDESK), sizeof(shm_cashdesk_t), 0666);
    int shm_blackboard_id = shmget(ftok(TRATTORIA_FTOK_PATH, PROJ_BLACKBOARD), sizeof(shm_blackboard_t), 0666);

    if (shm_dining_id == -1 || shm_kitchen_id == -1 || shm_cash_id == -1 || shm_blackboard_id == -1) {
        perror("Shared memory connection failed.");
        exit(1);
    }

    g_dining = (shm_diningroom_t *) shmat(shm_dining_id, NULL, 0);
    g_kitchen = (shm_kitchen_t *) shmat(shm_kitchen_id, NULL, 0);
    g_cash = (shm_cashdesk_t *) shmat(shm_cash_id, NULL, 0);
    g_board = (shm_blackboard_t *) shmat(shm_blackboard_id, NULL, 0);

    if (g_dining == (void *)-1 || g_kitchen == (void *)-1 || g_cash == (void *)-1 || g_board == (void *)-1) {
        perror("shmat failed);
            exit(1);
    }

    global_sem_id = semget(ftok(TRATTORIA_FTOK_PATH, PROJ_SEM), SEM_NSEMS, 0666);
    if (global_sem_id == -1) { perror("Semaphores failed"); exit(1); }

    init_fatigue_tracker();

    msg_hello_t hello_msg;
    memset(&hello_msg, 0, sizeof(hello_msg));
    hello_msg.mtype = MSGTYPE_HELLO;
    hello_msg.pid = getpid();
    hello_msg.studentid_n = 2;
    strncpy(hello_msg.studentids[0], "VR123456", STUDENTID_MAXLEN);
    strncpy(hello_msg.studentids[1], "VR654321", STUDENTID_MAXLEN);
    hello_msg.has_strategy = TR_TRUE;
    hello_msg.strategy = requested_strategy;

    if (msgsnd(qid_c2s, &hello_msg, sizeof(msg_hello_t) - sizeof(long), 0) == -1) {
        perror("send hello failed"); exit(1);
    }

    msg_welcome_t welcome_msg;
    if (msgrcv(qid_s2c, &welcome_msg, sizeof(msg_welcome_t) - sizeof(long), 0, 0) == -1) {
        perror("welcome failed"); exit(1);
    }

    if (welcome_msg.mtype != MSGTYPE_WELCOME) {
        fprint(stderr, "server didn't respond with WELCOME: check student IDs\n", welcome_msg.mtype);
        exit(1);
    }

    g_welcome_staff_n = welcome_msg.staff_n;
    memcpy(g_welcome_staff, welcome_msg.staff, sizeof(staff_member_t) * welcome_msg.staff_n);
    printf("Received WELCOME! Staff: %d | Tables: %d\n", welcome_msg.staff_n, welcome_msg.tables_n);

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
        if (msgrcv(qid_s2c, &s2c_msg, max_msg_size, 0, 0) == -1) break;

        if (s2c_msg.mtype == MSGTYPE_INSTANCE) {
            printf("\n[--- NEW ROUND STARTING (Strategy: %d) ---]\n", s2c_msg.instance.strategy);
            init_fatigue_tracker();
            g_timeout_cleaned_mask = 0;
            instance_active = 1;
            g_current_strategy = s2c_msg.instance.strategy;

            int speed = s2c_msg.instance.speed;
            if (speed < 1) speed = 1;
            g_poll_us = 2000 / speed;
            if (g_poll_us < 200) g_poll_us = 200;
            if (g_cleaner_cycles < 5) g_cleaner_cycles = 5;

            for (int i = 0; i < welcome_msg.staff_n; i++) {
                thread_data[i].my_id = i;
                pthread_create(&staff_threads[i], NULL, staff_worker, &thread_data[i]);
            }
        }
        else if (s2c_msg.mtype == MSGTYPE_INSTANCE_DONE) {
            printf("\n[--- ROUND FINISHED ---]\n");
            instance_active = 0;
            for (int i = 0; i < welcome_msg.staff_n; i++) {
                pthread_join(staff_threads[i], NULL);
            }
            printf("Avg Time: %.2f | Avg Review: %s\n", s2c_msg.done.total_families_time, s2c_msg.done.average_families_score_review);
        }
        else if (s2c_msg.mtype == MSGTYPE_ERROR) {
            fprint(stderr, ".../n);
                exit(1);
        }
    }

    shmdt(g_dining);
    shmdt(g_kitchen);
    shmdt(g_cash);
    shmdt(g_board);
    printf("Client exited cleanly.\n");
    return 0;
}
