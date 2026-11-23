# Validation results

Validated on Linux with GCC and real POSIX pthread workers on 23 November 2026.

| Check | Observed result |
| --- | --- |
| Native compile, `-Wall -Wextra -Wpedantic` | Passed without compiler warnings |
| Six integration test groups | All passed |
| 100 concurrent HTTP deposits of ₹100 | ₹50,000 became ₹60,000; zero lost updates |
| 100 concurrent opposing HTTP transfers | Both accounts returned to ₹50,000; no hangs |
| Overdraft request, ₹60,000 against ₹50,000 | Rejected with balance unchanged |
| 10 synchronized C deposits | ₹10,000 became ₹20,000 |
| 10 deliberately unsafe C deposits, sample run | ₹11,000 observed, versus ₹20,000 expected |
| PDF assignment workload, sample run | Account 101: ₹53,000; account 102: ₹40,000; total ₹93,000 |
| GCC ThreadSanitizer, synchronized deposits | Exit 0; no warnings |
| GCC ThreadSanitizer, 100 opposing transfers | Exit 0; no warnings |
| JavaScript syntax, `node --check` | Passed |
| Live browser, HTML to C deposit | Account 101 updated from ₹50,000 to ₹51,000 |
| Live browser, synchronized deposit demonstration | PASS; expected and actual ₹20,000 |
| Render Docker build and deployment | Successful; service status `live` |
| Public HTTPS `/health` | HTTP 200; backend identifies C / POSIX pthreads |
| Public browser, HTML to native C deposit | Account 101 updated from ₹50,000 to ₹51,000 |
| Public browser, synchronized C demonstration | PASS; expected and actual ₹20,000 |

The intentional race is excluded from the zero-race sanitizer checks. Its result
can vary. See README.md for the assignment workload's scheduling-dependent
overdraft outcome. Docker is not installed in this workspace; the container was
successfully built and run by Render. Native compilation and the actual C HTTP
server were also verified locally.
