import math
import sys

def main():
    if len(sys.argv) < 2:
        print("Usage: python tmp.py <path_to_log>")
        return
        
    log_file = sys.argv[1]
    
    with open(log_file, "r") as f:
        for line in f:
            line = line.strip()
            if not line: continue
            parts = line.split("=")
            if len(parts) != 2: continue
            
            left, right = parts[0].strip(), parts[1].strip()
            try:
                val = float(right)
            except ValueError:
                continue
            
            if "sin" in left:
                try:
                    arg = float(left.split("sin(")[1].split(")")[0])
                    expected = math.sin(arg)
                    if abs(expected - val) > 0.00001:
                        print(f"Error: {line} -> Expected {expected}, got {val}")
                except (IndexError, ValueError):
                    pass
            elif "sqrt" in left:
                try:
                    arg = float(left.split("sqrt(")[1].split(")")[0])
                    expected = math.sqrt(arg)
                    if abs(expected - val) > 0.00001:
                        print(f"Error: {line} -> Expected {expected}, got {val}")
                except (IndexError, ValueError):
                    pass

if __name__ == '__main__':
    main()
