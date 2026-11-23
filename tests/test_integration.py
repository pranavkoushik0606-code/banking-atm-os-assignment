"""Exercise the actual native C server, including contention over HTTP."""
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import unittest
from urllib.parse import urlencode

ROOT = Path(__file__).resolve().parents[1]


class BankingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.external = os.environ.get("BANK_TEST_BASE_URL")
        if cls.external:
            from urllib.parse import urlsplit
            endpoint = urlsplit(cls.external)
            cls.host = endpoint.hostname
            cls.port = endpoint.port or (443 if endpoint.scheme == "https" else 80)
            cls.https = endpoint.scheme == "https"
            cls.process = None
        else:
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                cls.port = probe.getsockname()[1]
            cls.host, cls.https = "127.0.0.1", False
            env = {**os.environ, "PORT": str(cls.port)}
            cls.process = subprocess.Popen([str(ROOT / "build/banking_server"), str(ROOT / "public")], env=env, stdout=subprocess.DEVNULL)
        for _ in range(100):
            try:
                status, data, _ = cls.request("GET", "/health")
                if status == 200 and data["ok"]:
                    break
            except (OSError, ValueError):
                pass
            time.sleep(0.1)
        else:
            if cls.process:
                cls.process.terminate()
            raise RuntimeError("C server did not become ready")

    @classmethod
    def tearDownClass(cls):
        if cls.process:
            cls.process.terminate()
            cls.process.wait(timeout=10)

    @classmethod
    def request(cls, method, path, data=None, cookie=None, custom=True):
        connection_type = http.client.HTTPSConnection if cls.https else http.client.HTTPConnection
        connection = connection_type(cls.host, cls.port, timeout=90)
        headers = {}
        if cookie:
            headers["Cookie"] = cookie
        if method == "POST" and custom:
            headers["X-Bank-Request"] = "1"
        if data is not None:
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        connection.request(method, path, urlencode(data) if data is not None else None, headers)
        response = connection.getresponse()
        text = response.read().decode()
        new_cookie = response.getheader("Set-Cookie")
        status = response.status
        connection.close()
        return status, json.loads(text) if path.startswith("/api") or path == "/health" else text, new_cookie

    def setUp(self):
        status, data, cookie = self.request("GET", "/api/state")
        self.assertEqual(status, 200)
        self.assertEqual(data["total"], 100000)
        self.cookie = cookie.split(";", 1)[0]

    def post(self, path, data):
        return self.request("POST", path, data, self.cookie)

    def state(self):
        return self.request("GET", "/api/state", cookie=self.cookie)[1]

    def test_01_frontend_and_backend(self):
        self.assertIn("POSIX", self.request("GET", "/health")[1]["backend"])
        self.assertIn('id="transaction-form"', self.request("GET", "/")[1])
        self.assertIn("/api/transaction", self.request("GET", "/app.js")[1])

    def test_02_operations_and_overdraft(self):
        self.post("/api/transaction", dict(operation="deposit", account=101, amount="5000"))
        self.post("/api/transaction", dict(operation="withdraw", account=101, amount="12000"))
        status, result, _ = self.post("/api/transaction", dict(operation="enquiry", account=101))
        self.assertEqual(status, 200)
        self.assertIn("ENQUIRY", result["log"])
        self.assertEqual(result["accounts"][0]["balance"], 43000)
        _, result, _ = self.post("/api/transaction", dict(operation="withdraw", account=102, amount="60000"))
        self.assertIn("REJECTED", result["log"])
        self.assertEqual(result["accounts"][1]["balance"], 50000)
        _, result, _ = self.post("/api/transaction", dict(operation="transfer", account=101, target=102, amount="10000"))
        self.assertEqual([a["balance"] for a in result["accounts"]], [33000, 60000])
        self.assertEqual(result["total"], 93000)

    def test_03_parallel_http_deposits(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as workers:
            jobs = [workers.submit(self.post, "/api/transaction", dict(operation="deposit", account=101, amount="100")) for _ in range(100)]
            self.assertTrue(all(job.result()[0] == 200 for job in jobs))
        self.assertEqual(self.state()["accounts"][0]["balance"], 60000)

    def test_04_parallel_opposing_transfers(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as workers:
            jobs = [workers.submit(self.post, "/api/transaction", dict(operation="transfer", account=101 if i % 2 else 102, target=102 if i % 2 else 101, amount="100")) for i in range(100)]
            self.assertTrue(all(job.result()[0] == 200 for job in jobs))
        self.assertEqual([a["balance"] for a in self.state()["accounts"]], [50000, 50000])

    def test_05_isolated_concurrency_scenarios(self):
        for kind in ["safe", "transfer", "assignment", "race"]:
            with self.subTest(kind=kind):
                status, result, _ = self.post("/api/demo", {"kind": kind})
                self.assertEqual(status, 200)
                if kind == "race":
                    self.assertGreaterEqual(result["actual"], 11000)
                    self.assertLessEqual(result["actual"], 20000)
                else:
                    self.assertAlmostEqual(result["actual"], result["expected"], places=2)
                self.assertEqual(self.state()["total"], 100000)

    def test_06_validation_and_sessions(self):
        invalid = ["-10", "0", "NaN", "inf", "1000001", "1.001", "1xyz", ""]
        for amount in invalid:
            self.assertEqual(self.post("/api/transaction", dict(operation="deposit", account=101, amount=amount))[0], 400)
        self.assertEqual(self.post("/api/transaction", dict(operation="transfer", account=101, target=101, amount=10))[0], 400)
        self.assertEqual(self.post("/api/transaction", {})[0], 400)
        self.assertEqual(self.request("POST", "/api/reset", {}, self.cookie, custom=False)[0], 403)
        self.assertEqual(self.request("GET", "/api/reset", cookie=self.cookie)[0], 405)
        self.post("/api/transaction", dict(operation="deposit", account=101, amount=100))
        # New browser session has an independent ledger.
        self.assertEqual(self.request("GET", "/api/state")[1]["total"], 100000)
        _, result, _ = self.post("/api/reset", {})
        self.assertEqual(result["total"], 100000)


if __name__ == "__main__":
    unittest.main(verbosity=2)
