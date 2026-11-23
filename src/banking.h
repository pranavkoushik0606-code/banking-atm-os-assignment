#ifndef BANKING_H
#define BANKING_H
#include <pthread.h>
#include <stddef.h>

typedef struct { int account_id; double balance; pthread_mutex_t lock; } BankAccount;
typedef struct { char text[32768]; size_t used; pthread_mutex_t lock; } Audit;
typedef struct {
    BankAccount *primary_acc, *secondary_acc;
    double amount;
    char channel[32];
    int tx_type;
    Audit *audit;
} TransactionTask;
extern pthread_mutex_t log_lock;
extern _Thread_local Audit *current_audit;
void audit_init(Audit *a);
void bank_printf(const char *format, ...);
void init_account(BankAccount *acc, int id, double balance);
void *unsafe_deposit(void *arg);
void safe_deposit(BankAccount *acc, double amount, const char *channel);
void safe_withdraw(BankAccount *acc, double amount, const char *channel);
void safe_enquiry(BankAccount *acc, const char *channel);
void safe_transfer(BankAccount *from, BankAccount *to, double amount, const char *channel);
void *execute_transaction(void *arg);
TransactionTask *create_task(BankAccount *p, BankAccount *s, double amt, const char *ch, int type);
int run_batch(BankAccount *a, BankAccount *b, const char *kind, Audit *audit, double *expected, double *actual);
#endif
