#pragma once

#include "scenario.h"

#include <sys/types.h> // pid_t

// ---- Boolean type (uniform across messages + SHM) ----
// Fixed-width for stable shared-memory / message layouts.
typedef uint8_t tr_bool_t;
#define TR_FALSE ((tr_bool_t)0)
#define TR_TRUE  ((tr_bool_t)1)

// ---- Project key prefix ----

#define TRATTORIA_FTOK_PATH "/tmp/trattoria_ipc_key"

// ---- Project key suffixes ----

// Admin message queue from client to server
#define PROJ_MSG_C2S      0x41
// Admin message queue from server to client
#define PROJ_MSG_S2C      0x42
// Dedicated message queue for fatigue notifications (server -> client)
#define PROJ_MSG_FATIGUE  0x48
// Shared memories
#define PROJ_DININGROOM   0x43
#define PROJ_KITCHEN      0x44
#define PROJ_BLACKBOARD   0x45
// Shared memory: Cash desk (read-only view for client)
#define PROJ_CASHDESK     0x49
#define PROJ_SEM          0x47

// The semaphore set contains additional internal semaphores reserved for the server.
// Clients must still use SEM_NSEMS when attaching to the set.
#define SEM_NSEMS      4
// The only index that clients need
#define SEMIDX_BLACKBOARD 0

// ---- Message queue message types ----

#define MSGTYPE_HELLO   1
#define MSGTYPE_WELCOME 2
#define MSGTYPE_END     3
#define MSGTYPE_ERROR   4
#define MSGTYPE_INSTANCE 5
#define MSGTYPE_INSTANCE_DONE 6

// ---- Messages (wire formats) ----

typedef struct {
  long mtype;                 // MSGTYPE_HELLO
  pid_t pid;
  int studentid_n;            // 1..STUDENTID_MAX
  char studentids[STUDENTID_MAX][STUDENTID_MAXLEN];
  tr_bool_t has_strategy;     // TR_FALSE/TR_TRUE
  strategy_t strategy;        // meaningful only if has_strategy==TR_TRUE
} msg_hello_t;

typedef struct {
  char name[MAX_NAME];
  // Staff parameters as confidence buckets (server never sends exact numeric values).
  // Abilities (skills): waiter, cook, helper, cashier.
  param_bucket_t skills[NUM_SKILLS];
  // Traits: patience, sociability, professionalism, resilience.
  param_bucket_t traits[NUM_TRAITS];
} staff_member_t;

typedef struct {
  long mtype;                  // MSGTYPE_WELCOME
  int staff_n;
  int tables_n;
  tr_bool_t verify_mode;       
  strategy_t imposed_strategy; // 0=none, 1=profit, 2=reputation
  // Group identifier (derived from the sorted student IDs).
  // Does not change across instances.
  char group[MAX_GROUP];

  staff_member_t staff[MAX_STAFF];
} msg_welcome_t;

typedef struct {
  long mtype;               // MSGTYPE_ERROR
  int code;                 // 0 generic, 1 strategy_not_allowed, ...
  char message[128];
} msg_error_t;

// A server-defined "instance" of the simulation.
// In verify mode the server will send multiple instances over time.
typedef struct {
  long mtype;               // MSGTYPE_INSTANCE
  int instance_id;
  strategy_t strategy;
  int speed;
  int families_n;
} msg_instance_t;

typedef struct {
  long mtype;               // MSGTYPE_INSTANCE_DONE
  int instance_id;
  double total_families_time;
  char average_families_score_review[32];
} msg_instance_done_t;

typedef struct {
  long mtype;                 // MSGTYPE_END
  int reason;                 // 0=normal, 1=timeout, ...
} msg_end_t;

// --- Message Queue; Fatigue notification (server -> client) ---
// Demultiplexing: mtype = staff_id + 1 (SysV requires mtype > 0).
typedef struct {
  long mtype;        // staff_id + 1
  int staff_id;      
  role_t role;       // role whose perceived fatigue increased
  level_t new_lvl;
} msg_fatigue_t;

// ---- Shared Memory: Dining Room (read-only view for client) ----

typedef struct {
  table_state_t state;
  char surname[MAX_SURNAME];
  level_t dirt_level;   // visible after eating
  level_t food_qty;     // visible after order
} table_t;

typedef struct {
  int tables_n;
  table_t tables[MAX_TABLES];
} shm_diningroom_t;

// ---- Shared Memory: Kitchen (read-only view for client) ----
typedef struct {
  int tables_n;
  tr_bool_t food_ready[MAX_TABLES];
  int pending_orders;
  level_t clean_plates;             
  level_t dirty_plates;
} shm_kitchen_t;

// ---- Shared Memory: Blackboard (RW with mutex/sem, but client may read-only) ----

typedef struct {
  int waiter;   // staff id or -1
  int cleaner;  // staff id or -1
} table_assigment_t;

typedef struct {
  int tables_n;
  table_assigment_t tables[MAX_TABLES];
  int cook;       // staff id or -1
  int cashier;    // staff id or -1
  int dishwasher; // staff id or -1
} shm_blackboard_t;

// ---- Shared Memory: Cash Desk (read-only view for client) ----
// Exposes the number of pending payments currently queued for the common cash desk.
// This SHM is updated ONLY by the server cash-desk process.
typedef struct {
  int pending_payments;
} shm_cashdesk_t;



/*PARTE DI CODICE DI KHADY*/
#include <stdio.h>
#include <string.h>
#include <sys/msg.h>
#include <errno.h>

/* DEFINIZIONI DI SUPPORTO
 * Riprodotte qui per rendere il file compilabile stand-alone.
 * Nel progetto reale queste vengono da scenario.h / ipc.h. */

/* Livelli qualitativi usati sia per abilità/tratti che per stanchezza */
typedef enum {
    LEVEL_LOW    = 0,
    LEVEL_MEDIUM = 1,
    LEVEL_HIGH   = 2
} Level;

/* Ruoli del personale */
typedef enum {
    ROLE_WAITER    = 0,   /* Cameriere  */
    ROLE_HELPER    = 1,   /* Aiutante   */
    ROLE_COOK      = 2,   /* Cuoco      */
    ROLE_CASHIER   = 3,   /* Cassiere   */
    NUM_ROLES      = 4
} Role;

/* Fasi in cui si trova un tavolo */
typedef enum {
    TABLE_EMPTY    = 0,
    TABLE_DIRTY    = 1,   /* Attende pulizia             */
    TABLE_ORDERING = 2,   /* Attende ordine cameriere    */
    TABLE_WAITING  = 3,   /* Attende preparazione cibo   */
    TABLE_SERVING  = 4,   /* Cibo pronto, attende consegna */
    TABLE_EATING   = 5,   /* Famiglia sta consumando      */
    TABLE_PAYING   = 6    /* Attende cassa                */
} TableState;

/* Struttura per un singolo tavolo nella memoria condivisa sala */
typedef struct {
    TableState state;
    Level      dirt_level;  /* sporcizia lasciata dalla famiglia */
    Level      food_level;  /* quantità cibo ordinata / da consegnare */
} TableStatus;

/* Memoria condivisa sala: array di N_TABLES tavoli */
typedef struct {
    int         n_tables;
    TableStatus tables[32];  /* dimensione massima ragionevole */
} SharedRoom;

/* Memoria condivisa cucina */
typedef struct {
    Level clean_plates;    /* quantità qualitativa piatti puliti  */
    Level dirty_plates;    /* quantità qualitativa piatti sporchi */
    int   pending_orders;  /* numero ordini ancora da cucinare    */
    int   ready_table[32]; /* 1 se il cibo del tavolo i è pronto  */
} SharedKitchen;

/* Struttura membro del personale (ricevuta nel messaggio di benvenuto) */
typedef struct {
    int  id;
    /* Abilità */
    Level skill_waiter;
    Level skill_helper;
    Level skill_cook;
    Level skill_cashier;
    /* Tratti */
    Level trait_patience;
    Level trait_sociability;
    Level trait_professionalism;
    Level trait_stamina;          /* resistenza */
} StaffMember;

/* Messaggio IPC per notifica stanchezza */
typedef struct {
    long mtype;      /* id_personale + 1  (deve essere > 0) */
    int  staff_id;   /* indice del membro (0-based)          */
    Role role;       /* ruolo in cui è aumentata la stanchezza */
    Level tiredness; /* nuovo livello qualitativo             */
} FatigueMessage;


/* TASK 1 – LETTORI DI STATO (State Readers) */

/*
 * count_tables_by_state()
 * Conta quanti tavoli si trovano in un determinato stato.
 *
 * Parametri:
 *   room        - puntatore alla memoria condivisa sala (sola lettura)
 *   target      - lo stato che si vuole contare
 *
 * Ritorna:
 *   Numero di tavoli nello stato richiesto.
 */
int count_tables_by_state(const SharedRoom *room, TableState target)
{
    int count = 0;
    for (int i = 0; i < room->n_tables; i++) {
        if (room->tables[i].state == target)
            count++;
    }
    return count;
}

/*
 * count_dirty_tables()
 * Wrapper conveniente: quanti tavoli attendono pulizia?
 */
int count_dirty_tables(const SharedRoom *room)
{
    return count_tables_by_state(room, TABLE_DIRTY);
}

/*
 * count_ordering_tables()
 * Quanti tavoli stanno aspettando un cameriere per l'ordine?
 */
int count_ordering_tables(const SharedRoom *room)
{
    return count_tables_by_state(room, TABLE_ORDERING);
}

/*
 * count_serving_tables()
 * Quanti tavoli hanno il cibo pronto ma non ancora consegnato?
 */
int count_serving_tables(const SharedRoom *room)
{
    return count_tables_by_state(room, TABLE_SERVING);
}

/*
 * get_pending_orders()
 * Quanti ordini sono in coda in cucina, ancora da preparare?
 *
 * Parametri:
 *   kitchen - puntatore alla memoria condivisa cucina (sola lettura)
 *
 * Ritorna:
 *   Numero intero di ordini pendenti.
 */
int get_pending_orders(const SharedKitchen *kitchen)
{
    return kitchen->pending_orders;
}

/* get_dirty_plates_level()
 * Livello qualitativo (LOW/MEDIUM/HIGH) di piatti sporchi in cucina. */
Level get_dirty_plates_level(const SharedKitchen *kitchen)
{
    return kitchen->dirty_plates;
}

/*
 * get_clean_plates_level()
 * Livello qualitativo di piatti puliti disponibili.
 */
Level get_clean_plates_level(const SharedKitchen *kitchen)
{
    return kitchen->clean_plates;
}

/*
 * is_food_ready_for_table()
 * Il cibo del tavolo con indice table_idx è pronto per la consegna?
 *
 * Ritorna 1 (vero) oppure 0 (falso).
 */
int is_food_ready_for_table(const SharedKitchen *kitchen, int table_idx)
{
    return kitchen->ready_table[table_idx];
}


/* TASK 2 – PROFILAZIONE DEL PERSONALE (Staff Profiling) */

/*
 * best_staff_for_role()
 * Trova il membro del personale con l'abilità più alta per il ruolo dato.
 *
 * In caso di parità viene restituito il primo trovato (indice più basso).
 * La funzione legge il campo abilità corretto in base al parametro role.
 *
 * Parametri:
 *   staff      - array di StaffMember ricevuto dal server
 *   n_staff    - numero totale di membri del personale
 *   role       - ruolo per cui trovare il migliore
 *
 * Ritorna:
 *   L'id del membro più adatto, oppure -1 se l'array è vuoto.
 */
int best_staff_for_role(const StaffMember *staff, int n_staff, Role role)
{
    if (n_staff <= 0) return -1;

    /* Inizializza con il primo membro come riferimento */
    int best_id = staff[0].id;
    Level best_level;
    switch (role) {
        case ROLE_WAITER:  best_level = staff[0].skill_waiter;  break;
        case ROLE_HELPER:  best_level = staff[0].skill_helper;  break;
        case ROLE_COOK:    best_level = staff[0].skill_cook;    break;
        case ROLE_CASHIER: best_level = staff[0].skill_cashier; break;
        default:           best_level = LEVEL_LOW;              break;
    }

    /* Confronta con tutti gli altri, partendo dal secondo */
    for (int i = 1; i < n_staff; i++) {
        Level current;

        switch (role) {
            case ROLE_WAITER:   current = staff[i].skill_waiter;   break;
            case ROLE_HELPER:   current = staff[i].skill_helper;   break;
            case ROLE_COOK:     current = staff[i].skill_cook;     break;
            case ROLE_CASHIER:  current = staff[i].skill_cashier;  break;
            default:            current = LEVEL_LOW;               break;
        }

        if (current > best_level) {
            best_level = current;
            best_id    = staff[i].id;
        }
    }

    return best_id;
}

/*
 * best_cook()   – Wrapper: chi è il cuoco migliore?
 */
int best_cook(const StaffMember *staff, int n_staff)
{
    return best_staff_for_role(staff, n_staff, ROLE_COOK);
}

/*
 * best_cashier() – Wrapper: chi è il cassiere migliore?
 */
int best_cashier(const StaffMember *staff, int n_staff)
{
    return best_staff_for_role(staff, n_staff, ROLE_CASHIER);
}

/*
 * best_helper() – Wrapper: chi è il miglior aiutante (pulizie/piatti)?
 */
int best_helper(const StaffMember *staff, int n_staff)
{
    return best_staff_for_role(staff, n_staff, ROLE_HELPER);
}

/*
 * best_waiter() – Wrapper: chi è il miglior cameriere?
 */
int best_waiter(const StaffMember *staff, int n_staff)
{
    return best_staff_for_role(staff, n_staff, ROLE_WAITER);
}


/* TASK 3 – MONITOR DELLA STANCHEZZA (Fatigue Monitor) */

#define MAX_STAFF 16   /* dimensione massima ragionevole per il roster */

/*
 * fatigue_tracker[staff_id][role] = livello attuale di stanchezza.
 *
 * È una griglia 2D: righe = ID membro del personale, colonne = ruolo.
 * Inizializzata a LEVEL_LOW (0) all'avvio.
 */
static Level fatigue_tracker[MAX_STAFF][NUM_ROLES];

/*
 * init_fatigue_tracker()
 * Azzera la griglia: tutti partono senza stanchezza.
 * Va chiamata una volta sola all'inizio di ogni istanza.
 */
void init_fatigue_tracker(void)
{
    memset(fatigue_tracker, LEVEL_LOW, sizeof(fatigue_tracker));
}

/*
 * poll_fatigue_messages()
 * Legge TUTTI i messaggi di stanchezza disponibili nella coda IPC
 * e aggiorna la griglia fatigue_tracker.
 *
 * Usa IPC_NOWAIT: se la coda è vuota, non blocca il thread e ritorna.
 * Deve essere chiamata periodicamente dal filo di ogni membro del personale
 * passando il proprio staff_id come filtro sul tipo di messaggio.
 *
 * Parametri:
 *   msqid    - id della coda di messaggi IPC (ottenuto con msgget)
 *   staff_id - id del membro del personale (0-based); il filo legge solo
 *              i messaggi con mtype == staff_id + 1
 *
 * Ritorna:
 *   Numero di messaggi letti (0 se la coda era vuota).
 *
 * Nota sul protocollo (slide 19):
 *   mtype = staff_id + 1  (perché mtype deve essere > 0)
 */
int poll_fatigue_messages(int msqid, int staff_id)
{
    FatigueMessage msg;
    int count = 0;
    long mtype_filter = (long)(staff_id + 1);  /* vedi slide 19 */

    while (1) {
        ssize_t ret = msgrcv(
            msqid,
            &msg,
            sizeof(FatigueMessage) - sizeof(long),  /* dimensione senza mtype */
            mtype_filter,
            IPC_NOWAIT   /* non bloccare se non ci sono messaggi */
        );

        if (ret == -1) {
            /* ENOMSG / EAGAIN = coda vuota per questo mtype: uscita normale */
            if (errno == ENOMSG || errno == EAGAIN)
                break;
            /* Altro errore: esci comunque senza crashare */
            perror("poll_fatigue_messages: msgrcv");
            break;
        }

        /* Messaggio valido: aggiorna la griglia */
        if (msg.staff_id >= 0 && msg.staff_id < MAX_STAFF &&
            msg.role     >= 0 && msg.role     < NUM_ROLES) {
            fatigue_tracker[msg.staff_id][msg.role] = msg.tiredness;
        }
        count++;
    }

    return count;
}

/*
 * get_fatigue()
 * Legge il livello di stanchezza corrente per un dato membro e ruolo.
 *
 * Ritorna:
 *   Level (LEVEL_LOW / LEVEL_MEDIUM / LEVEL_HIGH)
 */
Level get_fatigue(int staff_id, Role role)
{
    if (staff_id < 0 || staff_id >= MAX_STAFF) return LEVEL_LOW;
    if (role     < 0 || role     >= NUM_ROLES)  return LEVEL_LOW;
    return fatigue_tracker[staff_id][role];
}

/*
 * is_too_tired()
 * Controlla se un membro è troppo stanco in un dato ruolo (livello ALTO).
 * Utile per decidere se ruotare il personale prima di sovraccaricarli.
 *
 * Ritorna 1 (troppo stanco) oppure 0.
 */
int is_too_tired(int staff_id, Role role)
{
    return get_fatigue(staff_id, role) == LEVEL_HIGH;
}


/* MAIN DI TEST (rimovibile nella versione finale del client) */
int main(void)
{
    printf("=== Test Task 1: State Readers ===\n");

    /* Costruiamo una sala fittizia con 5 tavoli */
    SharedRoom room = {0};
    room.n_tables = 5;
    room.tables[0].state = TABLE_DIRTY;
    room.tables[1].state = TABLE_ORDERING;
    room.tables[2].state = TABLE_DIRTY;
    room.tables[3].state = TABLE_EATING;
    room.tables[4].state = TABLE_SERVING;

    printf("Tavoli sporchi:            %d (atteso 2)\n", count_dirty_tables(&room));
    printf("Tavoli in fase ordine:     %d (atteso 1)\n", count_ordering_tables(&room));
    printf("Tavoli pronti da servire:  %d (atteso 1)\n", count_serving_tables(&room));

    SharedKitchen kitchen = {0};
    kitchen.pending_orders = 3;
    kitchen.dirty_plates   = LEVEL_HIGH;
    kitchen.clean_plates   = LEVEL_LOW;
    kitchen.ready_table[4] = 1;

    printf("Ordini pendenti:           %d (atteso 3)\n", get_pending_orders(&kitchen));
    printf("Piatti sporchi (livello):  %d (atteso 2=HIGH)\n", get_dirty_plates_level(&kitchen));
    printf("Cibo pronto tavolo 4:      %d (atteso 1)\n",   is_food_ready_for_table(&kitchen, 4));
    printf("Cibo pronto tavolo 2:      %d (atteso 0)\n",   is_food_ready_for_table(&kitchen, 2));

    printf("\n=== Test Task 2: Staff Profiling ===\n");

    StaffMember staff[3] = {
        { .id=0, .skill_waiter=LEVEL_LOW,  .skill_helper=LEVEL_HIGH,
                 .skill_cook=LEVEL_MEDIUM, .skill_cashier=LEVEL_LOW  },
        { .id=1, .skill_waiter=LEVEL_HIGH, .skill_helper=LEVEL_LOW,
                 .skill_cook=LEVEL_LOW,    .skill_cashier=LEVEL_MEDIUM },
        { .id=2, .skill_waiter=LEVEL_MEDIUM,.skill_helper=LEVEL_MEDIUM,
                 .skill_cook=LEVEL_HIGH,   .skill_cashier=LEVEL_HIGH  }
    };

    printf("Miglior cuoco:     id=%d (atteso 2)\n", best_cook(staff, 3));
    printf("Miglior cameriere: id=%d (atteso 1)\n", best_waiter(staff, 3));
    printf("Miglior aiutante:  id=%d (atteso 0)\n", best_helper(staff, 3));
    printf("Miglior cassiere:  id=%d (atteso 2)\n", best_cashier(staff, 3));

    printf("\n=== Test Task 3: Fatigue Monitor ===\n");

    init_fatigue_tracker();
    printf("Stanchezza iniziale staff 0, COOK: %d (atteso 0=LOW)\n",
           get_fatigue(0, ROLE_COOK));

    /* Simuliamo un aggiornamento manuale come se fosse arrivato da msgrcv */
    fatigue_tracker[0][ROLE_COOK]    = LEVEL_MEDIUM;
    fatigue_tracker[1][ROLE_WAITER]  = LEVEL_HIGH;

    printf("Stanchezza staff 0, COOK:    %d (atteso 1=MEDIUM)\n",
           get_fatigue(0, ROLE_COOK));
    printf("Staff 1 troppo stanco come cameriere? %d (atteso 1)\n",
           is_too_tired(1, ROLE_WAITER));
    printf("Staff 0 troppo stanco come cuoco?     %d (atteso 0)\n",
           is_too_tired(0, ROLE_COOK));

    printf("\nTutti i test completati.\n");
    return 0;
} */
