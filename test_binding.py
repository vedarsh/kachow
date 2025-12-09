# test_binding.py
import time
import struct
import math
from usrl import USRL

def run():
    print("=== USRL PYTHON BINDING TEST ===")
    
    # 1. Initialize System
    sys = USRL(app_name="PyTest", log_level=1) # WARN only
    
    # 2. Config
    TOPIC = "py_wave"
    
    # Publisher: Fast SWMR
    pub = sys.create_publisher(TOPIC, slots=8192, size=64, block=False)
    
    # Subscriber
    sub = sys.create_subscriber(TOPIC, buffer_size=64)
    
    print(f"Connected on topic '{TOPIC}'. Generating Wave...")
    
    # 3. Wave Generator Loop
    t_start = time.time()
    sent_count = 0
    recv_count = 0
    
    try:
        for i in range(100000):
            # Create a binary payload (double: timestamp, double: value)
            val = math.sin(i * 0.1)
            payload = struct.pack("dd", time.time(), val)
            
            # Send
            if pub.send(payload) == 0:
                sent_count += 1
            
            # Recv (Drain all available)
            while True:
                msg = sub.recv()
                if msg:
                    recv_count += 1
                    # Unpack occasionally to verify
                    if recv_count % 10000 == 0:
                        ts, v = struct.unpack("dd", msg)
                        lag_us = (time.time() - ts) * 1e6
                        print(f"RX: {v:.4f} | Lag: {lag_us:.1f} us | Stats: {sub.stats()}")
                else:
                    break
            
            # Artificial delay to simulate 10kHz physics loop
            # time.sleep(0.0001) 
            
    except KeyboardInterrupt:
        pass
        
    dur = time.time() - t_start
    print("\n=== REPORT ===")
    print(f"Time:  {dur:.4f} s")
    print(f"Sent:  {sent_count}")
    print(f"Recv:  {recv_count}")
    print(f"Speed: {recv_count/dur:.0f} msgs/sec")
    
    sys.shutdown()

if __name__ == "__main__":
    run()
