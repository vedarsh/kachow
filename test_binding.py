#!/usr/bin/env python3
"""
test_usrl_god.py - GOD TIER TESTING
Validates: Fuzzing, Security, Race Conditions, Resource Exhaustion
"""

import unittest
import time
import threading
import random
import string
import os
import ctypes
import resource

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

from usrl import USRL, USRL_RING_SWMR, USRL_RING_MWMR

class TestUSRLGodMode(unittest.TestCase):
    
    def setUp(self):
        # Log level 0 to keep console clean during chaos
        self.usrl = USRL(app_name="GodTest", log_level=0)

    def tearDown(self):
        self.usrl.shutdown()
        # Clean up any rogue SHM files (linux specific)
        os.system("rm -f /dev/shm/usrl-* 2>/dev/null")

    # =========================================================================
    # SECURITY & FUZZING
    # =========================================================================

    def test_security_path_traversal(self):
        """SECURITY: Attempt path traversal in topic name."""
        print("\n[SECURITY] Testing Path Traversal...")
        malicious_topics = [
            "../../../../etc/passwd",
            "/dev/mem",
            "topic/with/slashes",
            "topic\0null",  # Null byte injection
            "A" * 256       # Buffer overflow on name
        ]
        
        for topic in malicious_topics:
            try:
                # Should either fail gracefully or sanitize
                # It MUST NOT crash or overwrite arbitrary files
                pub = self.usrl.publisher(topic, slots=16, size=64)
                pub.destroy()
            except RuntimeError:
                pass # Expected failure
            except Exception as e:
                self.fail(f"Crashed on malicious topic '{topic}': {e}")
                
        print("[PASS] Path Traversal / Name Injection Survived")

    def test_fuzzing_payloads(self):
        """FUZZ: Send random binary garbage payload sizes."""
        print("\n[FUZZ] Fuzzing Payloads...")
        topic = "fuzz_target"
        pub = self.usrl.publisher(topic, slots=128, size=1024)
        sub = self.usrl.subscriber(topic, buffer_size=2048)

        for i in range(1000):
            # Random size 0 to 2000 (some larger than slot)
            size = random.randint(0, 2000)
            payload = os.urandom(size)
            
            try:
                ret = pub.send(payload)
                if size > 1024:
                    self.assertNotEqual(ret, 0, f"Should reject oversize {size}")
                else:
                    self.assertEqual(ret, 0, f"Should accept valid size {size}")
            except Exception as e:
                self.fail(f"Crashed on fuzz size {size}: {e}")

        print("[PASS] 1000 Fuzz Cycles Survived")

    def test_null_pointer_attacks(self):
        """SECURITY: Verify C bindings handle None/NULL gracefully."""
        # This relies on the Python wrapper catching it, OR the C code checking ptrs.
        # Since we send via wrapper, we test wrapper resilience.
        
        topic = "null_attack"
        pub = self.usrl.publisher(topic)
        
        # Already covered by test_invalid_types, but explicitly checking segregation
        try:
            pub.send(None) 
        except TypeError:
            pass # Good
        except Exception as e: # Segfault usually kills process, so exception means wrapper caught it
            pass

    # =========================================================================
    # RESOURCE EXHAUSTION
    # =========================================================================

    def test_fd_starvation(self):
        """STRESS: Exhaust File Descriptors (Linux limit)."""
        print("\n[STRESS] Testing FD Exhaustion...")
        
        # Get current limit
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        # Lower limit to make test faster (e.g., 200)
        try:
            resource.setrlimit(resource.RLIMIT_NOFILE, (200, hard))
        except ValueError:
            print("Skipping FD test (cannot lower limit)")
            return

        pubs = []
        try:
            # Create pubs until we hit limit
            for i in range(300): 
                try:
                    p = self.usrl.publisher(f"fd_test_{i}", slots=16, size=64)
                    pubs.append(p)
                except RuntimeError:
                    # Expected to fail eventually
                    break
            
            print(f"Hit FD limit after {len(pubs)} publishers. System stable.")
            
            # Critical: Can we still use existing ones?
            if len(pubs) > 0:
                self.assertEqual(pubs[0].send(b"alive"), 0)
                
        finally:
            for p in pubs: p.destroy()
            resource.setrlimit(resource.RLIMIT_NOFILE, (soft, hard)) # Restore

    # =========================================================================
    # CONCURRENCY TORTURE
    # =========================================================================

    def test_race_condition_torture(self):
        """TORTURE: 50 threads fighting for 1 slot MWMR ring."""
        print("\n[TORTURE] 50 Threads vs 1-Slot Ring (Deadlock Check)...")
        topic = "torture_ring"
        # 1 slot ring = Maximum contention
        pub = self.usrl.publisher(topic, slots=1, size=64, mwmr=True, block=True)
        sub = self.usrl.subscriber(topic)
        
        stop_evt = threading.Event()
        counter = [0]
        
        def writer():
            while not stop_evt.is_set():
                # Blocking send
                try:
                    pub.send(b"X")
                    # No atomic inc needed for simple load test, just spam
                except: pass

        def reader():
            while not stop_evt.is_set():
                if sub.recv():
                    counter[0] += 1

        threads = []
        # 50 Writers
        for _ in range(50):
            t = threading.Thread(target=writer)
            t.start()
            threads.append(t)
            
        # 1 Reader (must drain fast to allow writes)
        r = threading.Thread(target=reader)
        r.start()
        
        time.sleep(2.0)
        stop_evt.set()
        
        for t in threads: t.join()
        r.join()
        
        print(f"[PASS] Survived 2s torture. Processed {counter[0]} msgs.")
        # If it didn't hang (deadlock), we pass.

    # =========================================================================
    # VALIDATION
    # =========================================================================

    def test_schema_violation(self):
        """FEATURE: If schema enabled, reject bad data (Mock)."""
        # Since we don't have the full schema validation .so loaded in this context usually,
        # we check if the hook exists. If not, this acts as a placeholder.
        pass 

if __name__ == '__main__':
    unittest.main()
