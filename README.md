# Multithreaded Banking / ATM application

Minimal HTML frontend connected to a native **C / POSIX pthreads HTTP backend**.

**Live application:** https://pranav-banking-atm.onrender.com/

**Source repository:** https://github.com/pranavkoushik0606-code/banking-atm-os-assignment

Prepared for Pranav Koushik L, 24ETCS002142, CSC304A Operating Systems.

## Run locally (Linux, macOS with Linux container, or Windows WSL)

On Linux / WSL, install GCC and Make, then:

```sh
make
make run
```

Open **http://localhost:10000**. Keep the terminal running. Opening `index.html`
directly as a file does not start or connect the backend.

To change the port:

```sh
PORT=8080 ./build/banking_server public
```

The C server uses Linux `getrandom()` and Linux socket flags. Use WSL or Docker
on Windows/macOS. The pthread banking algorithms themselves are portable POSIX C.

## Run with Docker

```sh
docker build -t banking-atm .
docker run --rm -p 10000:10000 banking-atm
```

## What matches the uploaded PDF

The source was reconstructed from the code screenshots on pages 6–8, with the
workload and initial balances from pages 8–9. The PDF does not contain a complete
machine-readable `.c` source file: its initial includes and type definitions are
not shown. Those definitions and the HTTP adapter were added to make it runnable.

`src/banking_sim.c` retains the PDF's C functions and behavior:

- `unsafe_deposit`: unlocked read, `usleep(10000)`, then write; 10 concurrent
  deposits of ₹1,000 into an account initially containing ₹10,000.
- `safe_deposit`, `safe_withdraw`, `safe_enquiry`: per-account `pthread_mutex_t`.
- `safe_transfer`: lock the smaller account ID first, then the larger ID; release
  both after the operation. This removes circular lock waiting.
- `execute_transaction` and `create_task`: the same four-operation dispatcher
  and heap-allocated task ownership.
- A dedicated audit mutex keeps receipts intact across concurrent workers.
- The assignment scenario runs its original eight operations on accounts 101 and
  102, initially ₹50,000 each.

The account algorithms run in C, not JavaScript. JavaScript only handles forms,
HTTP requests and displaying the C responses. Added boundary validation rejects
invalid/non-positive amounts and self-transfers before calling the PDF functions.
The original `double` balances are retained; this is an academic simulation.

## Important scheduling detail in the original assignment

The PDF shows a final total of ₹93,000 when the ₹60,000 withdrawal is rejected.
That is a possible outcome, not a guarantee for its simultaneous workload.
If the ₹10,000 transfer credits account 102 before its outgoing transfers,
account 102 can hold ₹60,000; the original check `balance >= amount` then
correctly permits the withdrawal. Its subsequent outgoing transfers can fail,
and the final total becomes ₹33,000. The application preserves this behavior
and derives the expected total from whether ATM-03 succeeded. Thread ordering,
enquiry snapshots, and unsafe race results may vary between runs.

The intentionally unsafe demo contains a real C data race, as required by the
assignment. Its result is nondeterministic, and the C language does not guarantee
a specific outcome. It runs on isolated demo accounts. Safe transactions use
mutexes; the stress test does not deliberately hang the server in a deadlock.

The design discussion mentions Java APIs, a thread pool and read/write locks,
but the implementation screenshots use C, per-account mutexes and pthread workers.
This project follows that C implementation. The session read/write gate is only
an HTTP adapter safeguard for resetting a browser's accounts.

## Use the frontend

1. Select deposit, withdrawal, balance enquiry or fund transfer.
2. Select an account, a channel and an amount when required.
3. Execute the transaction. The account cards and native C audit receipts update.
4. Open another tab in the same browser for concurrent access to the same accounts.
5. Use the concurrency lab for isolated unsafe deposits, synchronized deposits,
   100 opposing transfers, or the PDF's eight-transaction workload.
6. Reset returns that browser's two accounts to ₹50,000 each.

Browser sessions are isolated by an HttpOnly, SameSite cookie. All balances are in
memory, matching the assignment. A server restart or new browser session creates
fresh balances. The teaching server supports 256 browser sessions per process
and 32 concurrent HTTP workers, with request size and socket timeout limits.

## Tests

```sh
make test
./build/banking_sim race
./build/banking_sim safe
./build/banking_sim transfer
./build/banking_sim assignment
```

Integration tests start the actual C server and verify the HTML connection,
all operations, overdraft rejection, 100 simultaneous deposits, 100 opposing
HTTP transfers, isolated demonstrations, session isolation and validation.

To check a deployed service:

```sh
BANK_TEST_BASE_URL=https://YOUR-SERVICE.onrender.com python3 tests/test_integration.py
```

Optional ThreadSanitizer on a compatible Linux host (safe phase only):

```sh
gcc -std=c11 -O1 -g -fsanitize=thread -DBANKING_CLI -pthread src/banking_sim.c -o banking_tsan
./banking_tsan safe
./banking_tsan transfer
```

Do not run the intentional `race` scenario when checking for zero race warnings.

## Free Render deployment

Use a GitHub/GitLab/Bitbucket repository containing these files. Create a Render
Web Service with runtime **Docker**, plan **Free**, Dockerfile `./Dockerfile`,
context `.`, region **Singapore**, and health check path `/health`.
The app listens on `0.0.0.0:$PORT`, defaulting to 10000.

The frontend and C backend are served from the same service URL. No separate
frontend host, database, Node.js server, Python server or paid service is needed.
Free services may sleep during inactivity; the first request can take longer.
An idle restart may reset the in-memory demonstration balances.

## Layout

| File | Purpose |
| --- | --- |
| `public/index.html` | Minimal frontend |
| `public/style.css` | Responsive styling |
| `public/app.js` | Calls the C HTTP endpoints |
| `src/banking_sim.c` | PDF banking logic and real pthread demonstrations |
| `src/banking.h` | Shared declarations |
| `src/server.c` | Native C HTTP and browser-session adapter |
| `Dockerfile` | Native compilation and Render container |
| `Makefile` | Compile, run and test |
| `tests/test_integration.py` | End-to-end checks of the C server |

## API

GET `/health` and `/api/state`. POST `/api/transaction`, `/api/demo` and
`/api/reset` with `X-Bank-Request: 1` and URL-encoded form bodies.
Transaction fields: `operation`, `account`, `target` (transfer only), `amount`
(except enquiry), and `channel`. Demo field: `kind` (`race`, `safe`, `transfer`,
`assignment`). Keep the session cookie returned by `/api/state`.
