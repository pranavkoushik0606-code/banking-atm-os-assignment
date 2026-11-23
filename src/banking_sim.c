#define _DEFAULT_SOURCE
#include "banking.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
_Thread_local Audit *current_audit;
void audit_init(Audit *a) { a->used = 0; a->text[0] = '\0'; pthread_mutex_init(&a->lock, NULL); }
void bank_printf(const char *format, ...) {
    char line[1024];
    va_list args; va_start(args, format); vsnprintf(line, sizeof(line), format, args); va_end(args);
    if (current_audit) {
        pthread_mutex_lock(&current_audit->lock);
        size_t len = strlen(line), room = sizeof(current_audit->text) - current_audit->used - 1;
        if (len > room) len = room;
        memcpy(current_audit->text + current_audit->used, line, len);
        current_audit->used += len; current_audit->text[current_audit->used] = '\0';
        pthread_mutex_unlock(&current_audit->lock);
    }
}
/* Route the PDF's printf receipts to the HTTP response without changing the
 * account algorithms. Original function names, doubles, usleep, mutexes and
 * ascending-ID transfer lock order are retained. */
#define printf bank_printf

void init_account(BankAccount *acc, int id, double balance) {
    acc->account_id = id; acc->balance = balance; pthread_mutex_init(&acc->lock, NULL);
}

// 1. UNSAFE DEPOSIT: Demonstrates Race Condition (Lost Update)
void *unsafe_deposit(void *arg) {
    TransactionTask *task = (TransactionTask *)arg;
    double temp = task->primary_acc->balance;
    usleep(10000); // Simulate OS context switch inside critical section
    task->primary_acc->balance = temp + task->amount;
    free(task);
    return NULL;
}

// 2. SAFE DEPOSIT: Protected by Per-Account Mutex
void safe_deposit(BankAccount *acc, double amount, const char *channel) {
    pthread_mutex_lock(&acc->lock);
    double old_bal = acc->balance;
    acc->balance += amount;
    pthread_mutex_lock(&log_lock);
    printf("[%s] DEPOSIT | Acc-%d | +₹%.2f | Old: ₹%.2f -> New: ₹%.2f\n",
           channel, acc->account_id, amount, old_bal, acc->balance);
    pthread_mutex_unlock(&log_lock);
    pthread_mutex_unlock(&acc->lock);
}

// 3. SAFE WITHDRAWAL: Atomic Check-Then-Act prevents Overdraft
void safe_withdraw(BankAccount *acc, double amount, const char *channel) {
    pthread_mutex_lock(&acc->lock);
    if (acc->balance >= amount) {
        double old_bal = acc->balance;
        acc->balance -= amount;
        pthread_mutex_lock(&log_lock);
        printf("[%s] WITHDRAW | Acc-%d | -₹%.2f | Old: ₹%.2f -> New: ₹%.2f\n",
               channel, acc->account_id, amount, old_bal, acc->balance);
        pthread_mutex_unlock(&log_lock);
    } else {
        pthread_mutex_lock(&log_lock);
        printf("[%s] REJECTED | Acc-%d | Insufficient Funds (Req: ₹%.2f, Avail: ₹%.2f)\n",
               channel, acc->account_id, amount, acc->balance);
        pthread_mutex_unlock(&log_lock);
    }
    pthread_mutex_unlock(&acc->lock);
}

// 4. SAFE BALANCE ENQUIRY: Consistent Snapshot Read
void safe_enquiry(BankAccount *acc, const char *channel) {
    pthread_mutex_lock(&acc->lock);
    double current_bal = acc->balance;
    pthread_mutex_lock(&log_lock);
    printf("[%s] ENQUIRY | Acc-%d | Current Balance: ₹%.2f\n",
           channel, acc->account_id, current_bal);
    pthread_mutex_unlock(&log_lock);
    pthread_mutex_unlock(&acc->lock);
}

// 5. DEADLOCK-FREE FUND TRANSFER: Strict Global Lock Ordering
void safe_transfer(BankAccount *from, BankAccount *to, double amount, const char *channel) {
    // HTTP layer rejects a same-account transfer before calling this function.
    BankAccount *first = (from->account_id < to->account_id) ? from : to;
    BankAccount *second = (from->account_id < to->account_id) ? to : from;
    pthread_mutex_lock(&first->lock);
    pthread_mutex_lock(&second->lock);
    if (from->balance >= amount) {
        from->balance -= amount;
        to->balance += amount;
        pthread_mutex_lock(&log_lock);
        printf("[%s] TRANSFER | Acc-%d -> Acc-%d | ₹%.2f | New Balances: Acc-%d=₹%.2f, Acc-%d=₹%.2f\n",
               channel, from->account_id, to->account_id, amount,
               from->account_id, from->balance, to->account_id, to->balance);
        pthread_mutex_unlock(&log_lock);
    } else {
        pthread_mutex_lock(&log_lock);
        printf("[%s] TRANSFER FAILED | Acc-%d -> Acc-%d | Insufficient Funds in Acc-%d\n",
               channel, from->account_id, to->account_id, from->account_id);
        pthread_mutex_unlock(&log_lock);
    }
    pthread_mutex_unlock(&second->lock);
    pthread_mutex_unlock(&first->lock);
}

// Thread Worker Dispatcher
void *execute_transaction(void *arg) {
    TransactionTask *task = (TransactionTask *)arg;
    current_audit = task->audit; // Web adapter: capture this worker's receipts.
    switch (task->tx_type) {
        case 1: safe_deposit(task->primary_acc, task->amount, task->channel); break;
        case 2: safe_withdraw(task->primary_acc, task->amount, task->channel); break;
        case 3: safe_enquiry(task->primary_acc, task->channel); break;
        case 4: safe_transfer(task->primary_acc, task->secondary_acc, task->amount, task->channel); break;
    }
    free(task);
    current_audit = NULL;
    return NULL;
}

TransactionTask *create_task(BankAccount *p, BankAccount *s, double amt, const char *ch, int type) {
    TransactionTask *t = (TransactionTask *)malloc(sizeof(TransactionTask));
    if (!t) return NULL;
    t->primary_acc = p; t->secondary_acc = s; t->amount = amt;
    strncpy(t->channel, ch, sizeof(t->channel) - 1);
    t->channel[sizeof(t->channel) - 1] = '\0';
    t->tx_type = type; t->audit = NULL;
    return t;
}

/* Isolated demonstrations use exactly the workload shown in the PDF. */
int run_batch(BankAccount *a, BankAccount *b, const char *kind, Audit *audit, double *expected, double *actual) {
    pthread_t workers[100]; int count = 0, failed = 0;
    int race = !strcmp(kind, "race"), safe = !strcmp(kind, "safe");
    int transfer = !strcmp(kind, "transfer");
    int n = (race || safe) ? 10 : transfer ? 100 : 8;
    current_audit = audit;
    printf("%s\n", race ? "PHASE 1: UNSYNCHRONIZED DEPOSITS" : safe ? "SYNCHRONIZED DEPOSITS" : transfer ? "BIDIRECTIONAL TRANSFER STRESS TEST" : "PHASE 2: SYNCHRONIZED & DEADLOCK-FREE CONCURRENT BANKING");
    for (int i = 0; i < n; i++) {
        TransactionTask *t;
        if (race || safe) t = create_task(a, NULL, 1000.0, race ? "ATM-Unsafe" : "ATM-Safe", 1);
        else if (transfer) t = create_task(i % 2 ? b : a, i % 2 ? a : b, 100.0, i % 2 ? "ATM-02" : "Mobile-UPI", 4);
        else {
            static const int types[8] = {1,2,4,4,3,2,4,3};
            static const double amounts[8] = {5000,12000,10000,15000,0,60000,5000,0};
            static const char *channels[8] = {"ATM-01","Online-Web","Mobile-UPI","ATM-02","Online-App","ATM-03","Online-NEFT","ATM-04"};
            BankAccount *p = (i == 3 || i >= 5) ? b : a;
            t = create_task(p, types[i] == 4 ? (p == a ? b : a) : NULL, amounts[i], channels[i], types[i]);
        }
        if (!t) { failed = 1; break; }
        t->audit = audit;
        if (pthread_create(&workers[count], NULL, race ? unsafe_deposit : execute_transaction, t)) {
            free(t); failed = 1; break;
        }
        count++;
    }
    for (int i = 0; i < count; i++) pthread_join(workers[i], NULL);
    *expected = race || safe ? 20000.0 : transfer ? 100000.0 : 93000.0;
    /* With the PDF's genuinely concurrent workload, ATM-03 can succeed if
     * Mobile-UPI credits 102 before its two outgoing transfers. */
    if (!race && !safe && !transfer && !strstr(audit->text, "[ATM-03] REJECTED")) *expected = 33000.0;
    *actual = race || safe ? a->balance : a->balance + b->balance;
    printf("\nFINAL RECONCILED LEDGER\nAccount %d: ₹%.2f\n", a->account_id, a->balance);
    if (!race && !safe) printf("Account %d: ₹%.2f\n", b->account_id, b->balance);
    printf("Expected: ₹%.2f | Actual: ₹%.2f\n", *expected, *actual);
    current_audit = NULL;
    return failed ? -1 : 0;
}

#ifdef BANKING_CLI
int main(int argc, char **argv) {
    const char *kind = argc > 1 ? argv[1] : "assignment";
    if (strcmp(kind,"race") && strcmp(kind,"safe") && strcmp(kind,"transfer") && strcmp(kind,"assignment")) return 2;
    BankAccount a, b; Audit audit; double expected, actual;
    init_account(&a, !strcmp(kind,"race") || !strcmp(kind,"safe") ? 100 : 101,
                 !strcmp(kind,"race") || !strcmp(kind,"safe") ? 10000.0 : 50000.0);
    init_account(&b, 102, 50000.0); audit_init(&audit);
    int result = run_batch(&a, &b, kind, &audit, &expected, &actual);
    fputs(audit.text, stdout);
    pthread_mutex_destroy(&a.lock); pthread_mutex_destroy(&b.lock); pthread_mutex_destroy(&audit.lock);
    return result ? 1 : 0;
}
#endif
